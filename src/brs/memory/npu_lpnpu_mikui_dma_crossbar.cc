#include "brs/memory/npu_lpnpu_mikui_dma_crossbar.hh"

#include <algorithm>

namespace gem5
{
namespace brs
{

NpuLpnpuMikuiDmaCrossbar::NpuLpnpuMikuiDmaCrossbar()
  : NpuLpnpuMikuiDmaCrossbar(Config{})
{}

NpuLpnpuMikuiDmaCrossbar::NpuLpnpuMikuiDmaCrossbar(Config config)
  : config(config)
{
    reset();
}

void
NpuLpnpuMikuiDmaCrossbar::reset()
{
    currentState = State::Idle;
    masterRunning = {};
    transactionOverLatched = false;
    dbusPending = false;
    pendingDbus = {};
    dbusInflight = false;
    outputReg = {};
}

uint32_t
NpuLpnpuMikuiDmaCrossbar::bankStart(uint8_t bank) const
{
    const std::array<uint8_t, 3> split{{config.sramB, config.sramC,
                                        config.sramD}};
    return config.splitBase + static_cast<uint32_t>(split.at(bank)) * 0x2000;
}

uint32_t
NpuLpnpuMikuiDmaCrossbar::bankEnd(uint8_t bank) const
{
    if (bank == 2) {
        return config.splitEnd - 1;
    }
    return bankStart(bank + 1) - 1;
}

int
NpuLpnpuMikuiDmaCrossbar::decodeBank(uint32_t address) const
{
    for (uint8_t bank = 0; bank < 3; ++bank) {
        if (address >= bankStart(bank) && address <= bankEnd(bank)) {
            return bank;
        }
    }
    return -1;
}

std::array<uint8_t, Sram128Bytes>
NpuLpnpuMikuiDmaCrossbar::readLine(
    uint8_t bank, uint32_t address) const
{
    std::array<uint8_t, Sram128Bytes> data{};
    const uint32_t base = address & ~uint32_t{0x0f};
    for (uint8_t byte = 0; byte < Sram128Bytes; ++byte) {
        const auto found = memory[bank].find(base + byte);
        if (found != memory[bank].end()) {
            data[byte] = found->second;
        }
    }
    return data;
}

void
NpuLpnpuMikuiDmaCrossbar::accessBank(
    uint8_t bank, const Sram128Request &request)
{
    if (!request.valid) {
        return;
    }
    const uint32_t base = request.address & ~uint32_t{0x0f};
    if (request.isWrite()) {
        for (uint8_t byte = 0; byte < Sram128Bytes; ++byte) {
            if (request.writeStrobe & (uint16_t{1} << byte)) {
                memory[bank][base + byte] = request.writeData[byte];
            }
        }
    }
}

void
NpuLpnpuMikuiDmaCrossbar::clock(
    const NpuLpnpuMikuiCrossbarInputs &inputs)
{
    NpuLpnpuMikuiDmaCrossbarOutputs nextOutput;
    std::array<Sram128Request, 3> bankRequests{};
    std::array<int8_t, 3> winner{{-1, -1, -1}};

    const bool transactionStart =
        inputs.crossbarStart[0] || inputs.crossbarStart[1];
    const bool dbusInflightBeforeEdge = dbusInflight;

    if (currentState == State::Active) {
        for (uint8_t master = 0; master < 2; ++master) {
            if (!inputs.masters[master].valid) {
                continue;
            }
            const int bank = decodeBank(inputs.masters[master].address);
            if (bank < 0) {
                if (inputs.masters[master].address < config.splitBase ||
                    inputs.masters[master].address > config.splitEnd) {
                    nextOutput.addressError = true;
                }
                continue;
            }
            if (winner[bank] >= 0) {
                nextOutput.sameBankCollision = true;
                const uint8_t displaced = static_cast<uint8_t>(winner[bank]);
                nextOutput.acceptedMaster[displaced] = false;
                nextOutput.droppedMaster[displaced] = true;
            }
            winner[bank] = static_cast<int8_t>(master);
            nextOutput.acceptedMaster[master] = true;
            bankRequests[bank] = inputs.masters[master];
        }
    }

    Sram128Request effectiveDbus = inputs.dbus.valid ? inputs.dbus : pendingDbus;
    const bool hasEffectiveDbus = inputs.dbus.valid || dbusPending;
    bool dbusAccepted = false;
    int dbusBank = -1;
    if (hasEffectiveDbus) {
        dbusBank = decodeBank(effectiveDbus.address);
        if (dbusBank < 0) {
            // crossbar_mi_full leaves its DBUS selector at slave 0 for the
            // pre-stack decode hole and the exact SRAM_SPLIT_END address.
            // Addresses outside the inclusive split window still drive
            // slave 0, but additionally assert xbar_error.
            dbusBank = 0;
            if (effectiveDbus.address < config.splitBase ||
                effectiveDbus.address > config.splitEnd) {
                nextOutput.addressError = true;
            }
        }
        if (currentState == State::Idle || winner[dbusBank] < 0) {
            dbusAccepted = true;
            bankRequests[dbusBank] = effectiveDbus;
        }
    }

    for (uint8_t bank = 0; bank < 3; ++bank) {
        nextOutput.bankRequest[bank] = bankRequests[bank].valid;
        if (!bankRequests[bank].valid) {
            continue;
        }
        const auto readData = readLine(bank, bankRequests[bank].address);
        accessBank(bank, bankRequests[bank]);
        // The request mux is last-assignment-wins (VEU over SAU), while the
        // RTL response selectors are delayed independently for both masters.
        // Consequently the overwritten SAU request can still observe the
        // winning bank's registered ack/data one edge later.
        if (currentState == State::Active && winner[bank] >= 0) {
            for (uint8_t master = 0; master < 2; ++master) {
                if (!inputs.masters[master].valid ||
                    decodeBank(inputs.masters[master].address) != bank) {
                    continue;
                }
                nextOutput.masters[master].valid = true;
                nextOutput.masters[master].readData = readData;
            }
        }
        if (dbusAccepted && dbusBank == bank) {
            nextOutput.dbus.valid = true;
            nextOutput.dbus.readData = readData;
        }
    }

    if (inputs.dbus.valid && !dbusAccepted) {
        dbusPending = true;
        pendingDbus = inputs.dbus;
    } else if (!inputs.dbus.valid && dbusPending && dbusAccepted) {
        dbusPending = false;
        pendingDbus = {};
    }
    dbusInflight = dbusPending;

    const bool transactionOverBeforeEdge =
        currentState == State::Active &&
        !masterRunning[0] && !masterRunning[1];
    const bool leaveActive = currentState == State::Active &&
        (transactionOverBeforeEdge || transactionOverLatched) &&
        !dbusInflightBeforeEdge;

    if (currentState == State::Idle) {
        for (uint8_t master = 0; master < 2; ++master) {
            masterRunning[master] = inputs.crossbarStart[master];
        }
        currentState = transactionStart ? State::Active : State::Idle;
        transactionOverLatched = false;
    } else {
        for (uint8_t master = 0; master < 2; ++master) {
            if (inputs.crossbarStart[master]) {
                masterRunning[master] = true;
            } else if (inputs.crossbarDone[master]) {
                masterRunning[master] = false;
            }
        }
        transactionOverLatched = transactionOverLatched ||
            transactionOverBeforeEdge;
        if (leaveActive) {
            currentState = State::Idle;
        }
    }

    outputReg = nextOutput;
}

void
NpuLpnpuMikuiDmaCrossbar::writeByte(uint32_t address, uint8_t value)
{
    const int bank = decodeBank(address);
    if (bank >= 0) {
        memory[static_cast<uint8_t>(bank)][address] = value;
    }
}

uint8_t
NpuLpnpuMikuiDmaCrossbar::readByte(uint32_t address) const
{
    const int bank = decodeBank(address);
    if (bank < 0) {
        return 0;
    }
    const auto &storage = memory[static_cast<uint8_t>(bank)];
    const auto found = storage.find(address);
    return found == storage.end() ? 0 : found->second;
}

} // namespace brs
} // namespace gem5

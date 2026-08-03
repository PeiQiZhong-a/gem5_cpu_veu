#include "brs/memory/npu_lpnpu_mikui_crossbar.hh"

namespace gem5
{
namespace brs
{

void
NpuLpnpuMikuiCrossbar::reset()
{
    currentState = State::Idle;
    busBusyReg = false;
    slaveRequestRegs = {};
    slaveAckRegs = {};
    slaveReadDataRegs = {};
    selectStage1 = {};
    selectStage2 = {};
    selectValidStage1 = {};
    selectValidStage2 = {};
    masterReadDataRegs = {};
    dbusOutputReg = {};
    outputReg = {};
}

int
NpuLpnpuMikuiCrossbar::decodeMasterBank(uint32_t address) const
{
    if (address >= Bank0Base && address <= Bank0End) {
        return 0;
    }
    return address >= Bank1Base && address <= Bank1End ? 1 : -1;
}

uint8_t
NpuLpnpuMikuiCrossbar::decodeDbusBank(uint32_t address) const
{
    // This is deliberately not a validity check: crossbar_mi.sv sends every
    // DBUS address outside bank 0 to bank 1.
    return address >= Bank0Base && address <= Bank0End ? 0 : 1;
}

std::array<uint8_t, Sram128Bytes>
NpuLpnpuMikuiCrossbar::readLine(
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
NpuLpnpuMikuiCrossbar::accessBank(
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
NpuLpnpuMikuiCrossbar::clock(
    const NpuLpnpuMikuiCrossbarInputs &inputs)
{
    // The external tcdm banks and crossbar registers sample the same old
    // slave pins on this edge. Build their next registered read data first.
    auto nextSlaveReadData = slaveReadDataRegs;
    for (uint8_t bank = 0; bank < 2; ++bank) {
        if (slaveRequestRegs[bank].valid) {
            accessBank(bank, slaveRequestRegs[bank]);
            nextSlaveReadData[bank] =
                readLine(bank, slaveRequestRegs[bank].address);
        }
    }

    std::array<uint8_t, 2> selectedBank{};
    std::array<bool, 2> selectedValid{};
    for (uint8_t master = 0; master < 2; ++master) {
        const int bank = inputs.masters[master].valid ?
            decodeMasterBank(inputs.masters[master].address) : -1;
        selectedValid[master] = bank >= 0;
        selectedBank[master] = bank >= 0 ? static_cast<uint8_t>(bank) : 0;
    }

    const bool transactionStart =
        inputs.crossbarStart[0] || inputs.crossbarStart[1];
    const bool transactionDone =
        inputs.crossbarDone[0] || inputs.crossbarDone[1];
    State nextState = currentState;
    switch (currentState) {
      case State::Idle:
        nextState = transactionStart ? State::Active :
            (inputs.dbus.valid ? State::RvActive : State::Idle);
        break;
      case State::Active:
        nextState = transactionDone ? State::Idle : State::Active;
        break;
      case State::RvActive:
        // crossbar_mi.sv has no DBUS-idle transition.
        nextState = transactionStart ? State::Active : State::RvActive;
        break;
    }

    auto nextSlaveRequests = slaveRequestRegs;
    auto nextMasterReadData = masterReadDataRegs;
    bool nextBusBusy = busBusyReg;
    Sram128Response nextDbus = dbusOutputReg;
    NpuLpnpuMikuiCrossbarOutputs nextOutput;

    switch (currentState) {
      case State::Idle:
        nextBusBusy = false;
        nextSlaveRequests = {};
        nextMasterReadData = {};
        break;

      case State::Active: {
        nextBusBusy = true;
        std::array<int8_t, 2> winner{{-1, -1}};
        // RTL loop order is SAU (0), then VEU (1). Later nonblocking
        // assignments to one slave win, so VEU overwrites SAU on collision.
        for (uint8_t master = 0; master < 2; ++master) {
            if (!selectedValid[master]) {
                continue;
            }
            const uint8_t bank = selectedBank[master];
            if (winner[bank] >= 0) {
                nextOutput.sameBankCollision = true;
                const uint8_t displaced =
                    static_cast<uint8_t>(winner[bank]);
                nextOutput.acceptedMaster[displaced] = false;
                nextOutput.droppedMaster[displaced] = true;
            }
            winner[bank] = static_cast<int8_t>(master);
            nextOutput.acceptedMaster[master] = true;
            nextSlaveRequests[bank] = inputs.masters[master];
        }
        for (uint8_t master = 0; master < 2; ++master) {
            if (!selectValidStage2[master]) {
                continue;
            }
            const uint8_t bank = selectStage2[master];
            nextMasterReadData[master] = slaveReadDataRegs[bank];
            nextOutput.masters[master].valid = slaveAckRegs[bank];
            nextOutput.masters[master].readData =
                slaveReadDataRegs[bank];
        }
        break;
      }

      case State::RvActive: {
        nextBusBusy = true;
        const uint8_t bank = decodeDbusBank(inputs.dbus.address);
        nextSlaveRequests[bank] = inputs.dbus;
        nextDbus.valid = busBusyReg ? slaveAckRegs[bank] : false;
        nextDbus.readData = busBusyReg ? slaveReadDataRegs[bank] :
            std::array<uint8_t, Sram128Bytes>{};
        break;
      }
    }

    for (uint8_t master = 0; master < 2; ++master) {
        if (!nextOutput.masters[master].valid) {
            nextOutput.masters[master].readData =
                nextMasterReadData[master];
        }
    }
    nextOutput.dbus = nextDbus;
    for (uint8_t bank = 0; bank < 2; ++bank) {
        nextOutput.bankRequest[bank] = nextSlaveRequests[bank].valid;
    }

    // All of these are independent always_ff blocks in the RTL and therefore
    // sample old values atomically at this edge.
    selectStage2 = selectStage1;
    selectStage1 = selectedBank;
    selectValidStage2 = selectValidStage1;
    selectValidStage1 = selectedValid;
    for (uint8_t bank = 0; bank < 2; ++bank) {
        slaveAckRegs[bank] = slaveRequestRegs[bank].valid;
    }
    slaveReadDataRegs = nextSlaveReadData;
    slaveRequestRegs = nextSlaveRequests;
    masterReadDataRegs = nextMasterReadData;
    busBusyReg = nextBusBusy;
    dbusOutputReg = nextDbus;
    currentState = nextState;
    outputReg = nextOutput;
}

void
NpuLpnpuMikuiCrossbar::writeByte(uint32_t address, uint8_t value)
{
    const int bank = decodeMasterBank(address);
    if (bank >= 0) {
        memory[static_cast<uint8_t>(bank)][address] = value;
    }
}

uint8_t
NpuLpnpuMikuiCrossbar::readByte(uint32_t address) const
{
    const int bank = decodeMasterBank(address);
    if (bank < 0) {
        return 0;
    }
    const auto &storage = memory[static_cast<uint8_t>(bank)];
    const auto found = storage.find(address);
    return found == storage.end() ? 0 : found->second;
}

} // namespace brs
} // namespace gem5

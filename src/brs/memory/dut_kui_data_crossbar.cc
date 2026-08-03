#include "brs/memory/dut_kui_data_crossbar.hh"

#include <algorithm>

namespace gem5
{
namespace brs
{

DutKuiDataCrossbar::DutKuiDataCrossbar()
  : DutKuiDataCrossbar(Config{})
{}

DutKuiDataCrossbar::DutKuiDataCrossbar(Config config) : config(config)
{
    reset();
}

void
DutKuiDataCrossbar::reset()
{
    currentState = State::Idle;
    outputReg = {};
    dbusLevelIssued = false;
    pendingResponses.clear();
}

void
DutKuiDataCrossbar::clearMemory()
{
    memory.clear();
}

DutKuiDataCrossbarOutputs
DutKuiDataCrossbar::evaluate() const
{
    DutKuiDataCrossbarOutputs output = outputReg;
    return output;
}

int
DutKuiDataCrossbar::decodeBank(uint32_t address) const
{
    if (address < config.dataBase) {
        return -1;
    }
    const uint64_t offset = address - config.dataBase;
    const uint64_t mappedSize =
        static_cast<uint64_t>(config.bankSize) * config.portCount;
    return offset < mappedSize ?
        static_cast<int>(offset / config.bankSize) : -1;
}

bool
DutKuiDataCrossbar::isRealBank(uint32_t bank) const
{
    return bank < config.realBankCount;
}

Sram256Response
DutKuiDataCrossbar::access(
    uint32_t bank, const Sram256Request &request)
{
    Sram256Response response;
    response.valid = true;
    const uint32_t base = request.address & ~uint32_t{0x1f};
    if (!isRealBank(bank)) {
        return response;
    }

    if (request.isWrite()) {
        for (uint32_t byte = 0; byte < Sram256Bytes; ++byte) {
            if (request.writeStrobe & (uint32_t{1} << byte)) {
                memory[base + byte] = request.writeData[byte];
            }
        }
    } else {
        for (uint32_t byte = 0; byte < Sram256Bytes; ++byte) {
            const auto found = memory.find(base + byte);
            if (found != memory.end()) {
                response.readData[byte] = found->second;
            }
        }
    }
    return response;
}

void
DutKuiDataCrossbar::advanceResponses()
{
    for (auto &pending : pendingResponses) {
        if (pending.remainingCycles > 0) {
            --pending.remainingCycles;
        }
    }
    while (!pendingResponses.empty() &&
           pendingResponses.front().remainingCycles == 0) {
        const PendingResponse pending = pendingResponses.front();
        pendingResponses.pop_front();
        if (pending.dbus) {
            outputReg.dbus = pending.response;
        } else {
            outputReg.masters[pending.master] = pending.response;
        }
    }
}

void
DutKuiDataCrossbar::clock(const DutKuiDataCrossbarInputs &inputs)
{
    outputReg = {};
    advanceResponses();

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
        nextState = transactionStart ? State::Active :
            (!inputs.dbus.valid ? State::Idle : State::RvActive);
        break;
    }

    if (!inputs.dbus.valid) {
        dbusLevelIssued = false;
    }

    if (currentState == State::RvActive && inputs.dbus.valid &&
        !dbusLevelIssued) {
        const int bank = decodeBank(inputs.dbus.address);
        outputReg.acceptedDbus = true;
        dbusLevelIssued = true;
        if (bank < 0) {
            outputReg.dbus.valid = true;
        } else {
            outputReg.bankRequest[bank] = true;
            pendingResponses.push_back({
                config.dbusResponseLatency, true, 0,
                access(static_cast<uint32_t>(bank), inputs.dbus)});
        }
    } else if (currentState == State::Active) {
        // crossbar_mi iterates master 0 (SAU) then master 1 (VEU).
        // Nonblocking writes to the same slave mean VEU wins a collision.
        std::array<int8_t, 4> winner;
        winner.fill(-1);
        for (uint8_t master = 0; master < inputs.masters.size(); ++master) {
            const auto &request = inputs.masters[master];
            const int bank = request.valid ? decodeBank(request.address) : -1;
            if (bank >= 0) {
                if (winner[bank] >= 0) {
                    outputReg.sameBankCollision = true;
                    outputReg.droppedMaster[
                        static_cast<uint8_t>(winner[bank])] = true;
                }
                winner[bank] = static_cast<int8_t>(master);
            }
        }
        for (uint32_t bank = 0; bank < winner.size(); ++bank) {
            if (winner[bank] < 0) {
                continue;
            }
            const uint8_t master = static_cast<uint8_t>(winner[bank]);
            outputReg.bankRequest[bank] = true;
            outputReg.acceptedMaster[master] = true;
            pendingResponses.push_back({
                config.masterResponseLatency, false, master,
                access(bank, inputs.masters[master])});
        }
    }

    currentState = nextState;
}

void
DutKuiDataCrossbar::writeByte(uint32_t address, uint8_t value)
{
    const int bank = decodeBank(address);
    if (bank >= 0 && isRealBank(static_cast<uint32_t>(bank))) {
        memory[address] = value;
    }
}

uint8_t
DutKuiDataCrossbar::readByte(uint32_t address) const
{
    const int bank = decodeBank(address);
    if (bank < 0 || !isRealBank(static_cast<uint32_t>(bank))) {
        return 0;
    }
    const auto found = memory.find(address);
    return found == memory.end() ? 0 : found->second;
}

void
DutKuiDataCrossbar::writeWord(uint32_t address, uint32_t value)
{
    for (uint32_t byte = 0; byte < 4; ++byte) {
        writeByte(address + byte,
                  static_cast<uint8_t>(value >> (byte * 8)));
    }
}

uint32_t
DutKuiDataCrossbar::readWord(uint32_t address) const
{
    uint32_t value = 0;
    for (uint32_t byte = 0; byte < 4; ++byte) {
        value |= static_cast<uint32_t>(readByte(address + byte)) <<
            (byte * 8);
    }
    return value;
}

} // namespace brs
} // namespace gem5

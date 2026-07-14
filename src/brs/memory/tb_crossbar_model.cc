#include "brs/memory/tb_crossbar_model.hh"

#include <algorithm>

namespace gem5
{
namespace brs
{

TbCrossbarModel::TbCrossbarModel() : TbCrossbarModel(Config{})
{}

TbCrossbarModel::TbCrossbarModel(Config config) : config(config)
{
    reset();
}

void
TbCrossbarModel::reset()
{
    lockActive = false;
    normalBusy = false;
    ibusPending = {};
    dbusPending = {};
    delayedResponses.clear();
    veuReadDataPins = {};
}

bool
TbCrossbarModel::mapped(uint32_t address) const
{
    const bool inst = address >= config.instBase &&
        address - config.instBase < config.instSize;
    const bool data = address >= config.dataBase &&
        address - config.dataBase < config.dataSize;
    return inst || data || mappedToUart(address);
}

bool
TbCrossbarModel::backedBySram(uint32_t address) const
{
    const bool inst = address >= config.instBase &&
        address - config.instBase < config.instSize;
    const bool data = address >= config.dataBase &&
        address - config.dataBase < config.dataStorageSize;
    return inst || data;
}

bool
TbCrossbarModel::mappedToUart(uint32_t address) const
{
    return address >= config.uartBase &&
        address - config.uartBase < config.uartSize;
}

TbBusMaster
TbCrossbarModel::arbitrate(const TbCrossbarInputs &inputs) const
{
    if (lockActive) {
        return inputs.veu.valid && mapped(inputs.veu.address) ?
            TbBusMaster::Veu : TbBusMaster::None;
    }
    if (normalBusy) {
        return TbBusMaster::None;
    }
    if (inputs.veu.valid && mapped(inputs.veu.address)) {
        return TbBusMaster::Veu;
    }
    if (ibusPending.valid && mapped(ibusPending.request.address)) {
        return TbBusMaster::IBus;
    }
    if (dbusPending.valid && mapped(dbusPending.request.address)) {
        return TbBusMaster::DBus;
    }
    return TbBusMaster::None;
}

TbBusRequest
TbCrossbarModel::selectedRequest(
    TbBusMaster master, const TbCrossbarInputs &inputs) const
{
    switch (master) {
      case TbBusMaster::IBus: return ibusPending.request;
      case TbBusMaster::DBus: return dbusPending.request;
      case TbBusMaster::Veu: return inputs.veu;
      default: return {};
    }
}

TbBusResponse
TbCrossbarModel::access(const TbBusRequest &request)
{
    TbBusResponse response;
    response.valid = request.read;
    const uint32_t base = request.address & ~uint32_t{0x0f};

    if (request.read) {
        if (mappedToUart(request.address)) {
            const uint32_t uartData = (request.address & 0xf) == 0x8 ? 1 : 0;
            response.readData.fill(uartData);
        } else if (backedBySram(request.address)) {
            for (unsigned word = 0; word < 4; ++word) {
                response.readData[word] = readWord(base + word * 4);
            }
        }
    }

    if (request.write && backedBySram(request.address)) {
        for (unsigned byte = 0; byte < 16; ++byte) {
            if (request.writeStrobe & (uint16_t{1} << byte)) {
                const unsigned word = byte / 4;
                const unsigned shift = (byte % 4) * 8;
                writeByte(base + byte,
                    static_cast<uint8_t>(request.writeData[word] >> shift));
            }
        }
    }
    return response;
}

uint32_t
TbCrossbarModel::responseDelay(TbBusMaster master) const
{
    // One cycle is consumed by the synchronous SRAM read itself. The RTL then
    // applies the configured response pipeline after the slave return.
    switch (master) {
      case TbBusMaster::IBus: return 1 + config.ibusResponseDelay;
      case TbBusMaster::DBus: return 1 + config.dbusResponseDelay;
      case TbBusMaster::Veu: return 1 + config.veuPipelineStages;
      default: return 0;
    }
}

void
TbCrossbarModel::deliverResponses(TbCrossbarOutputs &outputs)
{
    for (auto &entry : delayedResponses) {
        if (entry.remainingCycles > 0) {
            --entry.remainingCycles;
        }
    }

    while (!delayedResponses.empty() &&
           delayedResponses.front().remainingCycles == 0) {
        const auto entry = delayedResponses.front();
        delayedResponses.pop_front();
        switch (entry.master) {
          case TbBusMaster::IBus: outputs.ibus = entry.response; break;
          case TbBusMaster::DBus: outputs.dbus = entry.response; break;
          case TbBusMaster::Veu: outputs.veu = entry.response; break;
          default: break;
        }
    }
}

TbCrossbarOutputs
TbCrossbarModel::clock(const TbCrossbarInputs &inputs)
{
    TbCrossbarOutputs outputs;
    // The RTL has no VEU read-valid output. Its registered read-data pins
    // retain the last completed value between responses.
    outputs.veu.readData = veuReadDataPins;
    deliverResponses(outputs);
    if (outputs.veu.valid) {
        veuReadDataPins = outputs.veu.readData;
    }

    // This mirrors the nonblocking lock register update: arbitration in this
    // call observes the old lock state; the new state is visible next cycle.
    const bool nextLock = inputs.lockStart ? true :
        (inputs.lockFinish ? false : lockActive);

    // Arbitration observes the registered pending state from before this
    // edge. A CPU pulse captured on this edge cannot be granted until the
    // following cycle, matching the nonblocking assignments in crossbar.sv.
    const TbBusMaster grant = arbitrate(inputs);
    outputs.grantedMaster = grant;

    PendingRequest nextIbusPending = ibusPending;
    PendingRequest nextDbusPending = dbusPending;
    if (inputs.ibus.valid && !ibusPending.valid) {
        nextIbusPending = {true, inputs.ibus};
    }
    if (inputs.dbus.valid && !dbusPending.valid) {
        nextDbusPending = {true, inputs.dbus};
    }

    if (grant != TbBusMaster::None) {
        const TbBusRequest request = selectedRequest(grant, inputs);
        TbBusResponse response = access(request);

        // In normal mode crossbar.sv returns an SRAM completion for both
        // reads and writes. Lock mode suppresses the VEU return on writes.
        if (!lockActive) {
            response.valid = true;
        }

        if (grant == TbBusMaster::IBus) {
            nextIbusPending.valid = false;
        } else if (grant == TbBusMaster::DBus) {
            nextDbusPending.valid = false;
        }

        if (response.valid) {
            delayedResponses.push_back(
                {responseDelay(grant), grant, response});
        }
    }

    ibusPending = nextIbusPending;
    dbusPending = nextDbusPending;

    // For the synchronous SRAM slaves, xbar_busy_q is asserted on an issue
    // edge and cleared on the next edge. Response pipelines run independently,
    // so normal arbitration resumes before the delayed response is visible.
    normalBusy = !lockActive && grant != TbBusMaster::None;

    lockActive = nextLock;
    outputs.lockActive = lockActive;
    return outputs;
}

void
TbCrossbarModel::writeByte(uint32_t address, uint8_t value)
{
    memory[address] = value;
}

uint8_t
TbCrossbarModel::readByte(uint32_t address) const
{
    const auto it = memory.find(address);
    return it == memory.end() ? 0 : it->second;
}

void
TbCrossbarModel::writeWord(uint32_t address, uint32_t value)
{
    for (unsigned byte = 0; byte < 4; ++byte) {
        writeByte(address + byte,
            static_cast<uint8_t>(value >> (byte * 8)));
    }
}

uint32_t
TbCrossbarModel::readWord(uint32_t address) const
{
    uint32_t value = 0;
    for (unsigned byte = 0; byte < 4; ++byte) {
        value |= static_cast<uint32_t>(readByte(address + byte)) <<
            (byte * 8);
    }
    return value;
}

} // namespace brs
} // namespace gem5

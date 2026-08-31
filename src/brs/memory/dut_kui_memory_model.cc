#include "brs/memory/dut_kui_memory_model.hh"

namespace gem5
{
namespace brs
{

DutKuiMemoryModel::DutKuiMemoryModel() : DutKuiMemoryModel(Config{})
{}

DutKuiMemoryModel::DutKuiMemoryModel(Config config)
  : config(config),
    dataCrossbar(DutKuiDataCrossbar::Config{
        config.dataBase,
        config.bankSize,
        config.bankCount,
        config.realBankCount,
        config.crossbarMasterResponseLatency,
        config.crossbarDbusResponseLatency})
{
    reset();
}

void
DutKuiMemoryModel::reset()
{
    ibusOutstanding = false;
    dbusOutstanding = false;
    veuOutstanding = 0;
    veuAcceptedThisCycle = false;
    ibusAcceptedThisCycle = false;
    dbusAcceptedThisCycle = false;
    acceptedIbus = {};
    acceptedDbus = {};
    acceptedVeu = {};
    pendingVeuRequests.clear();
    issuedVeuRequests.clear();
    issuedSauHalfOffsets.clear();
    previousVeuLockActive = false;
    ibusResponses.clear();
    dbusConverter.reset();
    dataCrossbar.reset();
    visibleOutputs = {};
}

bool
DutKuiMemoryModel::instructionMapped(uint32_t address) const
{
    return address >= config.instBase &&
        address - config.instBase < config.instSize;
}

bool
DutKuiMemoryModel::dataMapped(uint32_t address) const
{
    return dataCrossbar.decodeBank(address) >= 0;
}

bool
DutKuiMemoryModel::acceptIbus(const DutKuiIbusRequest &request)
{
    if (ibusOutstanding || ibusAcceptedThisCycle) {
        return false;
    }
    acceptedIbus = request;
    ibusAcceptedThisCycle = true;
    ibusOutstanding = true;
    return true;
}

bool
DutKuiMemoryModel::acceptDbus(
    const DutKuiDbusRequest &request, bool veuLockActive)
{
    if (dbusOutstanding || dbusAcceptedThisCycle) {
        return false;
    }
    acceptedDbus = request;
    dbusAcceptedThisCycle = true;
    dbusOutstanding = true;
    (void)veuLockActive;
    return true;
}

bool
DutKuiMemoryModel::acceptVeu(const DutKuiVeuRequest &request)
{
    if (veuAcceptedThisCycle ||
        veuOutstanding >= config.maxVeuOutstanding) {
        return false;
    }
    acceptedVeu = request;
    veuAcceptedThisCycle = true;
    ++veuOutstanding;
    return true;
}

DutKuiMemoryOutputs
DutKuiMemoryModel::advance(
    bool veuLockActive, const SauMemoryOutput &sau)
{
    DutKuiMemoryOutputs outputs;

    auto advance = [](auto &queue, auto &output) {
        for (auto &entry : queue) {
            if (entry.remainingCycles > 0) {
                --entry.remainingCycles;
            }
        }
        if (!queue.empty() && queue.front().remainingCycles == 0) {
            output = queue.front().response;
            queue.pop_front();
        }
    };

    advance(ibusResponses, outputs.ibus);
    if (outputs.ibus.valid) {
        ibusOutstanding = false;
    }

    if (ibusAcceptedThisCycle) {
        DutKuiIbusResponse response;
        response.valid = true;
        // dut_kui.sv presents the CPU's original IBus address at its CPU
        // boundary, then aligns the SRAM-side address to the 128-bit line.
        const uint32_t base = acceptedIbus.address & ~uint32_t{0x0f};
        if (instructionMapped(base)) {
            for (uint32_t word = 0; word < 4; ++word) {
                response.readData[word] = readWord(base + word * 4);
            }
        }
        ibusResponses.push_back({config.ibusResponseLatency, response});
    }

    if (veuAcceptedThisCycle) {
        pendingVeuRequests.push_back(acceptedVeu);
    }

    const SramConverter32To256Output converterBefore =
        dbusConverter.evaluate();
    const DutKuiDataCrossbarOutputs crossbarBefore =
        dataCrossbar.evaluate();

    Sram32Request dbusInput;
    if (dbusAcceptedThisCycle && dbusConverter.canAccept()) {
        dbusInput.valid = true;
        dbusInput.address = acceptedDbus.address;
        dbusInput.writeStrobe = acceptedDbus.writeStrobe;
        dbusInput.writeData = acceptedDbus.writeData;
        dbusAcceptedThisCycle = false;
        acceptedDbus = {};
    }
    dbusConverter.clock(dbusInput, crossbarBefore.dbus);

    DutKuiDataCrossbarInputs crossbarInputs;
    crossbarInputs.dbus = converterBefore.sram;
    constexpr uint8_t SauUpperHalfOffset = Sram128Bytes;
    const uint8_t sauHalfOffset =
        (sau.request.address & Sram128Bytes) ? SauUpperHalfOffset : 0;
    Sram256Request &legacySau = crossbarInputs.masters[
        static_cast<uint8_t>(DutKuiDataMaster::Sau)];
    legacySau.valid = sau.request.valid;
    legacySau.address = sau.request.address;
    legacySau.writeStrobe =
        static_cast<uint32_t>(sau.request.writeStrobe) << sauHalfOffset;
    for (uint8_t byte = 0; byte < Sram128Bytes; ++byte) {
        legacySau.writeData[sauHalfOffset + byte] =
            sau.request.writeData[byte];
    }
    crossbarInputs.crossbarStart[
        static_cast<uint8_t>(DutKuiDataMaster::Sau)] = sau.crossbarStart;
    crossbarInputs.crossbarDone[
        static_cast<uint8_t>(DutKuiDataMaster::Sau)] = sau.crossbarDone;

    if (!pendingVeuRequests.empty()) {
        const DutKuiVeuRequest &request = pendingVeuRequests.front();
        const uint8_t halfOffset =
            (request.address & Sram128Bytes) ? Sram128Bytes : 0;
        Sram256Request &veu = crossbarInputs.masters[
            static_cast<uint8_t>(DutKuiDataMaster::Veu)];
        veu.valid = true;
        veu.address = request.address;
        veu.writeStrobe = request.isWrite ?
            request.writeStrobe << halfOffset : 0;
        for (uint8_t byte = 0; byte < Sram128Bytes; ++byte) {
            veu.writeData[halfOffset + byte] = request.data[byte];
        }
    }
    crossbarInputs.crossbarStart[
        static_cast<uint8_t>(DutKuiDataMaster::Veu)] =
        veuLockActive && !previousVeuLockActive;
    crossbarInputs.crossbarDone[
        static_cast<uint8_t>(DutKuiDataMaster::Veu)] =
        !veuLockActive && previousVeuLockActive;

    dataCrossbar.clock(crossbarInputs);
    const DutKuiDataCrossbarOutputs crossbarAfter =
        dataCrossbar.evaluate();
    // These are the response pins made visible by this edge. The old
    // crossbarBefore values were sampled as inputs above and must not be
    // returned again, or the outer tick engine would insert an implicit
    // extra cycle.
    const uint8_t sauMaster =
        static_cast<uint8_t>(DutKuiDataMaster::Sau);
    if (crossbarAfter.acceptedMaster[sauMaster]) {
        issuedSauHalfOffsets.push_back(sauHalfOffset);
    }
    const Sram256Response &legacySauResponse =
        crossbarAfter.masters[sauMaster];
    if (legacySauResponse.valid && !issuedSauHalfOffsets.empty()) {
        outputs.sau.valid = true;
        const uint8_t completedHalf = issuedSauHalfOffsets.front();
        issuedSauHalfOffsets.pop_front();
        for (uint8_t byte = 0; byte < Sram128Bytes; ++byte) {
            outputs.sau.readData[byte] =
                legacySauResponse.readData[completedHalf + byte];
        }
    }
    outputs.masterAccepted = crossbarAfter.acceptedMaster;
    outputs.masterDropped = crossbarAfter.droppedMaster;
    outputs.bankRequest = crossbarAfter.bankRequest;
    outputs.sameBankCollision = crossbarAfter.sameBankCollision;
    if (crossbarAfter.masters[
            static_cast<uint8_t>(DutKuiDataMaster::Veu)].valid &&
        !issuedVeuRequests.empty()) {
        const DutKuiVeuRequest completed = issuedVeuRequests.front();
        issuedVeuRequests.pop_front();
        DutKuiVeuResponse response;
        response.valid = true;
        response.transactionId = completed.transactionId;
        response.isWrite = completed.isWrite;
        const uint8_t halfOffset =
            (completed.address & Sram128Bytes) ? Sram128Bytes : 0;
        const auto &line = crossbarAfter.masters[
            static_cast<uint8_t>(DutKuiDataMaster::Veu)].readData;
        for (uint8_t byte = 0; byte < Sram128Bytes; ++byte) {
            response.readData[byte] = line[halfOffset + byte];
        }
        if (completed.isWrite) {
            outputs.veuWrite = response;
        } else {
            outputs.veuRead = response;
        }
        if (veuOutstanding > 0) {
            --veuOutstanding;
        }
    }
    if (crossbarAfter.acceptedMaster[
            static_cast<uint8_t>(DutKuiDataMaster::Veu)] &&
        !pendingVeuRequests.empty()) {
        issuedVeuRequests.push_back(pendingVeuRequests.front());
        pendingVeuRequests.pop_front();
    }

    const SramConverter32To256Output converterAfter =
        dbusConverter.evaluate();
    if (converterAfter.master.valid) {
        outputs.dbus.valid = true;
        outputs.dbus.isWrite = converterAfter.master.isWrite;
        outputs.dbus.readData = converterAfter.master.readData;
        dbusOutstanding = false;
    }

    previousVeuLockActive = veuLockActive;
    ibusAcceptedThisCycle = false;
    veuAcceptedThisCycle = false;
    acceptedIbus = {};
    acceptedVeu = {};
    return outputs;
}

void
DutKuiMemoryModel::clockEdge(
    bool veuLockActive, const SauMemoryOutput &sau)
{
    visibleOutputs = advance(veuLockActive, sau);
}

DutKuiMemoryOutputs
DutKuiMemoryModel::clock(
    bool veuLockActive, const SauMemoryOutput &sau)
{
    clockEdge(veuLockActive, sau);
    return visibleOutputs;
}

void
DutKuiMemoryModel::writeByte(uint32_t address, uint8_t value)
{
    if (instructionMapped(address)) {
        instructionMemory[address] = value;
    } else {
        dataCrossbar.writeByte(address, value);
    }
}

uint8_t
DutKuiMemoryModel::readByte(uint32_t address) const
{
    if (instructionMapped(address)) {
        const auto found = instructionMemory.find(address);
        return found == instructionMemory.end() ? 0 : found->second;
    }
    return dataCrossbar.readByte(address);
}

void
DutKuiMemoryModel::writeWord(uint32_t address, uint32_t value)
{
    for (uint32_t byte = 0; byte < 4; ++byte) {
        writeByte(address + byte,
                  static_cast<uint8_t>(value >> (byte * 8)));
    }
}

uint32_t
DutKuiMemoryModel::readWord(uint32_t address) const
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

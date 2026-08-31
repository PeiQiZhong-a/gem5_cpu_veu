#include "brs/memory/npu_lpnpu_mikui_memory_model.hh"

namespace gem5
{
namespace brs
{

NpuLpnpuMikuiMemoryModel::NpuLpnpuMikuiMemoryModel()
  : NpuLpnpuMikuiMemoryModel(Config{})
{}

NpuLpnpuMikuiMemoryModel::NpuLpnpuMikuiMemoryModel(Config config)
  : config(config), dmaCrossbar(NpuLpnpuMikuiDmaCrossbar::Config{})
{
    reset();
}

void
NpuLpnpuMikuiMemoryModel::reset()
{
    ibusOutstanding = false;
    dbusOutstanding = false;
    veuOutstanding = 0;
    ibusAcceptedThisCycle = false;
    dbusAcceptedThisCycle = false;
    veuAcceptedThisCycle = false;
    acceptedIbus = {};
    acceptedDbus = {};
    acceptedVeu = {};
    pendingVeuRequests.clear();
    issuedVeuRequests.clear();
    previousVeuLockActive = false;
    dbusConverter.reset();
    crossbar.reset();
    dmaCrossbar.reset();
    visibleOutputs = {};
}

bool
NpuLpnpuMikuiMemoryModel::instructionMapped(uint32_t address) const
{
    return address >= config.instBase &&
        address - config.instBase < config.instSize;
}

bool
NpuLpnpuMikuiMemoryModel::acceptIbus(
    const DutKuiIbusRequest &request)
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
NpuLpnpuMikuiMemoryModel::acceptDbus(
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
NpuLpnpuMikuiMemoryModel::acceptVeu(
    const DutKuiVeuRequest &request)
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

Sram128Request
NpuLpnpuMikuiMemoryModel::currentVeuBeat() const
{
    Sram128Request beat;
    if (pendingVeuRequests.empty()) {
        return beat;
    }
    const DutKuiVeuRequest &request = pendingVeuRequests.front();
    beat.valid = true;
    beat.address = request.address;
    beat.writeStrobe = request.isWrite ?
        static_cast<uint16_t>(request.writeStrobe) : 0;
    for (uint8_t byte = 0; byte < Sram128Bytes; ++byte) {
        beat.writeData[byte] = request.data[byte];
    }
    return beat;
}

DutKuiMemoryOutputs
NpuLpnpuMikuiMemoryModel::advance(
    bool veuLockActive, const SauMemoryOutput &sau)
{
    DutKuiMemoryOutputs outputs;

    // sram_tcdm registers ready and read data directly from the current IBus
    // request. They become visible after this edge and are sampled by the CPU
    // on the following edge.
    if (ibusAcceptedThisCycle) {
        outputs.ibus.valid = true;
        const uint32_t base = acceptedIbus.address & ~uint32_t{0x0f};
        if (instructionMapped(base)) {
            for (uint8_t word = 0; word < 4; ++word) {
                outputs.ibus.readData[word] = readWord(base + word * 4);
            }
        }
        ibusOutstanding = false;
    }

    if (veuAcceptedThisCycle) {
        pendingVeuRequests.push_back(acceptedVeu);
    }

    const SramConverter32To128Output converterBefore =
        dbusConverter.evaluate();
    Sram128Response dbusResponseBefore;
    if (config.dmaTopology) {
        dbusResponseBefore = dmaCrossbar.evaluate().dbus;
    } else {
        dbusResponseBefore = crossbar.evaluate().dbus;
    }

    Sram32Request dbusInput;
    if (dbusAcceptedThisCycle && dbusConverter.canAccept()) {
        dbusInput.valid = true;
        dbusInput.address = acceptedDbus.address;
        dbusInput.writeStrobe = acceptedDbus.writeStrobe;
        dbusInput.writeData = acceptedDbus.writeData;
        dbusAcceptedThisCycle = false;
        acceptedDbus = {};
    }
    dbusConverter.clock(dbusInput, dbusResponseBefore);

    NpuLpnpuMikuiCrossbarInputs crossbarInputs;
    crossbarInputs.dbus = converterBefore.sram;
    const uint8_t sauMaster =
        static_cast<uint8_t>(NpuLpnpuMikuiMaster::Sau);
    const uint8_t veuMaster =
        static_cast<uint8_t>(NpuLpnpuMikuiMaster::Veu);
    crossbarInputs.masters[sauMaster] = sau.request;
    crossbarInputs.crossbarStart[sauMaster] = sau.crossbarStart;
    crossbarInputs.crossbarDone[sauMaster] = sau.crossbarDone;
    crossbarInputs.masters[veuMaster] = currentVeuBeat();
    crossbarInputs.crossbarStart[veuMaster] =
        veuLockActive && !previousVeuLockActive;
    crossbarInputs.crossbarDone[veuMaster] =
        !veuLockActive && previousVeuLockActive;

    Sram128Response veuResponse;
    bool veuAccepted = false;
    if (config.dmaTopology) {
        dmaCrossbar.clock(crossbarInputs);
        const auto crossbarAfter = dmaCrossbar.evaluate();
        outputs.sau = crossbarAfter.masters[sauMaster];
        outputs.masterAccepted = crossbarAfter.acceptedMaster;
        outputs.masterDropped = crossbarAfter.droppedMaster;
        for (uint8_t bank = 0; bank < 3; ++bank) {
            outputs.bankRequest[bank] = crossbarAfter.bankRequest[bank];
        }
        outputs.sameBankCollision = crossbarAfter.sameBankCollision;
        veuResponse = crossbarAfter.masters[veuMaster];
        veuAccepted = crossbarAfter.acceptedMaster[veuMaster];
    } else {
        crossbar.clock(crossbarInputs);
        const auto crossbarAfter = crossbar.evaluate();
        outputs.sau = crossbarAfter.masters[sauMaster];
        outputs.masterAccepted = crossbarAfter.acceptedMaster;
        outputs.masterDropped = crossbarAfter.droppedMaster;
        outputs.bankRequest[0] = crossbarAfter.bankRequest[0];
        outputs.bankRequest[1] = crossbarAfter.bankRequest[1];
        outputs.sameBankCollision = crossbarAfter.sameBankCollision;
        veuResponse = crossbarAfter.masters[veuMaster];
        veuAccepted = crossbarAfter.acceptedMaster[veuMaster];
    }

    if (veuAccepted && !pendingVeuRequests.empty()) {
        issuedVeuRequests.push_back(pendingVeuRequests.front());
        pendingVeuRequests.pop_front();
    }

    if (veuResponse.valid && !issuedVeuRequests.empty()) {
        const DutKuiVeuRequest issued = issuedVeuRequests.front();
        issuedVeuRequests.pop_front();
        DutKuiVeuResponse response;
        response.valid = true;
        response.transactionId = issued.transactionId;
        response.isWrite = issued.isWrite;
        if (!issued.isWrite) {
            response.readData = veuResponse.readData;
        }
        if (response.isWrite) {
            outputs.veuWrite = response;
        } else {
            outputs.veuRead = response;
        }
        if (veuOutstanding > 0) {
            --veuOutstanding;
        }
    }

    const SramConverter32To128Output converterAfter =
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
NpuLpnpuMikuiMemoryModel::clockEdge(
    bool veuLockActive, const SauMemoryOutput &sau)
{
    visibleOutputs = advance(veuLockActive, sau);
}

DutKuiMemoryOutputs
NpuLpnpuMikuiMemoryModel::clock(
    bool veuLockActive, const SauMemoryOutput &sau)
{
    clockEdge(veuLockActive, sau);
    return visibleOutputs;
}

void
NpuLpnpuMikuiMemoryModel::writeByte(uint32_t address, uint8_t value)
{
    if (instructionMapped(address)) {
        instructionMemory[address] = value;
    } else if (config.dmaTopology) {
        dmaCrossbar.writeByte(address, value);
    } else {
        crossbar.writeByte(address, value);
    }
}

uint8_t
NpuLpnpuMikuiMemoryModel::readByte(uint32_t address) const
{
    if (instructionMapped(address)) {
        const auto found = instructionMemory.find(address);
        return found == instructionMemory.end() ? 0 : found->second;
    }
    if (config.dmaTopology) {
        return dmaCrossbar.readByte(address);
    }
    return crossbar.readByte(address);
}

void
NpuLpnpuMikuiMemoryModel::writeWord(uint32_t address, uint32_t value)
{
    for (uint8_t byte = 0; byte < 4; ++byte) {
        writeByte(address + byte,
                  static_cast<uint8_t>(value >> (byte * 8)));
    }
}

uint32_t
NpuLpnpuMikuiMemoryModel::readWord(uint32_t address) const
{
    uint32_t value = 0;
    for (uint8_t byte = 0; byte < 4; ++byte) {
        value |= static_cast<uint32_t>(readByte(address + byte)) <<
            (byte * 8);
    }
    return value;
}

} // namespace brs
} // namespace gem5

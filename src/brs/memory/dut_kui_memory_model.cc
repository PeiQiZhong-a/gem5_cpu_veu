#include "brs/memory/dut_kui_memory_model.hh"

namespace gem5
{
namespace brs
{

DutKuiMemoryModel::DutKuiMemoryModel() : DutKuiMemoryModel(Config{})
{}

DutKuiMemoryModel::DutKuiMemoryModel(Config config) : config(config)
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
    ibusResponses.clear();
    dbusResponses.clear();
    veuReadResponses.clear();
    veuWriteResponses.clear();
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
    const uint64_t size = static_cast<uint64_t>(config.bankSize) *
        config.bankCount;
    return address >= config.dataBase &&
        static_cast<uint64_t>(address - config.dataBase) < size;
}

uint32_t
DutKuiMemoryModel::alignedDataAddress(uint32_t address) const
{
    return address & ~uint32_t{0x1f};
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
DutKuiMemoryModel::acceptDbus(const DutKuiDbusRequest &request,
                              bool veuLockActive)
{
    if (veuLockActive || dbusOutstanding || dbusAcceptedThisCycle) {
        return false;
    }
    acceptedDbus = request;
    dbusAcceptedThisCycle = true;
    dbusOutstanding = true;
    return true;
}

bool
DutKuiMemoryModel::acceptVeu(const DutKuiVeuRequest &request)
{
    if (veuAcceptedThisCycle || veuOutstanding >= config.maxVeuOutstanding) {
        return false;
    }
    acceptedVeu = request;
    veuAcceptedThisCycle = true;
    ++veuOutstanding;
    return true;
}

VeuVector
DutKuiMemoryModel::readVector(uint32_t address) const
{
    VeuVector data = {};
    const uint32_t base = alignedDataAddress(address);
    if (!dataMapped(base)) {
        return data;
    }
    for (uint32_t byte = 0; byte < VeuVectorBytes; ++byte) {
        const auto found = dataMemory.find(base + byte);
        if (found != dataMemory.end()) {
            data[byte] = found->second;
        }
    }
    return data;
}

void
DutKuiMemoryModel::writeVector(uint32_t address, uint32_t strobe,
                               const VeuVector &data)
{
    const uint32_t base = alignedDataAddress(address);
    if (!dataMapped(base)) {
        return;
    }
    for (uint32_t byte = 0; byte < VeuVectorBytes; ++byte) {
        if (strobe & (uint32_t{1} << byte)) {
            dataMemory[base + byte] = data[byte];
        }
    }
}

DutKuiMemoryOutputs
DutKuiMemoryModel::clock(bool veuLockActive)
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
    advance(dbusResponses, outputs.dbus);
    advance(veuReadResponses, outputs.veuRead);
    advance(veuWriteResponses, outputs.veuWrite);

    if (outputs.ibus.valid) {
        ibusOutstanding = false;
    }
    if (outputs.dbus.valid) {
        dbusOutstanding = false;
    }
    if (outputs.veuRead.valid && veuOutstanding > 0) {
        --veuOutstanding;
    }
    if (outputs.veuWrite.valid && veuOutstanding > 0) {
        --veuOutstanding;
    }

    if (ibusAcceptedThisCycle) {
        DutKuiIbusResponse response;
        response.valid = true;
        const uint32_t base = acceptedIbus.address & ~uint32_t{0x0f};
        if (instructionMapped(base)) {
            for (uint32_t word = 0; word < 4; ++word) {
                response.readData[word] = readWord(base + word * 4);
            }
        }
        ibusResponses.push_back({config.ibusResponseLatency, response});
    }

    if (dbusAcceptedThisCycle) {
        DutKuiDbusResponse response;
        response.valid = true;
        response.isWrite = acceptedDbus.isWrite();
        const uint32_t base = alignedDataAddress(acceptedDbus.address);
        const uint32_t wordOffset = (acceptedDbus.address >> 2) & 0x7;
        if (acceptedDbus.isWrite() && dataMapped(base)) {
            VeuVector writeData = {};
            uint32_t vectorStrobe = 0;
            for (uint32_t byte = 0; byte < 4; ++byte) {
                writeData[wordOffset * 4 + byte] =
                    static_cast<uint8_t>(acceptedDbus.writeData >> (byte * 8));
                if (acceptedDbus.writeStrobe & (uint8_t{1} << byte)) {
                    vectorStrobe |= uint32_t{1} << (wordOffset * 4 + byte);
                }
            }
            writeVector(base, vectorStrobe, writeData);
        } else if (dataMapped(base)) {
            const VeuVector data = readVector(base);
            for (uint32_t byte = 0; byte < 4; ++byte) {
                response.readData |= static_cast<uint32_t>(
                    data[wordOffset * 4 + byte]) << (byte * 8);
            }
        }
        dbusResponses.push_back({config.dbusResponseLatency, response});
    }

    if (veuAcceptedThisCycle) {
        DutKuiVeuResponse response;
        response.valid = true;
        response.transactionId = acceptedVeu.transactionId;
        response.isWrite = acceptedVeu.isWrite;
        if (acceptedVeu.isWrite) {
            writeVector(acceptedVeu.address, acceptedVeu.writeStrobe,
                        acceptedVeu.data);
        } else {
            response.readData = readVector(acceptedVeu.address);
        }
        const uint32_t latency = acceptedVeu.isWrite ?
            config.veuWriteLatency : config.veuReadLatency;
        if (latency == 0) {
            auto &output = acceptedVeu.isWrite ?
                outputs.veuWrite : outputs.veuRead;
            output = response;
            if (veuOutstanding > 0) {
                --veuOutstanding;
            }
        } else if (acceptedVeu.isWrite) {
            veuWriteResponses.push_back({latency, response});
        } else {
            veuReadResponses.push_back({latency, response});
        }
    }

    ibusAcceptedThisCycle = false;
    dbusAcceptedThisCycle = false;
    veuAcceptedThisCycle = false;
    acceptedIbus = {};
    acceptedDbus = {};
    acceptedVeu = {};
    (void)veuLockActive;
    return outputs;
}

void
DutKuiMemoryModel::writeByte(uint32_t address, uint8_t value)
{
    if (instructionMapped(address)) {
        instructionMemory[address] = value;
    } else if (dataMapped(address)) {
        dataMemory[address] = value;
    }
}

uint8_t
DutKuiMemoryModel::readByte(uint32_t address) const
{
    const auto &memory = instructionMapped(address) ?
        instructionMemory : dataMemory;
    const auto found = memory.find(address);
    return found == memory.end() ? 0 : found->second;
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

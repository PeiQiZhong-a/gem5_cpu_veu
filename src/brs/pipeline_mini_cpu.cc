#include "brs/pipeline_mini_cpu.hh"

#include "base/logging.hh"
#include "base/output.hh"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>

#include "sim/sim_exit.hh"
#include "mem/packet_access.hh"

namespace gem5
{

bool
PipelineMiniCPU::CpuRequestPort::recvTimingResp(PacketPtr pkt)
{
    if (kind == PortKind::Inst) {
        return owner->completeTimingFetch(pkt);
    }
    return owner->completeTimingData(pkt);
}

void
PipelineMiniCPU::CpuRequestPort::recvReqRetry()
{
    if (kind == PortKind::Inst) {
        owner->retryInstFetch();
    } else {
        owner->retryDataRequest();
    }
}

PipelineMiniCPU::PipelineMiniCPU(const PipelineMiniCPUParams &p)
    : ClockedObject(p),
      maxCycles(p.max_cycles),
      resetCyclesRemaining(p.reset_cycles),
      textBase(p.text_base),
      system(p.system),
      core(),
      tickEvent([this] { processTick(); }, name()),
      programFile(p.program_file),
      elfFile(p.elf_file),
      preloadedProgram(p.preloaded_program),
      preloadedProgramSize(p.preloaded_program_size),
      dmemHexFile(p.dmem_hex_file),
      dmemBase(p.dmem_base),
      pipeStats(this),
      instRequestorId(p.system->getRequestorId(this, "inst")),
      dataRequestorId(p.system->getRequestorId(this, "data")),
      tbMemoryEnabled(p.tb_memory_enabled),
      tbImemImageFile(p.tb_imem_image_file),
      tbDmemImageFile(p.tb_dmem_image_file),
      tbInstBase(p.tb_inst_base),
      tbInstSize(p.tb_inst_size),
      tbDataBase(p.tb_data_base),
      tbDataStorageSize(std::min<Addr>(p.tb_data_size, 0x00020000)),
      tbCrossbar(brs::TbCrossbarModel::Config{
          p.tb_ibus_response_delay,
          p.tb_dbus_response_delay,
          p.tb_veu_pipeline_stages,
          static_cast<uint32_t>(p.tb_inst_base),
          static_cast<uint32_t>(p.tb_inst_size),
          static_cast<uint32_t>(p.tb_data_base),
          static_cast<uint32_t>(p.tb_data_size)}),
      cycleTraceFile(p.cycle_trace_file),
      instPort(name() + ".inst_port", this, CpuRequestPort::PortKind::Inst),
      dataPort(name() + ".data_port", this, CpuRequestPort::PortKind::Data),
      icacheEnabled(p.icache_enabled),
      icacheSize(p.icache_size),
      icacheLineSize(p.icache_line_size),
      icacheNumLines(0),
      frontendBurstBytes(p.frontend_burst_bytes)
{
    fatal_if(frontendBurstBytes != 16,
             "PipelineMiniCPU frontend currently models 16-byte RV-NEW bursts");
    core.configureFrontend(p.instr_fifo_depth, p.frontend_burst_bytes);
    // aerith_tb_top.sv never terminates at the end of the loaded image.  It
    // keeps fetching until the DONE monitor or the absolute testbench timeout.
    core.configureTextEndTermination(!tbMemoryEnabled);
    core.configureFakeVeu(
        p.fake_veu_latency, p.fake_veu_response_data);
    core.setInterruptInputs(
        p.irq_external, p.irq_software, p.irq_timer);
    core.setDebugInputs(
        p.debug_halt, p.debug_halt_on_reset, p.debug_resume, p.debug_data0);
    core.setDebugInstruction(p.debug_instr, p.debug_instr_valid);

    fatal_if(tbMemoryEnabled && icacheEnabled,
             "RTL-testbench memory mode requires the gem5-side I-cache to be disabled");
    fatal_if(tbMemoryEnabled && !elfFile.empty(),
             "RTL-testbench memory mode requires a raw or hex instruction image, not ELF physProxy preload");
    fatal_if(p.tb_inst_base > 0xffffffffULL ||
             p.tb_data_base > 0xffffffffULL ||
             p.tb_inst_size > 0xffffffffULL ||
             p.tb_data_size > 0xffffffffULL,
             "RTL-testbench address map must fit in the 32-bit RTL bus");

    if (icacheEnabled) {
        fatal_if(icacheLineSize < frontendBurstBytes,
                 "I-cache line size must be at least the frontend burst size");
        fatal_if((icacheLineSize % sizeof(uint32_t)) != 0,
                 "I-cache line size must be a multiple of 4 bytes");
        fatal_if(icacheSize < icacheLineSize,
                 "I-cache size must be at least one line");
        fatal_if((icacheSize % icacheLineSize) != 0,
                 "I-cache size must be a multiple of line size");

        icacheNumLines = icacheSize / icacheLineSize;
        icache.resize(icacheNumLines);
        for (auto &line : icache) {
            line.bytes.resize(icacheLineSize, 0);
        }
    }
}

Port &
PipelineMiniCPU::getPort(const std::string &if_name, PortID idx)
{
    if (if_name == "inst_port") {
        return instPort;
    }
    if (if_name == "data_port") {
        return dataPort;
    }
    return ClockedObject::getPort(if_name, idx);
}

Addr
PipelineMiniCPU::icacheLineBase(Addr addr) const
{
    return (addr / icacheLineSize) * icacheLineSize;
}

bool
PipelineMiniCPU::icacheLookup(Addr addr, uint32_t &inst)
{
    if (!icacheEnabled || icacheNumLines == 0) {
        return false;
    }

    const Addr blockAddr = icacheLineBase(addr);
    const uint32_t index = (blockAddr / icacheLineSize) % icacheNumLines;
    const ICacheLine &line = icache[index];

    if (!line.valid || line.tag != blockAddr) {
        return false;
    }

    const uint32_t offset = addr - blockAddr;
    panic_if(offset + sizeof(uint32_t) > line.bytes.size(),
             "Instruction fetch crosses I-cache line boundary: addr=%#x",
             addr);

    inst = static_cast<uint32_t>(line.bytes[offset]) |
           (static_cast<uint32_t>(line.bytes[offset + 1]) << 8) |
           (static_cast<uint32_t>(line.bytes[offset + 2]) << 16) |
           (static_cast<uint32_t>(line.bytes[offset + 3]) << 24);
    return true;
}

bool
PipelineMiniCPU::icacheLookupBlock(Addr fetchAddr, FetchBlock &block)
{
    if (!icacheEnabled || icacheNumLines == 0) {
        return false;
    }

    const Addr blockAddr = (fetchAddr / frontendBurstBytes) * frontendBurstBytes;
    const Addr lineAddr = icacheLineBase(blockAddr);
    const uint32_t index = (lineAddr / icacheLineSize) % icacheNumLines;
    const ICacheLine &line = icache[index];

    if (!line.valid || line.tag != lineAddr) {
        return false;
    }

    const uint32_t offset = blockAddr - lineAddr;
    panic_if(offset + frontendBurstBytes > line.bytes.size(),
             "Instruction burst crosses I-cache line boundary: addr=%#x",
             blockAddr);

    block.fetchAddr = static_cast<uint32_t>(fetchAddr);
    block.blockAddr = static_cast<uint32_t>(blockAddr);
    for (unsigned i = 0; i < 4; ++i) {
        const uint32_t wordOffset = offset + i * sizeof(uint32_t);
        block.words[i] =
            static_cast<uint32_t>(line.bytes[wordOffset]) |
            (static_cast<uint32_t>(line.bytes[wordOffset + 1]) << 8) |
            (static_cast<uint32_t>(line.bytes[wordOffset + 2]) << 16) |
            (static_cast<uint32_t>(line.bytes[wordOffset + 3]) << 24);
    }

    return true;
}

void
PipelineMiniCPU::fillICacheLine(Addr blockAddr, const uint8_t *data,
                                unsigned size)
{
    if (!icacheEnabled || icacheNumLines == 0) {
        return;
    }

    const uint32_t index = (blockAddr / icacheLineSize) % icacheNumLines;
    ICacheLine &line = icache[index];
    line.valid = true;
    line.tag = blockAddr;
    std::fill(line.bytes.begin(), line.bytes.end(), 0);
    std::memcpy(line.bytes.data(), data,
                std::min<unsigned>(size, icacheLineSize));
}

bool
PipelineMiniCPU::fetchInstrFunctional(uint32_t addr, uint32_t &inst)
{
    if (tbMemoryEnabled) {
        inst = tbCrossbar.readWord(addr);
        return true;
    }

    auto req = std::make_shared<Request>(
        addr, 4, Request::INST_FETCH, instRequestorId);
    auto pkt = new Packet(req, MemCmd::ReadReq);
    pkt->allocate();

    instPort.sendFunctional(pkt);
    inst = pkt->getLE<uint32_t>();

    delete pkt;
    return true;
}

bool
PipelineMiniCPU::requestInstrTiming(uint32_t addr)
{
    const Addr fetchAddr = addr;
    FetchBlock cachedBlock;
    if (icacheLookupBlock(fetchAddr, cachedBlock)) {
        ++icacheHitCount;
        core.acceptFetchedBlock(cachedBlock);
        return true;
    }

    if ((tbMemoryEnabled && tbInstOutstanding) ||
        (!tbMemoryEnabled && pendingInstFetch != nullptr)) {
        return false;
    }

    ++icacheMissCount;
    const Addr reqAddr = icacheEnabled ? icacheLineBase(fetchAddr) :
        (fetchAddr / frontendBurstBytes) * frontendBurstBytes;
    const unsigned fetchSize = icacheEnabled ? icacheLineSize :
        frontendBurstBytes;

    if (tbMemoryEnabled) {
        panic_if(fetchSize != 16,
                 "RTL-testbench IBus supports one 16-byte beat, got %u",
                 fetchSize);
        pendingInstAddr = reqAddr;
        pendingInstFetchAddr = fetchAddr;
        tbIbusPulse = {};
        tbIbusPulse.valid = true;
        tbIbusPulse.address = static_cast<uint32_t>(reqAddr);
        tbIbusPulse.read = true;
        tbInstOutstanding = true;
        return true;
    }

    auto req = std::make_shared<Request>(
        reqAddr, fetchSize, Request::INST_FETCH, instRequestorId);
    auto pkt = new Packet(req, MemCmd::ReadReq);
    pkt->allocate();

    pendingInstAddr = reqAddr;
    pendingInstFetchAddr = fetchAddr;
    pendingInstFetch = pkt;
    instFetchRetry = false;

    if (instPort.sendTimingReq(pkt)) {
        pendingInstFetch = nullptr;
    } else {
        instFetchRetry = true;
    }

    return true;
}

void
PipelineMiniCPU::completeTbFetch(const brs::TbBusResponse &response)
{
    panic_if(!tbInstOutstanding,
             "RTL-testbench IBus returned without an outstanding fetch");

    FetchBlock block;
    block.fetchAddr = static_cast<uint32_t>(pendingInstFetchAddr);
    block.blockAddr = static_cast<uint32_t>(pendingInstAddr);
    block.words = response.readData;
    core.acceptFetchedBlock(block);

    tbInstOutstanding = false;
    pendingInstAddr = 0;
    pendingInstFetchAddr = 0;
}

bool
PipelineMiniCPU::completeTimingFetch(PacketPtr pkt)
{
    panic_if(pkt->isError(), "Timing instruction fetch failed: %s",
             pkt->print());

    FetchBlock block;
    if (icacheEnabled) {
        fillICacheLine(pkt->getAddr(), pkt->getConstPtr<uint8_t>(),
                       pkt->getSize());
        const bool hit = icacheLookupBlock(pendingInstFetchAddr, block);
        panic_if(!hit, "I-cache line fill did not contain requested fetch %#x",
                 pendingInstFetchAddr);
        core.acceptFetchedBlock(block);
    } else {
        const uint8_t *data = pkt->getConstPtr<uint8_t>();
        block.fetchAddr = static_cast<uint32_t>(pendingInstFetchAddr);
        block.blockAddr = static_cast<uint32_t>(pkt->getAddr());
        for (unsigned i = 0; i < 4; ++i) {
            const unsigned offset = i * sizeof(uint32_t);
            block.words[i] =
                static_cast<uint32_t>(data[offset]) |
                (static_cast<uint32_t>(data[offset + 1]) << 8) |
                (static_cast<uint32_t>(data[offset + 2]) << 16) |
                (static_cast<uint32_t>(data[offset + 3]) << 24);
        }
        core.acceptFetchedBlock(block);
    }

    pendingInstFetch = nullptr;
    instFetchRetry = false;
    pendingInstAddr = 0;
    pendingInstFetchAddr = 0;

    delete pkt;
    return true;
}

void
PipelineMiniCPU::retryInstFetch()
{
    if (!pendingInstFetch || !instFetchRetry) {
        return;
    }

    PacketPtr pkt = pendingInstFetch;
    if (instPort.sendTimingReq(pkt)) {
        pendingInstFetch = nullptr;
        instFetchRetry = false;
    }
}

bool
PipelineMiniCPU::requestDataTiming(uint32_t addr, unsigned size,
                                   bool isWrite, uint32_t writeData)
{
    if ((tbMemoryEnabled && tbDataOutstanding) ||
        (!tbMemoryEnabled && pendingDataReq != nullptr)) {
        return false;
    }

    if (tbMemoryEnabled) {
        panic_if(size != 1 && size != 2 && size != 4,
                 "Unsupported RTL-testbench data access size: %u", size);
        pendingDataAddr = addr;
        pendingDataSize = size;
        pendingDataIsWrite = isWrite;
        pendingDataWriteValue = writeData;
        tbDbusPulse = {};
        tbDbusPulse.valid = true;
        tbDbusPulse.address = addr;
        tbDbusPulse.read = !isWrite;
        tbDbusPulse.write = isWrite;

        if (isWrite) {
            const unsigned byteOffset = addr & 0x3u;
            uint16_t wordStrobe = 0;
            uint32_t busData = writeData;
            if (size == 1) {
                wordStrobe = uint16_t{1} << byteOffset;
                const uint32_t byte = writeData & 0xffu;
                busData = byte * 0x01010101u;
            } else if (size == 2) {
                wordStrobe = (uint16_t{3} << byteOffset) & 0xfu;
                const uint32_t half = writeData & 0xffffu;
                busData = half | (half << 16);
            } else {
                // SW ignores addr[1:0] in the generated LSU.
                wordStrobe = 0xfu;
            }
            // DBus is 4/32 bits at the crossbar boundary.  The crossbar, not
            // the LSU, shifts/broadcasts these values for an SRAM slave.
            tbDbusPulse.writeStrobe = wordStrobe;
            tbDbusPulse.writeData[0] = busData;
        }

        tbDataOutstanding = true;
        if (isWrite && size == 4 && addr == 0x4001e004 &&
            (writeData == 2 || writeData == 4)) {
            // aerith_tb_top.sv observes this request directly, outside the
            // crossbar decode, and terminates on the same rising edge.
            tbDoneRequested = true;
            tbDoneValue = writeData;
        }
        return true;
    }

    // The RTL DBus always transfers one selected 32-bit word.  Subword
    // behavior is represented by four byte enables, not by changing the bus
    // request width, and addr[1:0] never makes a transfer cross into the next
    // word.  Keep the regular gem5 memory path identical to the TB path.
    const Addr requestAddr = addr & ~Addr{0x3};
    constexpr unsigned requestSize = 4;
    Request::Flags flags;
    auto req = std::make_shared<Request>(
        requestAddr, requestSize, flags, dataRequestorId);
    auto pkt = new Packet(req, isWrite ? MemCmd::WriteReq : MemCmd::ReadReq);
    pkt->allocate();

    if (isWrite) {
        const unsigned byteOffset = addr & 0x3u;
        uint32_t busData = writeData;
        uint8_t wordStrobe = 0;
        if (size == 1) {
            wordStrobe = uint8_t{1} << byteOffset;
            busData = (writeData & 0xffu) * 0x01010101u;
        } else if (size == 2) {
            wordStrobe = (uint8_t{3} << byteOffset) & 0xfu;
            const uint32_t half = writeData & 0xffffu;
            busData = half | (half << 16);
        } else if (size == 4) {
            wordStrobe = 0xfu;
        } else {
            panic("Unsupported data store size: %u", size);
        }
        req->setByteEnable({
            bool(wordStrobe & 0x1), bool(wordStrobe & 0x2),
            bool(wordStrobe & 0x4), bool(wordStrobe & 0x8)});
        pkt->setLE<uint32_t>(busData);
    }

    pendingDataAddr = addr;
    pendingDataSize = size;
    pendingDataIsWrite = isWrite;
    pendingDataWriteValue = writeData;
    pendingDataReq = pkt;
    dataReqRetry = false;

    if (dataPort.sendTimingReq(pkt)) {
        pendingDataReq = nullptr;
    } else {
        dataReqRetry = true;
    }

    return true;
}

void
PipelineMiniCPU::completeTbData(const brs::TbBusResponse &response)
{
    panic_if(!tbDataOutstanding,
             "RTL-testbench DBus returned without an outstanding request");

    uint32_t data = 0;
    if (!pendingDataIsWrite) {
        const unsigned word = (pendingDataAddr >> 2) & 0x3;
        // The crossbar selects a 32-bit word with addr[3:2].  LSU.sv then
        // performs the byte/halfword lane selection using addr[1:0].
        data = response.readData[word];
    }

    core.acceptDataResponse(static_cast<uint32_t>(pendingDataAddr),
                            data, pendingDataIsWrite);
    tbDataOutstanding = false;
    pendingDataAddr = 0;
    pendingDataSize = 0;
    pendingDataIsWrite = false;
    pendingDataWriteValue = 0;
}

bool
PipelineMiniCPU::completeTimingData(PacketPtr pkt)
{
    panic_if(pkt->isError(), "Timing data access failed: %s", pkt->print());

    uint32_t data = 0;
    if (!pendingDataIsWrite) {
        data = pkt->getLE<uint32_t>();
    }

    core.acceptDataResponse(static_cast<uint32_t>(pendingDataAddr),
                            data, pendingDataIsWrite);

    pendingDataReq = nullptr;
    dataReqRetry = false;
    pendingDataAddr = 0;
    pendingDataSize = 0;
    pendingDataIsWrite = false;
    pendingDataWriteValue = 0;

    delete pkt;
    return true;
}

void
PipelineMiniCPU::retryDataRequest()
{
    if (!pendingDataReq || !dataReqRetry) {
        return;
    }

    PacketPtr pkt = pendingDataReq;
    if (dataPort.sendTimingReq(pkt)) {
        pendingDataReq = nullptr;
        dataReqRetry = false;
    }
}

void
PipelineMiniCPU::preloadElf()
{
    std::unique_ptr<loader::ObjectFile> obj(loader::createObjectFile(elfFile));
    fatal_if(!obj, "Failed to load ELF file: %s", elfFile);

    fatal_if(obj->getArch() != loader::Riscv64 &&
             obj->getArch() != loader::Riscv32,
             "ELF file is not a RISC-V binary: %s", elfFile);

    auto image = obj->buildImage();
    fatal_if(!image.write(system->physProxy),
             "Failed to write ELF image to physical memory: %s", elfFile);

    const Addr start = obj->entryPoint();
    const Addr end = image.maxAddr();
    fatal_if(end <= start,
             "Invalid ELF image range for %s: start=%#x end=%#x",
             elfFile, start, end);

    core.requestTimingFetch = [this](uint32_t addr) {
        return requestInstrTiming(addr);
    };
    core.requestTimingData = [this](uint32_t addr, unsigned size,
                                    bool isWrite, uint32_t writeData) {
        return requestDataTiming(addr, size, isWrite, writeData);
    };
    core.reset(static_cast<uint32_t>(start), static_cast<uint32_t>(end));
}

void
PipelineMiniCPU::preloadProgramFunctional()
{
    if (tbMemoryEnabled) {
        preloadedProgramSize = preloadTbReadmemh32Image(
            programFile, textBase, tbInstSize);
        core.requestTimingFetch = [this](uint32_t addr) {
            return requestInstrTiming(addr);
        };
        core.requestTimingData = [this](uint32_t addr, unsigned size,
                                        bool isWrite, uint32_t writeData) {
            return requestDataTiming(addr, size, isWrite, writeData);
        };
        core.reset(textBase, textBase + preloadedProgramSize);
        return;
    }

    ProgramImage img;
    if (!img.loadHexFile(programFile)) {
        fatal("Failed to load program hex: %s", programFile);
    }

    for (uint32_t i = 0; i < img.program_words; ++i) {
        Addr addr = textBase + i * 4;
        if (tbMemoryEnabled) {
            tbCrossbar.writeWord(static_cast<uint32_t>(addr),
                                 img.instr_mem[i]);
            continue;
        }
        auto req = std::make_shared<Request>(
            addr, 4, Request::INST_FETCH, instRequestorId);
        auto pkt = new Packet(req, MemCmd::WriteReq);
        pkt->allocate();
        pkt->setLE<uint32_t>(img.instr_mem[i]);
        instPort.sendFunctional(pkt);
        delete pkt;
    }

    core.requestTimingFetch = [this](uint32_t addr) {
        return requestInstrTiming(addr);
    };
    core.requestTimingData = [this](uint32_t addr, unsigned size,
                                    bool isWrite, uint32_t writeData) {
        return requestDataTiming(addr, size, isWrite, writeData);
    };
    core.reset(textBase, textBase + img.program_words * 4);
}

void
PipelineMiniCPU::usePreloadedProgram()
{
    fatal_if(preloadedProgramSize == 0,
             "preloaded_program_size must be non-zero when preloaded_program is enabled");

    core.requestTimingFetch = [this](uint32_t addr) {
        return requestInstrTiming(addr);
    };
    core.requestTimingData = [this](uint32_t addr, unsigned size,
                                    bool isWrite, uint32_t writeData) {
        return requestDataTiming(addr, size, isWrite, writeData);
    };
    core.reset(textBase, textBase + preloadedProgramSize);
}

void
PipelineMiniCPU::preloadDataFunctional()
{
    DataImage img;
    const bool loaded = tbMemoryEnabled ?
        img.loadReadmemh32File(dmemHexFile) :
        img.loadHexFile(dmemHexFile);
    if (!loaded) {
        fatal("Failed to load DMEM hex file: %s", dmemHexFile);
    }

    const size_t totalBytes = img.data.size();
    if (tbMemoryEnabled) {
        fatal_if(totalBytes > tbDataStorageSize,
                 "RTL DMEM $readmemh image is %llu bytes, capacity is %llu",
                 static_cast<unsigned long long>(totalBytes),
                 static_cast<unsigned long long>(tbDataStorageSize));
    }
    for (size_t i = 0; i < totalBytes; ++i) {
        Addr addr = dmemBase + i;
        if (tbMemoryEnabled) {
            tbCrossbar.writeByte(static_cast<uint32_t>(addr), img.data[i]);
            continue;
        }
        auto req = std::make_shared<Request>(
            addr, 1, 0, dataRequestorId);
        auto pkt = new Packet(req, MemCmd::WriteReq);
        pkt->allocate();
        pkt->setLE<uint8_t>(img.data[i]);
        dataPort.sendFunctional(pkt);
        delete pkt;
    }

    inform("preloadDataFunctional: wrote %llu bytes to DMEM at %#x from %s",
           static_cast<unsigned long long>(totalBytes), dmemBase, dmemHexFile);
}

Addr
PipelineMiniCPU::preloadTbRawImage(
    const std::string &path, Addr base, Addr capacity)
{
    std::ifstream input(path, std::ios::binary);
    fatal_if(!input.is_open(), "Failed to open RTL-testbench raw image: %s",
             path);

    Addr size = 0;
    char byte = 0;
    while (input.get(byte)) {
        fatal_if(size >= capacity,
                 "RTL-testbench raw image exceeds SRAM capacity (%llu bytes): %s",
                 static_cast<unsigned long long>(capacity), path);
        fatal_if(base + size > 0xffffffffULL,
                 "RTL-testbench raw image exceeds the 32-bit address bus: %s",
                 path);
        tbCrossbar.writeByte(static_cast<uint32_t>(base + size),
                             static_cast<uint8_t>(byte));
        ++size;
    }
    fatal_if(!input.eof(), "Failed while reading RTL-testbench image: %s",
             path);

    inform("preloadTbRawImage: wrote %llu bytes at %#x from %s",
           static_cast<unsigned long long>(size), base, path);
    return size;
}

Addr
PipelineMiniCPU::preloadTbReadmemh32Image(
    const std::string &path, Addr base, Addr capacity)
{
    DataImage image;
    fatal_if(!image.loadReadmemh32File(path),
             "Failed to load RTL-testbench $readmemh image: %s", path);
    fatal_if(image.data.size() > capacity,
             "RTL-testbench $readmemh image is %llu bytes, SRAM capacity is %llu: %s",
             static_cast<unsigned long long>(image.data.size()),
             static_cast<unsigned long long>(capacity), path);

    for (size_t byte = 0; byte < image.data.size(); ++byte) {
        tbCrossbar.writeByte(static_cast<uint32_t>(base + byte),
                             image.data[byte]);
    }
    inform("preloadTbReadmemh32Image: wrote %llu bytes at %#x from %s",
           static_cast<unsigned long long>(image.data.size()), base, path);
    return image.data.size();
}

void
PipelineMiniCPU::processTbMemoryCycle(const brs::VeuMemoryOutput &veu)
{
    // All sequential RTL blocks sample their old input pins on the same edge.
    // Feed the VEU the response that was already visible before this edge;
    // the crossbar response produced below becomes visible on the next edge.
    core.clockVeuMemory(tbVeuResponsePins);

    brs::TbCrossbarInputs inputs;
    inputs.ibus = tbIbusPulse;
    inputs.dbus = tbDbusPulse;

    inputs.veu.valid = veu.request.valid;
    inputs.veu.address = veu.request.address;
    inputs.veu.read = veu.request.valid && !veu.request.isWrite();
    inputs.veu.write = veu.request.valid && veu.request.isWrite();
    inputs.veu.writeStrobe = veu.request.writeStrobe;
    inputs.veu.writeData = veu.request.writeData;
    inputs.lockStart = veu.lockStart;
    inputs.lockFinish = veu.lockFinish;

    const brs::TbCrossbarOutputs outputs = tbCrossbar.clock(inputs);
    const std::string uartOutput = tbCrossbar.takeUartOutput();
    if (!uartOutput.empty() && uartLog.is_open()) {
        uartLog.write(uartOutput.data(), uartOutput.size());
        uartLog.flush();
    }
    writeTbCycleTrace(inputs, outputs);
    tbIbusPulse = {};
    tbDbusPulse = {};

    if (outputs.ibus.valid) {
        completeTbFetch(outputs.ibus);
    }
    if (outputs.dbus.valid) {
        completeTbData(outputs.dbus);
    }

    tbVeuResponsePins.valid = outputs.veu.valid;
    tbVeuResponsePins.readData = outputs.veu.readData;
    tbVeuResponsePins.lockActive = outputs.lockActive;
}

void
PipelineMiniCPU::writeTbCycleTrace(
    const brs::TbCrossbarInputs &inputs,
    const brs::TbCrossbarOutputs &outputs)
{
    if (!cycleTrace.is_open()) {
        return;
    }

    const PipelineRetireEvent &retire = core.lastRetireEvent();
    const unsigned dbusReadLane =
        static_cast<unsigned>((pendingDataAddr >> 2) & 0x3u);
    const unsigned stallMask = core.spiritExecuteStalled() ? 0x7u :
        ((core.stall_pc || core.stall_ifid) ? 0x3u : 0u);
    cycleTrace << "edge=" << elapsedClockEdges
        << " reset=0"
        << " cpu_cycle=" << core.getCycle()
        << " ibus_req=" << inputs.ibus.valid
        << " ibus_addr=0x" << std::hex << inputs.ibus.address
        << " ibus_re=" << inputs.ibus.read
        << " ibus_resp=" << outputs.ibus.valid
        << " ibus_r0=0x" << outputs.ibus.readData[0]
        << " ibus_r1=0x" << outputs.ibus.readData[1]
        << " ibus_r2=0x" << outputs.ibus.readData[2]
        << " ibus_r3=0x" << outputs.ibus.readData[3]
        << " dbus_req=" << std::dec << inputs.dbus.valid
        << " dbus_addr=0x" << std::hex << inputs.dbus.address
        << " dbus_re=" << std::dec << inputs.dbus.read
        << " dbus_we=" << inputs.dbus.write
        << " dbus_wstrb=0x" << std::hex << inputs.dbus.writeStrobe
        << " dbus_wdata=0x" << inputs.dbus.writeData[0]
        << " dbus_resp=" << std::dec << outputs.dbus.valid
        << " dbus_rdata=0x" << std::hex
        << outputs.dbus.readData[dbusReadLane]
        << " grant=" << std::dec
        << static_cast<unsigned>(outputs.grantedMaster)
        << " retire=" << retire.valid
        << " retire_pc=0x" << std::hex << retire.pc
        << " retire_instr=0x" << retire.instr
        << " wb_we=" << std::dec << retire.regWrite
        << " wb_fp=" << retire.fpWrite
        << " wb_rd=" << static_cast<unsigned>(retire.rd)
        << " wb_data=0x" << std::hex << retire.data
        << " stall_mask=0x" << stallMask
        << " stall_pc=" << std::dec << core.stall_pc
        << " stall_ifid=" << core.stall_ifid
        << " bubble_idex=" << core.bubble_idex
        << " stall_veu=" << core.veuStalled()
        << " stall_mdu=" << core.mduStalled()
        << " stall_lsu=" << core.lsuStalled()
        << " stall_fp=" << core.fpStalled()
        << " redirect=" << core.redirect_pc
        << " redirect_target=0x" << std::hex << core.redirect_target
        << " done=" << std::dec << tbDoneRequested
        << " done_value=" << tbDoneValue << '\n';
    cycleTrace.flush();
}

void
PipelineMiniCPU::startup()
{
    if (tbMemoryEnabled) {
        // Both RTL SRAM arrays start at zero before their image loaders run.
        // Clearing the shared sparse backing store gives raw-byte and
        // $readmemh-word paths identical zero-fill outside loaded bytes.
        tbCrossbar.clearMemory();
        uartLog.open(simout.resolve("uart.log"),
                     std::ios::out | std::ios::binary | std::ios::trunc);
        fatal_if(!uartLog.is_open(), "Failed to open RTL UART log");
        if (!cycleTraceFile.empty()) {
            cycleTrace.open(simout.resolve(cycleTraceFile),
                            std::ios::out | std::ios::trunc);
            fatal_if(!cycleTrace.is_open(),
                     "Failed to open cycle trace file: %s", cycleTraceFile);
            cycleTrace << "# brs-cycle-trace-v1 source=gem5 "
                       << "sampling=posedge-pre-nba reset_edges="
                       << resetCyclesRemaining << '\n';
            cycleTrace.flush();
        }
        if (!tbImemImageFile.empty()) {
            preloadedProgramSize = preloadTbRawImage(
                tbImemImageFile, tbInstBase, tbInstSize);
        }
        if (!tbDmemImageFile.empty()) {
            preloadTbRawImage(
                tbDmemImageFile, tbDataBase, tbDataStorageSize);
        }
    }

    if (!elfFile.empty()) {
        preloadElf();
    } else if (!programFile.empty()) {
        preloadProgramFunctional();
    } else if (preloadedProgram) {
        usePreloadedProgram();
    }

    if (!dmemHexFile.empty()) {
        preloadDataFunctional();
    }

    schedule(tickEvent, clockEdge(Cycles(1)));
}

void
PipelineMiniCPU::processTick()
{
    ++elapsedClockEdges;

    // Match aerith_tb_top.sv: the DUT remains inactive for 100 rising
    // edges by default. These reset edges advance gem5 time, but they do
    // not advance PipelineCore::cycle. The first active CPU edge is the
    // rising edge immediately after the configured reset interval.
    if (resetCyclesRemaining > 0) {
        --resetCyclesRemaining;
        if (cycleTrace.is_open()) {
            cycleTrace << "edge=" << elapsedClockEdges
                       << " reset=1 cpu_cycle=0\n";
            cycleTrace.flush();
        }
        if (tbMemoryEnabled && elapsedClockEdges >= maxCycles) {
            exitSimLoop("Aerith RTL testbench timeout");
            return;
        }
        schedule(tickEvent, clockEdge(Cycles(1)));
        return;
    }

    // Sample the VEU's current-cycle pins before either endpoint is clocked.
    // The CPU request callbacks below likewise represent signals driven during
    // this cycle and captured by the crossbar at its closing edge.
    const brs::VeuMemoryOutput veuMemory = tbMemoryEnabled ?
        core.evaluateVeuMemory() : brs::VeuMemoryOutput{};
    core.stepOneCycle();
    if (tbMemoryEnabled) {
        // The core drives its one-cycle request pulses during this cycle. The
        // crossbar captures them on this edge; registered responses are made
        // visible to PipelineCore on the following CPU cycle.
        processTbMemoryCycle(veuMemory);
        if (tbDoneRequested) {
            exitSimLoop(tbDoneValue == 2 ?
                "Aerith RTL testbench DONE (PASS)" :
                "Aerith RTL testbench DONE (ERROR)");
            return;
        }
    }

    pipeStats.cycle_count = core.getCycle();
    pipeStats.retired_inst_count = core.getRetiredInstCount();
    pipeStats.forward_count = core.getForwardCount();
    pipeStats.stall_count = core.getStallCount();
    pipeStats.flush_count = core.getFlushCount();
    pipeStats.icache_hit_count = icacheHitCount;
    pipeStats.icache_miss_count = icacheMissCount;
    pipeStats.ibus_req_count = core.getIbusReqCount();
    pipeStats.fetch_fifo_flush_count = core.getFetchFifoFlushCount();
    pipeStats.aligned_instr_count = core.getAlignedInstrCount();

    std::cout
        << "[PipelineMiniCPU] cycle=" << core.getCycle()
        << " pc=0x" << std::hex << core.getPC() << std::dec
        << " IFID=" << core.ifidValid()
        << " IDEX=" << core.idexValid()
        << " EXMEM=" << core.exmemValid()
        << " MEMWB=" << core.memwbValid()
        << " x1=" << core.getReg(1)
        << " x2=" << core.getReg(2)
        << " x3=" << core.getReg(3)
        << " x4=" << core.getReg(4)
        << " retiredInst=" << core.getRetiredInstCount()
        << " stall=" << core.getStallCount()
        << " fwd=" << core.getForwardCount()
        << " flush=" << core.getFlushCount()
        << " icHit=" << icacheHitCount
        << " icMiss=" << icacheMissCount
        << " ibusReq=" << core.getIbusReqCount()
        << " align=" << core.getAlignedInstrCount()
        << std::endl;

    if (!tbMemoryEnabled && core.done()) {
        exitSimLoop("PipelineMiniCPU completed test");
        return;
    }

    const uint64_t timeoutCycles = tbMemoryEnabled ?
        elapsedClockEdges : core.getCycle();
    if (timeoutCycles >= maxCycles) {
        exitSimLoop(tbMemoryEnabled ?
            "Aerith RTL testbench timeout" :
            "PipelineMiniCPU hit max_cycles");
        return;
    }

    schedule(tickEvent, clockEdge(Cycles(1)));
}

} // namespace gem5

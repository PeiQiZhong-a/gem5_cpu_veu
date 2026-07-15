#include "brs/pipeline_mini_cpu.hh"

#include "base/logging.hh"

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
    if (kind == PortKind::Veu) {
        return owner->completeTimingVeu(pkt);
    }
    return owner->completeTimingData(pkt);
}

void
PipelineMiniCPU::CpuRequestPort::recvReqRetry()
{
    if (kind == PortKind::Inst) {
        owner->retryInstFetch();
    } else if (kind == PortKind::Veu) {
        owner->retryVeuRequest();
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
      veuRequestorId(p.system->getRequestorId(this, "veu")),
      tbMemoryEnabled(p.tb_memory_enabled),
      tbImemImageFile(p.tb_imem_image_file),
      tbDmemImageFile(p.tb_dmem_image_file),
      tbCrossbar(brs::TbCrossbarModel::Config{
          p.tb_ibus_response_delay,
          p.tb_dbus_response_delay,
          p.tb_veu_pipeline_stages,
          static_cast<uint32_t>(p.tb_inst_base),
          static_cast<uint32_t>(p.tb_inst_size),
          static_cast<uint32_t>(p.tb_data_base),
          static_cast<uint32_t>(p.tb_data_size)}),
      instPort(name() + ".inst_port", this, CpuRequestPort::PortKind::Inst),
      dataPort(name() + ".data_port", this, CpuRequestPort::PortKind::Data),
      veuPort(name() + ".veu_port", this, CpuRequestPort::PortKind::Veu),
      icacheEnabled(p.icache_enabled),
      icacheSize(p.icache_size),
      icacheLineSize(p.icache_line_size),
      icacheNumLines(0),
      frontendBurstBytes(p.frontend_burst_bytes)
{
    fatal_if(frontendBurstBytes != 16,
             "PipelineMiniCPU frontend currently models 16-byte RV-NEW bursts");
    core.configureFrontend(p.instr_fifo_depth, p.frontend_burst_bytes);
    core.configureFakeVeu(
        p.fake_veu_latency, p.fake_veu_response_data);
    brs::VeuTimingConfig veuConfig;
    veuConfig.inputFifoDepth = p.veu_input_fifo_depth;
    veuConfig.executeLatency = p.veu_execute_latency;
    veuConfig.startupCycles = p.veu_startup_cycles;
    veuConfig.finishCycles = p.veu_finish_cycles;
    core.configureTimingVeu(veuConfig);
    core.setTimingVeuMemoryRequestCallback(
        [this](const brs::TimingVeuMemoryRequest &request) {
            return requestVeuTiming(request);
        });
    if (p.veu_model == "timing") {
        core.useTimingVeuEndpoint();
    } else if (p.veu_model == "fake") {
        core.useFakeVeuEndpoint();
    } else {
        fatal("Unsupported veu_model '%s': expected fake or timing",
              p.veu_model.c_str());
    }

    fatal_if(tbMemoryEnabled && icacheEnabled,
             "RTL-testbench memory mode requires the gem5-side I-cache to be disabled");
    fatal_if(tbMemoryEnabled && !elfFile.empty(),
             "RTL-testbench memory mode requires a raw or hex instruction image, not ELF physProxy preload");
    fatal_if(p.tb_inst_base > 0xffffffffULL ||
             p.tb_data_base > 0xffffffffULL ||
             p.tb_inst_size > 0xffffffffULL ||
              p.tb_data_size > 0xffffffffULL,
             "RTL-testbench address map must fit in the 32-bit RTL bus");
    fatal_if(tbMemoryEnabled && p.veu_model == "timing",
             "TimingVEU uses a 256-bit gem5 memory port and is not compatible "
             "with the 128-bit Aerith RTL-testbench VEU TCM pins");

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
    if (if_name == "veu_port") {
        return veuPort;
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
        const unsigned byteInWord = addr & 0x3;
        panic_if(byteInWord + size > 4,
                 "RTL DBus access crosses a 32-bit word: addr=%#x size=%u",
                 addr, size);

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
            const unsigned blockByte = addr & 0xf;
            for (unsigned byte = 0; byte < size; ++byte) {
                const unsigned destination = blockByte + byte;
                const unsigned word = destination / 4;
                const unsigned shift = (destination % 4) * 8;
                tbDbusPulse.writeStrobe |= uint16_t{1} << destination;
                tbDbusPulse.writeData[word] |=
                    ((writeData >> (byte * 8)) & 0xff) << shift;
            }
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

    Request::Flags flags;
    auto req = std::make_shared<Request>(
        addr, size, flags, dataRequestorId);
    auto pkt = new Packet(req, isWrite ? MemCmd::WriteReq : MemCmd::ReadReq);
    pkt->allocate();

    if (isWrite) {
        switch (size) {
          case 1:
            pkt->setLE<uint8_t>(static_cast<uint8_t>(writeData & 0xff));
            break;
          case 2:
            pkt->setLE<uint16_t>(static_cast<uint16_t>(writeData & 0xffff));
            break;
          case 4:
            pkt->setLE<uint32_t>(writeData);
            break;
          default:
            panic("Unsupported data store size: %u", size);
        }
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
        const unsigned shift = (pendingDataAddr & 0x3) * 8;
        data = response.readData[word] >> shift;
        if (pendingDataSize < 4) {
            data &= (uint32_t{1} << (pendingDataSize * 8)) - 1;
        }
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
        switch (pendingDataSize) {
          case 1:
            data = pkt->getLE<uint8_t>();
            break;
          case 2:
            data = pkt->getLE<uint16_t>();
            break;
          case 4:
            data = pkt->getLE<uint32_t>();
            break;
          default:
            panic("Unsupported data load size: %u", pendingDataSize);
        }
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

bool
PipelineMiniCPU::requestVeuTiming(
    const brs::TimingVeuMemoryRequest &request)
{
    if (pendingVeuReq != nullptr) {
        return false;
    }

    Request::Flags flags;
    auto req = std::make_shared<Request>(
        request.addr, brs::VeuVectorBytes, flags, veuRequestorId);
    auto pkt = new Packet(req, request.isWrite ? MemCmd::WriteReq :
                          MemCmd::ReadReq);
    pkt->allocate();

    if (request.isWrite) {
        std::memcpy(pkt->getPtr<uint8_t>(), request.data.data(),
                    brs::VeuVectorBytes);
    }

    pendingVeuAddr = request.addr;
    pendingVeuIsWrite = request.isWrite;
    pendingVeuReq = pkt;
    veuReqRetry = false;

    if (veuPort.sendTimingReq(pkt)) {
        pendingVeuReq = nullptr;
    } else {
        veuReqRetry = true;
    }

    return true;
}

bool
PipelineMiniCPU::completeTimingVeu(PacketPtr pkt)
{
    panic_if(pkt->isError(), "Timing VEU data access failed: %s",
             pkt->print());

    if (pendingVeuIsWrite) {
        core.acceptVeuMemoryWrite(static_cast<uint32_t>(pendingVeuAddr));
    } else {
        std::array<uint8_t, brs::VeuVectorBytes> data = {};
        const uint8_t *bytes = pkt->getConstPtr<uint8_t>();
        std::copy(bytes, bytes + brs::VeuVectorBytes, data.begin());
        core.acceptVeuMemoryRead(static_cast<uint32_t>(pendingVeuAddr), data);
    }

    pendingVeuReq = nullptr;
    veuReqRetry = false;
    pendingVeuAddr = 0;
    pendingVeuIsWrite = false;

    delete pkt;
    return true;
}

void
PipelineMiniCPU::retryVeuRequest()
{
    if (!pendingVeuReq || !veuReqRetry) {
        return;
    }

    PacketPtr pkt = pendingVeuReq;
    if (veuPort.sendTimingReq(pkt)) {
        pendingVeuReq = nullptr;
        veuReqRetry = false;
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
PipelineMiniCPU::preloadTbRawImage(const std::string &path, Addr base)
{
    std::ifstream input(path, std::ios::binary);
    fatal_if(!input.is_open(), "Failed to open RTL-testbench raw image: %s",
             path);

    Addr size = 0;
    char byte = 0;
    while (input.get(byte)) {
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
PipelineMiniCPU::startup()
{
    if (tbMemoryEnabled) {
        if (!tbImemImageFile.empty()) {
            preloadedProgramSize = preloadTbRawImage(tbImemImageFile, textBase);
        }
        if (!tbDmemImageFile.empty()) {
            preloadTbRawImage(tbDmemImageFile, dmemBase);
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
    // Match spirit_top_tb.sv: the DUT remains inactive for ten rising
    // edges by default. These reset edges advance gem5 time, but they do
    // not advance PipelineCore::cycle, just as sim_cycle stays at zero
    // while rstn is asserted in the RTL testbench.
    if (resetCyclesRemaining > 0) {
        --resetCyclesRemaining;
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
    pipeStats.veu_issue_count = core.getVeuIssueCount();
    pipeStats.veu_complete_count = core.getVeuCompleteCount();
    pipeStats.veu_csr_handshake_cycles = core.getVeuCsrHandshakeCycles();
    pipeStats.rv_dmem_blocked_by_veu_cycles =
        core.getRvDmemBlockedByVeuCycles();
    pipeStats.veu_operation_start_count =
        core.getTimingVeuOperationStarts();
    pipeStats.veu_operation_complete_count =
        core.getTimingVeuOperationCompletes();
    pipeStats.veu_busy_cycles = core.getTimingVeuBusyCycles();
    pipeStats.veu_load_wait_cycles = core.getTimingVeuLoadWaitCycles();
    pipeStats.veu_execute_cycles = core.getTimingVeuExecuteCycles();
    pipeStats.veu_store_wait_cycles = core.getTimingVeuStoreWaitCycles();
    pipeStats.veu_chunks = core.getTimingVeuChunks();
    pipeStats.veu_memory_reads = core.getTimingVeuMemoryReads();
    pipeStats.veu_memory_writes = core.getTimingVeuMemoryWrites();

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

    if (core.done()) {
        exitSimLoop("PipelineMiniCPU completed test");
        return;
    }

    if (core.getCycle() >= maxCycles) {
        exitSimLoop("PipelineMiniCPU hit max_cycles");
        return;
    }

    schedule(tickEvent, clockEdge(Cycles(1)));
}

} // namespace gem5

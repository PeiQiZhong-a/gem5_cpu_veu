#include "brs/pipeline_mini_cpu.hh"

#include "base/logging.hh"
#include "base/output.hh"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>

#include "sim/sim_exit.hh"
#include "mem/packet_access.hh"

namespace gem5
{

namespace
{

template <std::size_t ByteCount>
std::string
sramDataHex(const std::array<uint8_t, ByteCount> &data)
{
    std::ostringstream value;
    value << std::hex << std::setfill('0');
    for (std::size_t byte = data.size(); byte > 0; --byte) {
        value << std::setw(2)
              << static_cast<unsigned>(data[byte - 1]);
    }
    return value.str();
}

template <typename Request>
std::string
sramWriteDataHex(const Request &request)
{
    return sramDataHex(request.writeData);
}

brs::DutKuiMemoryModel::Config
makeDutKuiMemoryConfig(const PipelineMiniCPUParams &params)
{
    brs::DutKuiMemoryModel::Config config;
    config.instBase = static_cast<uint32_t>(params.tb_inst_base);
    config.instSize = static_cast<uint32_t>(params.tb_inst_size);
    config.dataBase = static_cast<uint32_t>(params.tb_data_base);
    config.bankSize = params.tb_data_bank_size;
    config.bankCount = params.tb_data_bank_count;
    config.realBankCount = params.tb_data_real_bank_count;
    return config;
}

brs::NpuLpnpuMikuiMemoryModel::Config
makeNpuLpnpuMikuiMemoryConfig(const PipelineMiniCPUParams &params)
{
    brs::NpuLpnpuMikuiMemoryModel::Config config;
    config.instBase = static_cast<uint32_t>(params.tb_inst_base);
    config.instSize = static_cast<uint32_t>(params.tb_inst_size);
    config.dmaTopology =
        params.tb_memory_kind == "npu-lpnpu-mikui-dma";
    return config;
}

Addr
dutKuiDataStorageSize(const PipelineMiniCPUParams &params)
{
    const uint64_t configuredCapacity =
        static_cast<uint64_t>(params.tb_data_bank_size) *
        params.tb_data_real_bank_count;
    return std::min<Addr>(params.tb_data_size, configuredCapacity);
}

} // namespace

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
      configuredExternalIrq(p.irq_external),
      configuredSoftwareIrq(p.irq_software),
      configuredTimerIrq(p.irq_timer),
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
      tbMemoryKind(p.tb_memory_kind),
      tbImemImageFile(p.tb_imem_image_file),
      tbDmemImageFile(p.tb_dmem_image_file),
      tbInstBase(p.tb_inst_base),
      tbInstSize(p.tb_inst_size),
      tbDataBase(p.tb_data_base),
      tbDataStorageSize(
          p.tb_memory_kind == "npu-lpnpu-mikui" ?
              std::min<Addr>(p.tb_data_size, 0x00010000) :
          p.tb_memory_kind == "npu-lpnpu-mikui-dma" ?
              std::min<Addr>(p.tb_data_size, 0x00018000) :
              dutKuiDataStorageSize(p)),
      dmaPioEnabled(p.dma_pio_enabled),
      dmaPioBase(p.dma_pio_base),
      dmaPioSize(p.dma_pio_size),
      dutKuiMemory(makeDutKuiMemoryConfig(p)),
      npuLpnpuMikuiMemory(makeNpuLpnpuMikuiMemoryConfig(p)),
      cycleTraceFile(p.cycle_trace_file),
      cycleTraceCompact(p.cycle_trace_compact),
      instPort(name() + ".inst_port", this, CpuRequestPort::PortKind::Inst),
      dataPort(name() + ".data_port", this, CpuRequestPort::PortKind::Data),
      veuPort(name() + ".veu_port", this, CpuRequestPort::PortKind::Veu),
      dmaIrqPin(name() + ".dma_irq", 0, this),
      icacheEnabled(p.icache_enabled),
      icacheSize(p.icache_size),
      icacheLineSize(p.icache_line_size),
      icacheNumLines(0),
      frontendBurstBytes(p.frontend_burst_bytes)
{
    fatal_if(frontendBurstBytes != 16,
             "PipelineMiniCPU frontend currently models 16-byte RV-NEW bursts");
    if (tbMemoryEnabled && tbMemoryKind == "dut-kui") {
        fatal_if(p.tb_data_bank_count == 0 || p.tb_data_bank_count > 4,
                 "dut_kui data bank count must be in the range 1..4");
        fatal_if(p.tb_data_real_bank_count > p.tb_data_bank_count,
                 "dut_kui real bank count exceeds decoded bank count");
        fatal_if(static_cast<uint64_t>(p.tb_data_bank_size) *
                     p.tb_data_bank_count != p.tb_data_size,
                 "dut_kui data size must equal bank_size * bank_count");
    }
    core.configureFrontend(p.instr_fifo_depth, p.frontend_burst_bytes);
    core.configureTextEndTermination(true);
    core.configureEbreakTermination(p.ebreak_terminates);
    core.configureFakeVeu(
        p.fake_veu_latency, p.fake_veu_response_data);
    fatal_if(tbMemoryEnabled && tbMemoryKind != "dut-kui" &&
             tbMemoryKind != "npu-lpnpu-mikui" &&
             tbMemoryKind != "npu-lpnpu-mikui-dma",
             "Unsupported RTL memory model '%s'",
             tbMemoryKind.c_str());
    fatal_if(dmaPioEnabled && !npuLpnpuMikuiMemoryEnabled(),
             "External Mikui DMA requires npu-lpnpu-mikui memory mode");
    fatal_if(dmaPioEnabled && dmaPioSize < 0x10,
             "External Mikui DMA PIO window is too small");
    if (npuLpnpuMikuiMemoryEnabled()) {
        core.setInterruptInputs(0, false, false);
    } else {
        core.setInterruptInputs(
            configuredExternalIrq,
            configuredSoftwareIrq,
            configuredTimerIrq);
    }
    core.setDebugInputs(
        p.debug_halt, p.debug_halt_on_reset, p.debug_resume, p.debug_data0);
    core.setDebugInstruction(p.debug_instr, p.debug_instr_valid);
    brs::VeuTimingConfig veuConfig;
    veuConfig.inputFifoDepth = p.veu_input_fifo_depth;
    veuConfig.executeLatency = p.veu_execute_latency;
    veuConfig.executeII = p.veu_execute_ii;
    veuConfig.vsuLatency = p.veu_vsu_latency;
    veuConfig.startupCycles = p.veu_startup_cycles;
    veuConfig.lockStartDelayCycles = p.veu_lock_start_delay_cycles;
    veuConfig.finishCycles = p.veu_finish_cycles;
    veuConfig.timingProfilePath = p.veu_timing_profile;
    veuConfig.terminalBehaviorPath = p.veu_terminal_behavior;
    veuConfig.cycleTracePath = p.veu_cycle_trace;
    try {
        core.configureTimingVeu(veuConfig);
    } catch (const std::exception &error) {
        fatal("Invalid TimingVEU configuration: %s", error.what());
    }
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

    fatal_if(p.sau_model != "stub" && p.sau_model != "sau_n",
             "Unsupported sau_model '%s': expected stub or sau_n",
             p.sau_model.c_str());
    fatal_if(p.sau_model == "sau_n" &&
             (!tbMemoryEnabled || tbMemoryKind != "dut-kui"),
             "sau_model=%s requires rtl-dut-kui-tb mode",
             p.sau_model.c_str());
    if (p.sau_model == "sau_n") {
        sauNSau = std::make_unique<brs::SauNEndpoint>(
            dutKuiMemory, static_cast<uint32_t>(tbDataBase),
            static_cast<uint64_t>(tbDataStorageSize));
        core.attachSauEndpoint(*sauNSau);
    } else {
        core.useStubSauEndpoint();
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
    if (if_name == "dma_irq") {
        return dmaIrqPin;
    }
    return ClockedObject::getPort(if_name, idx);
}

void
PipelineMiniCPU::raiseInterruptPin(int id)
{
    panic_if(id != 0, "Unexpected PipelineMiniCPU interrupt pin %d", id);
    dmaIrqInput = true;
}

void
PipelineMiniCPU::lowerInterruptPin(int id)
{
    panic_if(id != 0, "Unexpected PipelineMiniCPU interrupt pin %d", id);
    dmaIrqInput = false;
}

bool
PipelineMiniCPU::dutKuiMemoryEnabled() const
{
    return tbMemoryEnabled;
}

bool
PipelineMiniCPU::npuLpnpuMikuiMemoryEnabled() const
{
    return tbMemoryEnabled &&
        (tbMemoryKind == "npu-lpnpu-mikui" ||
         tbMemoryKind == "npu-lpnpu-mikui-dma");
}

bool
PipelineMiniCPU::dmaPioMapped(Addr address) const
{
    return dmaPioEnabled && address >= dmaPioBase &&
        address - dmaPioBase < dmaPioSize;
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
    if (dutKuiMemoryEnabled()) {
        inst = npuLpnpuMikuiMemoryEnabled() ?
            npuLpnpuMikuiMemory.readWord(addr) :
            dutKuiMemory.readWord(addr);
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

    if (dutKuiMemoryEnabled()) {
        panic_if(fetchSize != 16,
                 "dut_kui IBus supports one 16-byte beat, got %u",
                 fetchSize);
        // Match Spirit's IBus pins and spirit_top_tb IMEM indexing: drive the
        // true fetch address, while the returned four-word block starts at
        // addr[31:2].
        pendingInstAddr = fetchAddr & ~Addr{0x3};
        pendingInstFetchAddr = fetchAddr;
        const brs::DutKuiIbusRequest request{
            static_cast<uint32_t>(fetchAddr)};
        const bool accepted = npuLpnpuMikuiMemoryEnabled() ?
            npuLpnpuMikuiMemory.acceptIbus(request) :
            dutKuiMemory.acceptIbus(request);
        if (!accepted) {
            return false;
        }
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
PipelineMiniCPU::completeDutKuiFetch(
    const brs::DutKuiIbusResponse &response)
{
    panic_if(!tbInstOutstanding,
             "dut_kui IBus returned without an outstanding fetch");
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
    if (tbDataOutstanding || pendingDataReq != nullptr) {
        return false;
    }

    if (dutKuiMemoryEnabled() && !dmaPioMapped(addr)) {
        panic_if(size != 1 && size != 2 && size != 4,
                 "Unsupported dut_kui data access size: %u", size);
        const unsigned byteInWord = addr & 0x3;

        pendingDataAddr = addr;
        pendingDataSize = size;
        pendingDataIsWrite = isWrite;
        pendingDataWriteValue = writeData;
        pendingDataUnaligned = byteInWord + size > 4;
        pendingDataBeatCount = pendingDataUnaligned ? 2 : 1;
        pendingDataBeatIndex = 0;
        pendingDataCurrentBeatAddress = addr & ~uint32_t{0x3};
        pendingDataReadValue = 0;

        auto makeRequest = [this](uint32_t beatAddress) {
            brs::DutKuiDbusRequest request;
            request.address = beatAddress;
            if (pendingDataIsWrite) {
                for (unsigned byte = 0; byte < pendingDataSize; ++byte) {
                    const uint32_t byteAddress =
                        pendingDataAddr + static_cast<uint32_t>(byte);
                    if ((byteAddress & ~uint32_t{0x3}) != beatAddress) {
                        continue;
                    }
                    const unsigned lane = byteAddress & 0x3;
                    request.writeStrobe |= uint8_t{1} << lane;
                    request.writeData |=
                        ((pendingDataWriteValue >> (byte * 8)) & 0xffu) <<
                        (lane * 8);
                }
            }
            return request;
        };

        brs::DutKuiDbusRequest request = makeRequest(
            pendingDataCurrentBeatAddress);
        if (!pendingDataUnaligned) {
            request.address = addr;
            if (isWrite) {
                request.writeStrobe = static_cast<uint8_t>(
                    ((uint32_t{1} << size) - 1) << byteInWord);
                request.writeData = writeData << (byteInWord * 8);
            }
        }
        const bool accepted = npuLpnpuMikuiMemoryEnabled() ?
            npuLpnpuMikuiMemory.acceptDbus(
                request, core.timingVeuOwnsSharedDmem()) :
            dutKuiMemory.acceptDbus(
                request, core.timingVeuOwnsSharedDmem());
        if (!accepted) {
            return false;
        }

        tbDataOutstanding = true;
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
PipelineMiniCPU::completeDutKuiData(
    const brs::DutKuiDbusResponse &response)
{
    panic_if(!tbDataOutstanding,
             "dut_kui DBus returned without an outstanding request");
    panic_if(response.isWrite != pendingDataIsWrite,
             "dut_kui DBus response type does not match outstanding request");

    uint32_t data = 0;
    if (pendingDataUnaligned && !pendingDataIsWrite) {
        for (unsigned byte = 0; byte < pendingDataSize; ++byte) {
            const uint32_t byteAddress =
                pendingDataAddr + static_cast<uint32_t>(byte);
            if ((byteAddress & ~uint32_t{0x3}) !=
                pendingDataCurrentBeatAddress) {
                continue;
            }
            const unsigned lane = byteAddress & 0x3;
            pendingDataReadValue |=
                ((response.readData >> (lane * 8)) & 0xffu) << (byte * 8);
        }
    } else if (!pendingDataIsWrite) {
        // PipelineCore::stageEX applies the RV32 load lane selection and
        // sign/zero extension.  Keep the complete 32-bit SRAM word here,
        // matching the regular gem5 timing-memory path; extracting the lane
        // here as well would shift lbu/lh results twice for nonzero offsets.
        data = response.readData;
    }

    if (pendingDataUnaligned &&
        pendingDataBeatIndex + 1 < pendingDataBeatCount) {
        ++pendingDataBeatIndex;
        pendingDataCurrentBeatAddress += 4;
        brs::DutKuiDbusRequest request;
        request.address = pendingDataCurrentBeatAddress;
        if (pendingDataIsWrite) {
            for (unsigned byte = 0; byte < pendingDataSize; ++byte) {
                const uint32_t byteAddress =
                    pendingDataAddr + static_cast<uint32_t>(byte);
                if ((byteAddress & ~uint32_t{0x3}) !=
                    pendingDataCurrentBeatAddress) {
                    continue;
                }
                const unsigned lane = byteAddress & 0x3;
                request.writeStrobe |= uint8_t{1} << lane;
                request.writeData |=
                    ((pendingDataWriteValue >> (byte * 8)) & 0xffu) <<
                    (lane * 8);
            }
        }
        const bool accepted = npuLpnpuMikuiMemoryEnabled() ?
            npuLpnpuMikuiMemory.acceptDbus(
                request, core.timingVeuOwnsSharedDmem()) :
            dutKuiMemory.acceptDbus(
                request, core.timingVeuOwnsSharedDmem());
        panic_if(!accepted,
                  "dut_kui DBus could not accept an unaligned continuation");
        return;
    }

    if (pendingDataUnaligned && !pendingDataIsWrite) {
        data = pendingDataReadValue;
        // PipelineCore's existing halfword selector follows the RTL rule of
        // selecting the high half for every non-zero byte offset.  Present a
        // split halfword in that lane so archive-compatible unaligned LH/LHU
        // accesses retain the same downstream interpretation.
        if (pendingDataSize == 2 && (pendingDataAddr & 0x3) != 0) {
            data <<= 16;
        }
    }
    core.acceptDataResponse(static_cast<uint32_t>(pendingDataAddr),
                            data, pendingDataIsWrite);
    tbDataOutstanding = false;
    pendingDataAddr = 0;
    pendingDataSize = 0;
    pendingDataIsWrite = false;
    pendingDataWriteValue = 0;
    pendingDataUnaligned = false;
    pendingDataBeatCount = 1;
    pendingDataBeatIndex = 0;
    pendingDataCurrentBeatAddress = 0;
    pendingDataReadValue = 0;
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

bool
PipelineMiniCPU::requestVeuTiming(
    const brs::TimingVeuMemoryRequest &request)
{
    if (dutKuiMemoryEnabled()) {
        fatal_if(request.address > 0xffffffffULL,
                 "dut_kui VEU request address exceeds 32 bits: %#llx",
                 static_cast<unsigned long long>(request.address));
        brs::DutKuiVeuRequest tbRequest;
        tbRequest.transactionId = request.transactionId;
        tbRequest.address = static_cast<uint32_t>(request.address);
        tbRequest.isWrite = request.isWrite;
        tbRequest.writeStrobe = request.writeStrobe;
        tbRequest.data = request.data;
        const bool accepted = npuLpnpuMikuiMemoryEnabled() ?
            npuLpnpuMikuiMemory.acceptVeu(tbRequest) :
            dutKuiMemory.acceptVeu(tbRequest);
        if (!accepted) {
            core.noteVeuMemoryRetry();
            return false;
        }
        return true;
    }

    if (pendingVeuReq != nullptr) {
        return false;
    }

    Request::Flags flags;
    auto req = std::make_shared<Request>(
        request.address, brs::VeuVectorBytes, flags, veuRequestorId);
    if (request.isWrite) {
        std::vector<bool> byteEnable(brs::VeuVectorBytes, false);
        for (unsigned byte = 0; byte < brs::VeuVectorBytes; ++byte) {
            byteEnable[byte] = request.writeStrobe & (uint32_t{1} << byte);
        }
        req->setByteEnable(byteEnable);
    }
    auto pkt = new Packet(req, request.isWrite ? MemCmd::WriteReq :
                          MemCmd::ReadReq);
    pkt->allocate();
    pkt->pushSenderState(
        new VeuSenderState(request.transactionId, request.isWrite));

    if (request.isWrite) {
        std::memcpy(pkt->getPtr<uint8_t>(), request.data.data(),
                    brs::VeuVectorBytes);
    }

    pendingVeuReq = pkt;
    veuReqRetry = false;
    ++veuPacketsInFlight;

    if (veuPort.sendTimingReq(pkt)) {
        pendingVeuReq = nullptr;
    } else {
        veuReqRetry = true;
        core.noteVeuMemoryRetry();
    }

    return true;
}

bool
PipelineMiniCPU::completeTimingVeu(PacketPtr pkt)
{
    panic_if(pkt->isError(), "Timing VEU data access failed: %s",
             pkt->print());

    auto *senderState = dynamic_cast<VeuSenderState *>(pkt->senderState);
    panic_if(!senderState, "Timing VEU response has no sender state");
    pkt->popSenderState();

    if (senderState->isWrite) {
        core.acceptVeuMemoryWrite(senderState->transactionId);
    } else {
        std::array<uint8_t, brs::VeuVectorBytes> data = {};
        const uint8_t *bytes = pkt->getConstPtr<uint8_t>();
        std::copy(bytes, bytes + brs::VeuVectorBytes, data.begin());
        core.acceptVeuMemoryRead(senderState->transactionId, data);
    }

    panic_if(veuPacketsInFlight == 0,
             "Timing VEU response count underflow");
    --veuPacketsInFlight;

    delete senderState;
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
    // rtl-dut-kui-tb consumes the same logic [31:0] $readmemh format for
    // instruction and data SRAM images. Keep the byte-token loader for the
    // other memory models, whose existing fixtures use one byte per token.
    const bool loaded = tbMemoryEnabled ?
        img.loadReadmemh32File(dmemHexFile) :
        img.loadHexFile(dmemHexFile);
    if (!loaded) {
        fatal("Failed to load DMEM hex file: %s", dmemHexFile);
    }

    const size_t decodedBytes = img.data.size();
    if (tbMemoryEnabled) {
        fatal_if(!img.trimZeroFilledTail(tbDataStorageSize),
                 "RTL DMEM $readmemh image has non-zero data beyond the "
                 "%llu-byte real-SRAM capacity (decoded size %llu)",
                 static_cast<unsigned long long>(tbDataStorageSize),
                 static_cast<unsigned long long>(decodedBytes));
        if (decodedBytes > img.data.size()) {
            inform("preloadDataFunctional: ignored %llu zero-fill bytes "
                   "beyond the real SRAM banks",
                   static_cast<unsigned long long>(
                       decodedBytes - img.data.size()));
        }
    }
    const size_t totalBytes = img.data.size();
    for (size_t i = 0; i < totalBytes; ++i) {
        Addr addr = dmemBase + i;
        if (dutKuiMemoryEnabled()) {
            if (npuLpnpuMikuiMemoryEnabled()) {
                npuLpnpuMikuiMemory.writeByte(
                    static_cast<uint32_t>(addr), img.data[i]);
                // top_mikui_tb initializes DATA_RAM0 and DATA_RAM1 from the
                // same four readmemh files.
                npuLpnpuMikuiMemory.writeByte(
                    static_cast<uint32_t>(addr + 0x00010000), img.data[i]);
            } else {
                dutKuiMemory.writeByte(
                    static_cast<uint32_t>(addr), img.data[i]);
            }
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
        if (npuLpnpuMikuiMemoryEnabled()) {
            npuLpnpuMikuiMemory.writeByte(
                static_cast<uint32_t>(base + size),
                static_cast<uint8_t>(byte));
        } else {
            dutKuiMemory.writeByte(static_cast<uint32_t>(base + size),
                                   static_cast<uint8_t>(byte));
        }
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
        if (npuLpnpuMikuiMemoryEnabled()) {
            npuLpnpuMikuiMemory.writeByte(
                static_cast<uint32_t>(base + byte), image.data[byte]);
        } else {
            dutKuiMemory.writeByte(static_cast<uint32_t>(base + byte),
                                   image.data[byte]);
        }
    }
    inform("preloadTbReadmemh32Image: wrote %llu bytes at %#x from %s",
           static_cast<unsigned long long>(image.data.size()), base, path);
    return image.data.size();
}

void
PipelineMiniCPU::processDutKuiMemoryCycle(
    const brs::SauMemoryOutput &sau)
{
    const bool mikui = npuLpnpuMikuiMemoryEnabled();
    const brs::DutKuiMemoryOutputs outputs = mikui ?
        npuLpnpuMikuiMemory.evaluate() : dutKuiMemory.evaluate();
    core.clockSauMemory(outputs.sau);
    // The frozen npu_lpnpu dut_mikui RV_CORE ties its interrupt inputs low;
    // crossbarDone is ownership control only and must not synthesize IRQ3.
    // Explicitly configured IRQs remain available for standalone CPU tests.
    if (mikui) {
        core.setInterruptInputs(0, false, false, dmaIrqInput);
    } else {
        core.setInterruptInputs(
            configuredExternalIrq,
            configuredSoftwareIrq,
            configuredTimerIrq);
    }
    core.evaluateOneCycle();
    writeDutKuiCycleTrace(sau, outputs);
    if (mikui) {
        npuLpnpuMikuiMemory.clockEdge(
            core.timingVeuOwnsSharedDmem(), sau);
    } else {
        dutKuiMemory.clockEdge(core.timingVeuOwnsSharedDmem(), sau);
    }

    // Commit responses sampled from the old output pins. They affect the next
    // evaluate phase, independent of C++ call order within this edge.
    if (outputs.ibus.valid) {
        completeDutKuiFetch(outputs.ibus);
    }
    if (outputs.dbus.valid) {
        completeDutKuiData(outputs.dbus);
    }
    if (outputs.veuRead.valid) {
        core.acceptVeuMemoryRead(outputs.veuRead.transactionId,
                                 outputs.veuRead.readData);
    }
    if (outputs.veuWrite.valid) {
        core.acceptVeuMemoryWrite(outputs.veuWrite.transactionId);
    }
    core.clockOneCycle();
}

void
PipelineMiniCPU::writeSauNOutputTrace()
{
    if (!cycleTrace.is_open() || !sauNSau) {
        return;
    }

    const uint64_t completed = sauNSau->operationCompleteCount();
    if (completed <= sauNOutputTraceOperations) {
        return;
    }
    const brs::SauNResolvedConfig *config = sauNSau->activeConfig();
    fatal_if(config == nullptr,
             "sau_n completed without an active configuration");

    cycleTrace << "event=sau_n_output"
        << " operation=" << completed
        << " output_base=0x" << std::hex << config->abi.outputBase
        << std::dec << " output_bytes=" << config->abi.outputBytes
        << " data_hex=" << std::hex << std::setfill('0');
    for (uint64_t byte = 0; byte < config->abi.outputBytes; ++byte) {
        cycleTrace << std::setw(2)
            << static_cast<unsigned>(dutKuiMemory.readByte(
                config->abi.outputBase + byte));
    }
    cycleTrace << std::setfill(' ') << std::dec << '\n';
    cycleTrace.flush();
    sauNOutputTraceOperations = completed;
}

void
PipelineMiniCPU::writeDutKuiCycleTrace(
    const brs::SauMemoryOutput &sau,
    const brs::DutKuiMemoryOutputs &outputs)
{
    if (!cycleTrace.is_open()) {
        return;
    }

    const PipelineRetireEvent &retire = core.lastRetireEvent();
    const brs::HcRequest &hcRequest = core.getHcRequest();
    const brs::HcResponse &hcResponse = core.getHcResponse();
    const bool mikui = npuLpnpuMikuiMemoryEnabled();
    const brs::DutKuiIbusRequest &ibus = mikui ?
        npuLpnpuMikuiMemory.currentIbusRequest() :
        dutKuiMemory.currentIbusRequest();
    const brs::DutKuiDbusRequest &dbus = mikui ?
        npuLpnpuMikuiMemory.currentDbusRequest() :
        dutKuiMemory.currentDbusRequest();
    const brs::DutKuiVeuRequest &veu = mikui ?
        npuLpnpuMikuiMemory.currentVeuRequest() :
        dutKuiMemory.currentVeuRequest();
    const unsigned converterState = mikui ?
        static_cast<unsigned>(npuLpnpuMikuiMemory.converterState()) :
        static_cast<unsigned>(dutKuiMemory.converterState());
    const unsigned crossbarState = mikui ?
        static_cast<unsigned>(npuLpnpuMikuiMemory.crossbarState()) :
        static_cast<unsigned>(dutKuiMemory.crossbarState());
    const bool ibusAccepted = mikui ?
        npuLpnpuMikuiMemory.ibusAcceptedThisTick() :
        dutKuiMemory.ibusAcceptedThisTick();
    const bool dbusAccepted = mikui ?
        npuLpnpuMikuiMemory.dbusAcceptedThisTick() :
        dutKuiMemory.dbusAcceptedThisTick();
    const bool veuAccepted = mikui ?
        npuLpnpuMikuiMemory.veuAcceptedThisTick() :
        dutKuiMemory.veuAcceptedThisTick();
    const bool dmaEnabled = dmaPioEnabled;
    const unsigned stallMask = core.spiritExecuteStalled() ? 0x7u :
        ((core.stall_pc || core.stall_ifid) ? 0x3u : 0u);

    if (cycleTraceCompact) {
        cycleTrace << std::dec << "edge=" << elapsedClockEdges
            << " reset=0"
            << " cpu_cycle=" << core.getCycle()
            << " cpu_pc=0x" << std::hex << core.getPC();

        if (hcRequest.hasTransaction()) {
            cycleTrace << " hc_req=1"
                << " hc_addr=0x" << hcRequest.csrAddr
                << " hc_re=" << std::dec << hcRequest.csrRead
                << " hc_we=" << hcRequest.csrWrite
                << " hc_write_type="
                << static_cast<unsigned>(hcRequest.writeType)
                << " hc_wdata=0x" << std::hex << hcRequest.writeData
                << " hc_ve_start=0x" << hcRequest.veStart
                << " hc_target=" << static_cast<unsigned>(core.getHcTarget());
        }
        if (hcResponse.valid) {
            cycleTrace << " hc_valid=1"
                << " hc_addr=0x" << std::hex << hcRequest.csrAddr
                << " hc_target=" << std::dec
                << static_cast<unsigned>(core.getHcTarget())
                << " hc_rdata=0x" << std::hex << hcResponse.readData;
        }

        if (sau.request.valid) {
            cycleTrace << " sau_sram_req=1"
                << " sau_sram_addr=0x" << std::hex << sau.request.address
                << " sau_sram_wstrb=0x" << sau.request.writeStrobe
                << " sau_sram_wdata=0x" << sramWriteDataHex(sau.request);
        }
        if (outputs.sau.valid) {
            cycleTrace << " sau_sram_resp=1"
                << " sau_sram_rdata=0x" << sramDataHex(outputs.sau.readData);
        }
        if (sau.crossbarStart) {
            cycleTrace << " sau_xbar_start=1";
        }
        if (sau.crossbarDone) {
            cycleTrace << " sau_xbar_done=1";
        }
        if (dmaEnabled && dmaIrqInput) {
            cycleTrace << " dma_irq=1";
        }
        if (outputs.masterDropped[
                static_cast<uint8_t>(brs::DutKuiDataMaster::Sau)]) {
            cycleTrace << " sau_sram_drop=1";
        }

        if (retire.valid) {
            cycleTrace << " retire=1"
                << " retire_pc=0x" << std::hex << retire.pc
                << " retire_instr=0x" << retire.instr
                << " wb_we=" << std::dec << retire.regWrite
                << " wb_fp=" << retire.fpWrite
                << " wb_rd=" << static_cast<unsigned>(retire.rd)
                << " wb_data=0x" << std::hex << retire.data;
        }
        cycleTrace << '\n';
        if ((elapsedClockEdges & 0x3ffu) == 0) {
            cycleTrace.flush();
        }
        return;
    }

    cycleTrace << "edge=" << elapsedClockEdges
        << " reset=0"
        << " phase=evaluate"
        << " platform=" << (dmaEnabled ?
            "rtl-npu-lpnpu-mikui-decompress-dma" :
            (mikui ? "rtl-npu-lpnpu-mikui" : "rtl-dut-kui-tb"))
        << " cpu_cycle=" << core.getCycle()
        << " cpu_pc=0x" << std::hex << core.getPC()
        << " converter_state_pre=" << std::dec
        << converterState
        << " xbar_state_pre="
        << crossbarState
        << " ibus_req=" << ibusAccepted
        << " ibus_addr=0x" << std::hex << ibus.address
        << " ibus_resp=" << std::dec << outputs.ibus.valid
        << " dbus_req=" << dbusAccepted
        << " dbus_addr=0x" << std::hex << dbus.address
        << " dbus_wstrb=0x" << static_cast<unsigned>(dbus.writeStrobe)
        << " dbus_wdata=0x" << dbus.writeData
        << " dbus_resp=" << std::dec << outputs.dbus.valid
        << " dbus_rdata=0x" << std::hex << outputs.dbus.readData
        << " veu_req=" << std::dec << veuAccepted
        << " veu_addr=0x" << std::hex << veu.address
        << " veu_write=" << std::dec << veu.isWrite
        << " veu_rresp=" << outputs.veuRead.valid
        << " veu_wresp=" << outputs.veuWrite.valid
        << " hc_req=" << hcRequest.hasTransaction()
        << " hc_addr=0x" << std::hex << hcRequest.csrAddr
        << " hc_re=" << std::dec << hcRequest.csrRead
        << " hc_we=" << hcRequest.csrWrite
        << " hc_write_type="
        << static_cast<unsigned>(hcRequest.writeType)
        << " hc_wdata=0x" << std::hex << hcRequest.writeData
        << " hc_ve_start=0x" << hcRequest.veStart
        << " hc_target=" << static_cast<unsigned>(core.getHcTarget())
        << " hc_valid=" << hcResponse.valid
        << " hc_rdata=0x" << std::hex << hcResponse.readData
        << " sau_sram_req=" << std::dec << sau.request.valid
        << " sau_sram_addr=0x" << std::hex << sau.request.address
        << " sau_sram_wstrb=0x" << sau.request.writeStrobe
        << " sau_sram_wdata=0x" << sramWriteDataHex(sau.request)
        << " sau_sram_rdata=0x" << sramDataHex(outputs.sau.readData)
        << " sau_xbar_start=" << std::dec << sau.crossbarStart
        << " sau_xbar_done=" << sau.crossbarDone
        << " sau_sram_resp=" << outputs.sau.valid
        << " sau_sram_accept="
        << outputs.masterAccepted[
            static_cast<uint8_t>(brs::DutKuiDataMaster::Sau)]
        << " sau_sram_drop="
        << outputs.masterDropped[
            static_cast<uint8_t>(brs::DutKuiDataMaster::Sau)]
        << " veu_sram_accept="
        << outputs.masterAccepted[
            static_cast<uint8_t>(brs::DutKuiDataMaster::Veu)]
        << " veu_sram_drop="
        << outputs.masterDropped[
            static_cast<uint8_t>(brs::DutKuiDataMaster::Veu)]
        << " xbar_same_bank_collision=" << outputs.sameBankCollision
        << " dma_enabled=" << dmaEnabled
        << " dma_irq=" << dmaIrqInput
        << " xbar_bank_req=0x" << std::hex
        << (static_cast<unsigned>(outputs.bankRequest[0]) |
            (static_cast<unsigned>(outputs.bankRequest[1]) << 1) |
            (static_cast<unsigned>(outputs.bankRequest[2]) << 2) |
            (static_cast<unsigned>(outputs.bankRequest[3]) << 3))
        << " sau_irq3=" << std::dec
        << (mikui ? 0u : ((configuredExternalIrq >> 3) & 1u))
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
        << " stall_sau=" << core.sauStalled()
        << " stall_veu=" << core.veuStalled()
        << " stall_mdu=" << core.mduStalled()
        << " stall_lsu=" << core.lsuStalled()
        << " stall_fp=" << core.fpStalled()
        << '\n';
    cycleTrace.flush();
}

void
PipelineMiniCPU::startup()
{
    if (tbMemoryEnabled) {
        const bool mikui = npuLpnpuMikuiMemoryEnabled();
        if (mikui) {
            npuLpnpuMikuiMemory.reset();
        } else {
            dutKuiMemory.reset();
        }
        if (!cycleTraceFile.empty()) {
            cycleTrace.open(simout.resolve(cycleTraceFile),
                            std::ios::out | std::ios::trunc);
            fatal_if(!cycleTrace.is_open(),
                     "Failed to open cycle trace file: %s",
                     cycleTraceFile);
            cycleTrace << "# brs-cycle-trace-v2 source=gem5 "
                       << "sampling=evaluate-before-clock "
                       << "platform=" << (dmaPioEnabled ?
                           "rtl-npu-lpnpu-mikui-decompress-dma" :
                           (mikui ? "rtl-npu-lpnpu-mikui" :
                            "rtl-dut-kui-tb"))
                       << " reset_edges=" << resetCyclesRemaining
                       << " trace_mode="
                       << (cycleTraceCompact ? "compact" : "full") << '\n';
            cycleTrace.flush();
        }
        if (!tbImemImageFile.empty()) {
            preloadedProgramSize = preloadTbRawImage(
                tbImemImageFile, tbInstBase, tbInstSize);
        }
        if (!tbDmemImageFile.empty()) {
            preloadTbRawImage(
                tbDmemImageFile, tbDataBase, tbDataStorageSize);
            if (tbMemoryKind == "npu-lpnpu-mikui") {
                preloadTbRawImage(
                    tbDmemImageFile,
                    brs::NpuLpnpuMikuiCrossbar::Bank1Base,
                    tbDataStorageSize);
            }
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

    // Reset edges advance gem5 time but do not advance PipelineCore::cycle.
    // The first active CPU edge follows the configured reset interval.
    if (resetCyclesRemaining > 0) {
        --resetCyclesRemaining;
        if (cycleTrace.is_open()) {
            cycleTrace << "edge=" << elapsedClockEdges
                       << " reset=1 cpu_cycle=0\n";
            cycleTrace.flush();
        }
        if (tbMemoryEnabled && elapsedClockEdges >= maxCycles) {
            exitSimLoop("RTL testbench timeout");
            return;
        }
        schedule(tickEvent, clockEdge(Cycles(1)));
        return;
    }

    const brs::SauMemoryOutput sauMemory = dutKuiMemoryEnabled() ?
        core.evaluateSauMemory() : brs::SauMemoryOutput{};
    if (dutKuiMemoryEnabled()) {
        processDutKuiMemoryCycle(sauMemory);
        writeSauNOutputTrace();
    } else {
        core.stepOneCycle();
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
    pipeStats.sau_issue_count = core.getSauIssueCount();
    pipeStats.sau_retire_count = core.getSauCompleteCount();
    pipeStats.sau_csr_handshake_cycles =
        core.getSauCsrHandshakeCycles();
    if (sauNSau) {
        pipeStats.sau_memory_requests = 0;
        pipeStats.sau_compute_wait_cycles = 0;
        pipeStats.sau_writeback_wait_cycles = 0;
        pipeStats.sau_operation_start_count =
            sauNSau->operationStartCount();
        pipeStats.sau_operation_complete_count =
            sauNSau->operationCompleteCount();
        pipeStats.sau_roi_start_cycle = sauNSau->roiStartCycle();
        pipeStats.sau_roi_end_cycle = sauNSau->roiEndCycle();
        pipeStats.sau_n_model_ticks = sauNSau->modelTickCount();
        pipeStats.sau_n_operation_start_count =
            sauNSau->operationStartCount();
        pipeStats.sau_n_operation_complete_count =
            sauNSau->operationCompleteCount();
        pipeStats.sau_n_roi_start_cycle = sauNSau->roiStartCycle();
        pipeStats.sau_n_roi_end_cycle = sauNSau->roiEndCycle();
        const auto *streamingStats = sauNSau->streamingStats();
        if (streamingStats) {
            pipeStats.sau_n_spad_read_requests_a =
                streamingStats->spadReadRequestsA;
            pipeStats.sau_n_spad_read_grants_a =
                streamingStats->spadReadGrantsA;
            pipeStats.sau_n_spad_read_responses_a =
                streamingStats->spadReadResponsesA;
            pipeStats.sau_n_spad_read_requests_b =
                streamingStats->spadReadRequestsB;
            pipeStats.sau_n_spad_read_grants_b =
                streamingStats->spadReadGrantsB;
            pipeStats.sau_n_spad_read_responses_b =
                streamingStats->spadReadResponsesB;
            pipeStats.sau_n_spad_read_requests_c =
                streamingStats->spadReadRequestsC;
            pipeStats.sau_n_spad_read_grants_c =
                streamingStats->spadReadGrantsC;
            pipeStats.sau_n_spad_read_responses_c =
                streamingStats->spadReadResponsesC;
            pipeStats.sau_n_spad_write_requests_d =
                streamingStats->spadWriteRequestsD;
            pipeStats.sau_n_spad_write_grants_d =
                streamingStats->spadWriteGrantsD;
            pipeStats.sau_n_b_buffer_hit_vectors =
                streamingStats->bBufferHitVectors;
            pipeStats.sau_n_b_buffer_switches =
                streamingStats->bBufferSwitches;
            pipeStats.sau_n_d_pending_peak =
                streamingStats->dPendingPeak;
            pipeStats.sau_n_output_elements =
                streamingStats->outputElements;
        }
    }
    const uint64_t sauStarts = sauNSau ?
        sauNSau->operationStartCount() : 0;
    if (sauStarts > observedSauOperationStarts) {
        observedSauOperationStarts = sauStarts;
        sauRoiStartRetiredInst = core.getRetiredInstCount();
    }
    const uint64_t sauCompletes = sauNSau ?
        sauNSau->operationCompleteCount() : 0;
    if (sauCompletes > observedSauOperationCompletes) {
        observedSauOperationCompletes = sauCompletes;
        sauRoiEndRetiredInst = core.getRetiredInstCount();
    }
    pipeStats.sau_roi_start_retired_inst = sauRoiStartRetiredInst;
    pipeStats.sau_roi_end_retired_inst = sauRoiEndRetiredInst;
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
    pipeStats.veu_status_active_cycles =
        core.timingVeu.statusActiveCycleCount();
    pipeStats.veu_lock_active_cycles =
        core.timingVeu.lockActiveCycleCount();
    pipeStats.veu_current_outstanding_reads =
        core.timingVeu.currentOutstandingReadCount();
    pipeStats.veu_max_outstanding_reads =
        core.timingVeu.maxOutstandingReadCount();
    pipeStats.veu_fifo1_max_occupancy = core.timingVeu.fifoMaxOccupancy(0);
    pipeStats.veu_fifo2_max_occupancy = core.timingVeu.fifoMaxOccupancy(1);
    pipeStats.veu_fifo3_max_occupancy = core.timingVeu.fifoMaxOccupancy(2);
    pipeStats.veu_fifo_empty_stalls = core.timingVeu.fifoEmptyStallCount();
    pipeStats.veu_fifo_full_stalls = core.timingVeu.fifoFullStallCount();
    pipeStats.veu_vfu_accepted = core.timingVeu.vfuAcceptedCount();
    pipeStats.veu_vfu_completed = core.timingVeu.vfuCompletedCount();
    pipeStats.veu_vfu_max_in_flight = core.timingVeu.maxVfuInFlightCount();
    pipeStats.veu_vfu_ii_stalls = core.timingVeu.vfuIIStallCount();
    pipeStats.veu_vsu_queue_stalls = core.timingVeu.vsuQueueStallCount();
    pipeStats.veu_store_priority_cycles = core.timingVeu.storePriorityCount();
    pipeStats.veu_reads_blocked_by_store = core.timingVeu.readBlockedByStoreCount();
    pipeStats.veu_masked_writes = core.timingVeu.maskedWriteCount();
    pipeStats.veu_zero_mask_skipped_writes =
        core.timingVeu.zeroMaskSkippedWriteCount();
    pipeStats.veu_retries = core.timingVeu.retryCount();
    pipeStats.veu_unexpected_responses = core.timingVeu.unexpectedResponseCount();
    pipeStats.veu_profile_hits = core.timingVeu.profileHitCount();
    pipeStats.veu_profile_misses = core.timingVeu.profileMissCount();
    pipeStats.veu_profile_fallbacks = core.timingVeu.profileFallbackCount();
    pipeStats.veu_terminal_behavior_uses =
        core.timingVeu.terminalBehaviorUseCount();
    pipeStats.veu_timing_rtl_sim_uses =
        core.timingVeu.rtlSimTimingUseCount();
    pipeStats.veu_timing_legacy_uses =
        core.timingVeu.legacyTimingUseCount();
    pipeStats.veu_timing_default_uses =
        core.timingVeu.defaultTimingUseCount();
    pipeStats.veu_control_timing_rtl_sim_uses =
        core.timingVeu.rtlSimControlTimingUseCount();
    pipeStats.veu_control_timing_default_uses =
        core.timingVeu.defaultControlTimingUseCount();
    pipeStats.veu_zero_length_noops = core.timingVeu.zeroLengthNoopCount();
    pipeStats.veu_illegal_operations = core.timingVeu.illegalOperationCount();
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

    if (core.done() && pendingVeuReq == nullptr &&
        veuPacketsInFlight == 0 && core.timingVeu.quiescent()) {
        // exitSimLoop() stops the event loop while this SimObject is still
        // alive.  Flush the trace explicitly so the terminating retire event
        // is visible to the post-run verifier, including compact traces
        // whose periodic flush boundary has not been reached yet.
        if (cycleTrace.is_open()) {
            cycleTrace.flush();
        }
        exitSimLoop("PipelineMiniCPU completed test");
        return;
    }

    const uint64_t timeoutCycles = tbMemoryEnabled ?
        elapsedClockEdges : core.getCycle();
    if (timeoutCycles >= maxCycles) {
        if (cycleTrace.is_open()) {
            cycleTrace.flush();
        }
        exitSimLoop(tbMemoryEnabled ?
            "RTL testbench timeout" :
            "PipelineMiniCPU hit max_cycles");
        return;
    }

    schedule(tickEvent, clockEdge(Cycles(1)));
}

} // namespace gem5

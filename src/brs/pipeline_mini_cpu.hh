#ifndef __BRS_PIPELINE_MINI_CPU_HH__
#define __BRS_PIPELINE_MINI_CPU_HH__

#include <cstdint>
#include <array>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "base/loader/object_file.hh"
#include "dev/intpin.hh"
#include "mem/packet.hh"
#include "mem/port.hh"
#include "mem/request.hh"
#include "params/PipelineMiniCPU.hh"
#include "sim/clocked_object.hh"
#include "sim/eventq.hh"
#include "sim/system.hh"

#include "brs/pipeline/pipeline_core.hh"
#include "brs/pipeline/program_image.hh"
#include "brs/pipeline_stats.hh"
#include "brs/memory/dut_kui_memory_model.hh"
#include "brs/memory/npu_lpnpu_mikui_memory_model.hh"
#include "brs/sau/sau_n_endpoint.hh"

namespace gem5
{

class PipelineMiniCPU : public ClockedObject
{
  private:
    class CpuRequestPort : public RequestPort
    {
      public:
        enum class PortKind
        {
            Inst,
            Data,
            Veu
        };

      private:
        PipelineMiniCPU *owner;
        PortKind kind;

      public:
        CpuRequestPort(const std::string &name, PipelineMiniCPU *owner,
                       PortKind kind)
          : RequestPort(name, owner), owner(owner), kind(kind)
        {}

        bool recvTimingResp(PacketPtr pkt) override;
        void recvReqRetry() override;
    };

    uint64_t maxCycles;
    uint64_t resetCyclesRemaining;
    uint64_t elapsedClockEdges = 0;
    Addr textBase;
    System *system;
    uint32_t configuredExternalIrq;
    bool configuredSoftwareIrq;
    bool configuredTimerIrq;
    PipelineCore core;
    EventFunctionWrapper tickEvent;
    std::string programFile;
    std::string elfFile;
    bool preloadedProgram;
    Addr preloadedProgramSize;
    std::string dmemHexFile;
    Addr dmemBase;
    PipelineStats pipeStats;
    RequestorID instRequestorId;
    RequestorID dataRequestorId;
    RequestorID veuRequestorId;

    bool tbMemoryEnabled;
    std::string tbMemoryKind;
    std::string tbImemImageFile;
    std::string tbDmemImageFile;
    Addr tbInstBase;
    Addr tbInstSize;
    Addr tbDataBase;
    Addr tbDataStorageSize;
    bool dmaPioEnabled;
    Addr dmaPioBase;
    Addr dmaPioSize;
    bool dmaIrqInput = false;
    brs::DutKuiMemoryModel dutKuiMemory;
    brs::NpuLpnpuMikuiMemoryModel npuLpnpuMikuiMemory;
    std::unique_ptr<brs::SauNEndpoint> sauNSau;
    uint64_t observedSauOperationStarts = 0;
    uint64_t observedSauOperationCompletes = 0;
    uint64_t sauRoiStartRetiredInst = 0;
    uint64_t sauRoiEndRetiredInst = 0;
    bool tbInstOutstanding = false;
    bool tbDataOutstanding = false;
    std::string cycleTraceFile;
    bool cycleTraceCompact;
    std::ofstream cycleTrace;
    uint64_t sauNOutputTraceOperations = 0;

    CpuRequestPort instPort;
    CpuRequestPort dataPort;
    CpuRequestPort veuPort;
    IntSinkPin<PipelineMiniCPU> dmaIrqPin;
    PacketPtr pendingInstFetch = nullptr;
    bool instFetchRetry = false;
    Addr pendingInstAddr = 0;
    Addr pendingInstFetchAddr = 0;

    PacketPtr pendingDataReq = nullptr;
    bool dataReqRetry = false;
    Addr pendingDataAddr = 0;
    unsigned pendingDataSize = 0;
    bool pendingDataIsWrite = false;
    uint32_t pendingDataWriteValue = 0;
    bool pendingDataUnaligned = false;
    unsigned pendingDataBeatCount = 1;
    unsigned pendingDataBeatIndex = 0;
    uint32_t pendingDataCurrentBeatAddress = 0;
    uint32_t pendingDataReadValue = 0;

    PacketPtr pendingVeuReq = nullptr;
    bool veuReqRetry = false;
    uint64_t veuPacketsInFlight = 0;

    struct VeuSenderState : public Packet::SenderState
    {
        uint64_t transactionId;
        bool isWrite;

        VeuSenderState(uint64_t transactionId, bool isWrite)
          : transactionId(transactionId), isWrite(isWrite)
        {}
    };

    struct ICacheLine
    {
        bool valid = false;
        Addr tag = 0;
        std::vector<uint8_t> bytes;
    };

    bool icacheEnabled;
    uint32_t icacheSize;
    uint32_t icacheLineSize;
    uint32_t icacheNumLines;
    std::vector<ICacheLine> icache;
    uint64_t icacheHitCount = 0;
    uint64_t icacheMissCount = 0;
    uint32_t frontendBurstBytes;

    bool fetchInstrFunctional(uint32_t addr, uint32_t &inst);
    bool requestInstrTiming(uint32_t addr);
    bool completeTimingFetch(PacketPtr pkt);
    void retryInstFetch();
    bool requestDataTiming(uint32_t addr, unsigned size,
                           bool isWrite, uint32_t writeData);
    bool completeTimingData(PacketPtr pkt);
    void completeDutKuiFetch(const brs::DutKuiIbusResponse &response);
    void completeDutKuiData(const brs::DutKuiDbusResponse &response);
    void retryDataRequest();
    bool requestVeuTiming(const brs::TimingVeuMemoryRequest &request);
    bool completeTimingVeu(PacketPtr pkt);
    void retryVeuRequest();
    void preloadElf();
    void preloadProgramFunctional();
    void usePreloadedProgram();
    void preloadDataFunctional();
    Addr preloadTbRawImage(
        const std::string &path, Addr base, Addr capacity);
    Addr preloadTbReadmemh32Image(
        const std::string &path, Addr base, Addr capacity);
    void writeDutKuiCycleTrace(
        const brs::SauMemoryOutput &sau,
        const brs::DutKuiMemoryOutputs &outputs);
    void writeSauNOutputTrace();
    void processDutKuiMemoryCycle(const brs::SauMemoryOutput &sau);
    bool dutKuiMemoryEnabled() const;
    bool npuLpnpuMikuiMemoryEnabled() const;
    bool dmaPioMapped(Addr address) const;
    Addr icacheLineBase(Addr addr) const;
    bool icacheLookup(Addr addr, uint32_t &inst);
    bool icacheLookupBlock(Addr fetchAddr, FetchBlock &block);
    void fillICacheLine(Addr blockAddr, const uint8_t *data, unsigned size);

    void processTick();

  public:
    PipelineMiniCPU(const PipelineMiniCPUParams &p);
    ~PipelineMiniCPU() override = default;

    Port &getPort(const std::string &if_name,
                  PortID idx = InvalidPortID) override;

    void raiseInterruptPin(int id);
    void lowerInterruptPin(int id);

    void startup() override;
};

} // namespace gem5

#endif

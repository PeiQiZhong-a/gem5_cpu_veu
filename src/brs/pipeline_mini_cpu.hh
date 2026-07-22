#ifndef __BRS_PIPELINE_MINI_CPU_HH__
#define __BRS_PIPELINE_MINI_CPU_HH__

#include <cstdint>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "base/loader/object_file.hh"
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
#include "brs/memory/tb_crossbar_model.hh"

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
            Data
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

    bool tbMemoryEnabled;
    std::string tbImemImageFile;
    std::string tbDmemImageFile;
    Addr tbInstBase;
    Addr tbInstSize;
    Addr tbDataBase;
    Addr tbDataStorageSize;
    brs::TbCrossbarModel tbCrossbar;
    brs::TbBusRequest tbIbusPulse;
    brs::TbBusRequest tbDbusPulse;
    bool tbInstOutstanding = false;
    bool tbDataOutstanding = false;
    bool tbDoneRequested = false;
    uint32_t tbDoneValue = 0;
    brs::VeuMemoryResponse tbVeuResponsePins;
    std::ofstream uartLog;
    std::string cycleTraceFile;
    std::ofstream cycleTrace;

    CpuRequestPort instPort;
    CpuRequestPort dataPort;
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
    void completeTbFetch(const brs::TbBusResponse &response);
    void retryInstFetch();
    bool requestDataTiming(uint32_t addr, unsigned size,
                           bool isWrite, uint32_t writeData);
    bool completeTimingData(PacketPtr pkt);
    void completeTbData(const brs::TbBusResponse &response);
    void retryDataRequest();
    void preloadElf();
    void preloadProgramFunctional();
    void usePreloadedProgram();
    void preloadDataFunctional();
    Addr preloadTbRawImage(
        const std::string &path, Addr base, Addr capacity);
    Addr preloadTbReadmemh32Image(
        const std::string &path, Addr base, Addr capacity);
    void processTbMemoryCycle(const brs::VeuMemoryOutput &veu);
    void writeTbCycleTrace(
        const brs::TbCrossbarInputs &inputs,
        const brs::TbCrossbarOutputs &outputs);
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

    void startup() override;
};

} // namespace gem5

#endif

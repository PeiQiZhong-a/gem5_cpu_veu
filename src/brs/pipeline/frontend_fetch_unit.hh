#ifndef __BRS_PIPELINE_FRONTEND_FETCH_UNIT_HH__
#define __BRS_PIPELINE_FRONTEND_FETCH_UNIT_HH__

#include <array>
#include <cstdint>
#include <vector>

namespace gem5
{

struct FetchBlock
{
    uint32_t fetchAddr = 0;
    uint32_t blockAddr = 0;
    std::array<uint32_t, 4> words = {};
};

class FrontendAligner
{
  public:
    struct Input
    {
        bool fetchInstrValid = false;
        uint32_t fetchInstrBits = 0;
        uint32_t fetchInstrAddr = 0;
        bool flush = false;
    };

    struct Output
    {
        uint32_t alignedInstrBits = 0;
        uint32_t nextInstrAddr = 0;
        bool stopFetch = false;
        bool alignedInstrValid = false;
        bool lowHalfWordInvalid = false;
        bool instrNeedConcat = false;
    };

    void reset(uint32_t pcReset);
    Output step(const Input &in);
    bool needsConcat() const { return instrNeedConcat; }

  private:
    bool stopFetch = false;
    uint16_t fetchInstrBitsH = 0;
    bool instrNeedConcat = false;
    uint32_t alignedInstrBits = 0;
    uint32_t nextInstrAddr = 0;
    bool alignedInstrValid = false;
    bool lowHalfWordInvalid = false;
};

class FetchFifo
{
  public:
    struct Entry
    {
        uint32_t addr = 0;
        uint32_t word = 0;
    };

    struct CycleInput
    {
        bool flushAll = false;
        bool flushExceptFirst = false;
        bool pop = false;
        unsigned pushCount = 0;
        std::array<Entry, 4> pushEntries = {};
    };

    struct CycleOutput
    {
        bool empty = true;
        bool full = false;
        unsigned count = 0;
        Entry rdata = {};
    };

    explicit FetchFifo(unsigned depth = 12);

    void reset();
    void flush();
    bool pushBlock(const FetchBlock &block);
    CycleOutput step(const CycleInput &in);
    bool empty() const { return cntStatus == 0; }
    bool full() const { return cntStatus >= depth; }
    unsigned count() const { return cntStatus; }
    unsigned capacityLeft() const { return depth - cntStatus; }
    const Entry &front() const { return entries[readPointer]; }
    void pop();

  private:
    unsigned depth;
    std::vector<Entry> entries;
    unsigned readPointer = 0;
    unsigned writePointer = 0;
    unsigned cntStatus = 0;
};

class FetchBusUnit
{
  public:
    void reset(uint32_t pc);
    void flush(uint32_t pc);
    bool canRequest() const { return !inFlight; }
    uint32_t requestFetchAddr() const { return nextFetchAddr; }
    uint32_t requestBlockAddr() const;
    void markRequestIssued();
    bool acceptResponse(const FetchBlock &block);

  private:
    bool inFlight = false;
    bool discardInFlight = false;
    uint32_t inFlightFetchAddr = 0;
    uint32_t inFlightBlockAddr = 0;
    uint32_t nextFetchAddr = 0;
};

class FrontendFetchUnit
{
  public:
    struct Config
    {
        unsigned fifoDepth = 12;
        unsigned burstBytes = 16;
    };

    struct Input
    {
        bool stall = false;
        bool redirect = false;
        uint32_t redirectTarget = 0;
        uint32_t textEnd = 0;
        bool responseValid = false;
        FetchBlock response;
    };

    struct Output
    {
        bool instValid = false;
        uint32_t pc = 0;
        uint32_t instr = 0;
        uint8_t instrLen = 4;
        bool requestValid = false;
        uint32_t requestAddr = 0;
        uint32_t requestFetchAddr = 0;
    };

    FrontendFetchUnit();
    explicit FrontendFetchUnit(const Config &config);

    void configure(const Config &newConfig);
    void reset(uint32_t startPc);
    Output step(const Input &in);
    void markRequestIssued();
    void flush(uint32_t targetPc);

    uint32_t getPC() const { return pc; }
    uint64_t getIbusReqCount() const { return ibusReqCount; }
    uint64_t getFifoFlushCount() const { return fifoFlushCount; }
    uint64_t getAlignedInstrCount() const { return alignedInstrCount; }

  private:
    void clearIfOutput();

    Config config;
    FetchFifo fifo;
    FetchBusUnit ibu;
    FrontendAligner aligner;
    uint32_t pc = 0;
    uint8_t resetEnd = 0;
    bool pfuInstrNeedUpdate = false;
    bool ifReadyQ = false;
    uint32_t ifPcQ = 0;
    uint32_t ifInstrQ = 0;
    uint8_t ifInstrLenQ = 4;
    uint64_t ibusReqCount = 0;
    uint64_t fifoFlushCount = 0;
    uint64_t alignedInstrCount = 0;
};

} // namespace gem5

#endif

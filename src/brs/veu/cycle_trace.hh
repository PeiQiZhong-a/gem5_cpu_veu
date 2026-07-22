#ifndef __BRS_VEU_CYCLE_TRACE_HH__
#define __BRS_VEU_CYCLE_TRACE_HH__

#include <cstdint>
#include <string>

namespace gem5
{
namespace brs
{

enum class CycleTraceEvent : uint8_t
{
    OperationStart,
    ZeroLengthNoop,
    StatusSet,
    StatusClear,
    LockStart,
    LockFinish,
    ReadRequest,
    ReadResponse,
    FifoPush,
    FifoPop,
    VfuAccept,
    VfuDone,
    VsuReady,
    StorePriority,
    WriteRequest,
    WriteResponse,
    OperationFinish,
    TimingProfileFallback,
    IllegalOperation
};

struct CycleTraceRecord
{
    uint64_t cycle = 0;
    CycleTraceEvent event = CycleTraceEvent::OperationStart;
    uint32_t pc = 0;
    uint32_t instruction = 0;
    std::string op;
    uint32_t requestedVlen = 0;
    uint32_t effectiveVlen = 0;
    int32_t chunk = -1;
    uint8_t source = 0;
    uint64_t transactionId = 0;
    uint64_t address = 0;
    std::string data;
    uint32_t writeStrobe = 0;
    uint32_t fifo1 = 0;
    uint32_t fifo2 = 0;
    uint32_t fifo3 = 0;
    uint32_t outstanding = 0;
    bool status = false;
    bool lock = false;
    std::string detail;
};

constexpr const char CycleTraceCsvHeader[] =
    "cycle,event,pc,instr,op,requested_vlen,effective_vlen,chunk,source,"
    "transaction_id,addr,data,wstrb,fifo1,fifo2,fifo3,outstanding,status,"
    "lock,detail";

constexpr const char *
cycleTraceEventName(CycleTraceEvent event)
{
    switch (event) {
      case CycleTraceEvent::OperationStart: return "operation_start";
      case CycleTraceEvent::ZeroLengthNoop: return "zero_length_noop";
      case CycleTraceEvent::StatusSet: return "status_set";
      case CycleTraceEvent::StatusClear: return "status_clear";
      case CycleTraceEvent::LockStart: return "lock_start";
      case CycleTraceEvent::LockFinish: return "lock_finish";
      case CycleTraceEvent::ReadRequest: return "read_request";
      case CycleTraceEvent::ReadResponse: return "read_response";
      case CycleTraceEvent::FifoPush: return "fifo_push";
      case CycleTraceEvent::FifoPop: return "fifo_pop";
      case CycleTraceEvent::VfuAccept: return "vfu_accept";
      case CycleTraceEvent::VfuDone: return "vfu_done";
      case CycleTraceEvent::VsuReady: return "vsu_ready";
      case CycleTraceEvent::StorePriority: return "store_priority";
      case CycleTraceEvent::WriteRequest: return "write_request";
      case CycleTraceEvent::WriteResponse: return "write_response";
      case CycleTraceEvent::OperationFinish: return "operation_finish";
      case CycleTraceEvent::TimingProfileFallback:
        return "timing_profile_fallback";
      case CycleTraceEvent::IllegalOperation: return "illegal_operation";
      default: return "unknown";
    }
}

} // namespace brs
} // namespace gem5

#endif

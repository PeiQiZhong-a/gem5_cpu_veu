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
    ResetRelease,
    Fetch,
    Decode,
    Retire,
    CbuRequest,
    CbuResponse,
    VeuStart,
    TcmReadRequest,
    TcmReadResponse,
    VfuInput,
    VfuDone,
    TcmWriteRequest,
    TcmWriteResponse,
    VeuFinish,
    IllegalInstruction,
    IllegalVeuOperation
};

struct CycleTraceRecord
{
    uint64_t cycle = 0;
    CycleTraceEvent event = CycleTraceEvent::ResetRelease;
    uint32_t pc = 0;
    uint32_t instruction = 0;
    uint32_t address = 0;
    std::string data;
    uint32_t mask = 0;
    std::string detail;
};

constexpr const char CycleTraceCsvHeader[] =
    "cycle,event,pc,instr,addr,data,mask,detail";

constexpr const char *
cycleTraceEventName(CycleTraceEvent event)
{
    switch (event) {
      case CycleTraceEvent::ResetRelease: return "RESET_RELEASE";
      case CycleTraceEvent::Fetch: return "FETCH";
      case CycleTraceEvent::Decode: return "DECODE";
      case CycleTraceEvent::Retire: return "RETIRE";
      case CycleTraceEvent::CbuRequest: return "CBU_REQ";
      case CycleTraceEvent::CbuResponse: return "CBU_RESP";
      case CycleTraceEvent::VeuStart: return "VEU_START";
      case CycleTraceEvent::TcmReadRequest: return "TCM_READ_REQ";
      case CycleTraceEvent::TcmReadResponse: return "TCM_READ_RESP";
      case CycleTraceEvent::VfuInput: return "VFU_INPUT";
      case CycleTraceEvent::VfuDone: return "VFU_DONE";
      case CycleTraceEvent::TcmWriteRequest: return "TCM_WRITE_REQ";
      case CycleTraceEvent::TcmWriteResponse: return "TCM_WRITE_RESP";
      case CycleTraceEvent::VeuFinish: return "VEU_FINISH";
      case CycleTraceEvent::IllegalInstruction: return "ILLEGAL_INSTR";
      case CycleTraceEvent::IllegalVeuOperation: return "ILLEGAL_VEU_OP";
      default: return "UNKNOWN";
    }
}

} // namespace brs
} // namespace gem5

#endif // __BRS_VEU_CYCLE_TRACE_HH__

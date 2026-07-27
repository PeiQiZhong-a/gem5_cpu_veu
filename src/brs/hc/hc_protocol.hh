#ifndef __BRS_HC_HC_PROTOCOL_HH__
#define __BRS_HC_HC_PROTOCOL_HH__

#include <cstdint>

namespace gem5
{
namespace brs
{

// CPU custom-instruction (HC) request/response signals shared by VEU and
// SAU. The field widths and directions match Spirit's CBU boundary.
enum class HcWriteType : uint8_t
{
    Write = 0,
    Set = 1,
    Clear = 2,
    VectorStart = 3
};

struct HcRequest
{
    uint16_t csrAddr = 0;
    bool csrRead = false;
    bool csrWrite = false;
    uint8_t writeType = 0;
    uint64_t writeData = 0;
    uint32_t veStart = 0;

    constexpr bool hasTransaction() const
    {
        return csrRead || csrWrite;
    }
};

struct HcResponse
{
    bool valid = false;
    uint32_t readData = 0;
};

constexpr uint64_t
packHcOperands(uint32_t operand1, uint32_t operand2)
{
    return static_cast<uint64_t>(operand1) |
           (static_cast<uint64_t>(operand2) << 32);
}

constexpr uint32_t
unpackHcOperand1(uint64_t writeData)
{
    return static_cast<uint32_t>(writeData);
}

constexpr uint32_t
unpackHcOperand2(uint64_t writeData)
{
    return static_cast<uint32_t>(writeData >> 32);
}

} // namespace brs
} // namespace gem5

#endif // __BRS_HC_HC_PROTOCOL_HH__

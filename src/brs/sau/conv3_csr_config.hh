#ifndef __BRS_SAU_CONV3_CSR_CONFIG_HH__
#define __BRS_SAU_CONV3_CSR_CONFIG_HH__

#include <array>
#include <cstdint>

#include "brs/sau/sau_protocol.hh"

namespace gem5
{
namespace brs
{

// Decoded and checked Conv3 configuration. The first group mirrors the four
// ABI payloads; the remaining fields are derived values used by the future
// endpoint. No field in this structure is a raw memory-bus transaction.
struct Conv3Config
{
    uint32_t inputBase = 0;
    uint32_t weightBase = 0;
    uint32_t biasBase = 0;
    uint32_t outputBase = 0;

    uint32_t inputH = 0;
    uint32_t inputW = 0;
    uint32_t batchN = 0;
    uint32_t inputC = 0;
    uint32_t outputC = 0;

    uint32_t abiVersion = 0;
    uint32_t padding = 0;
    uint32_t strideMinus1 = 0;
    uint32_t cutbit = 0;
    uint32_t kernelSize = 0;

    uint64_t outputH = 0;
    uint64_t outputW = 0;
    uint64_t inputBytes = 0;
    uint64_t weightBytes = 0;
    uint64_t biasBytes = 0;
    uint64_t outputBytes = 0;
    uint64_t inputEnd = 0;
    uint64_t weightEnd = 0;
    uint64_t biasEnd = 0;
    uint64_t outputEnd = 0;
};

enum class Conv3ConfigError : uint8_t
{
    None,
    Busy,
    UnsupportedOperation,
    InvalidSequence,
    InvalidDataMemoryRange,
    InvalidAbiVersion,
    InvalidMagic,
    StartNotSet,
    InvalidPadding,
    InvalidStride,
    InvalidCutbit,
    InvalidKernelSize,
    ReservedShapeBits,
    ReservedControlBits,
    InvalidShape,
    ShapeOverflow,
    AddressOverflow,
    AddressOutOfRange,
    BiasUnaligned,
    OverlappingRanges
};

const char *conv3ConfigErrorString(Conv3ConfigError error);

struct Conv3DecodeResult
{
    bool valid = false;
    Conv3Config config;
    std::array<uint64_t, 4> payloads{};
    Conv3ConfigError error = Conv3ConfigError::None;
};

struct Conv3WriteResult
{
    bool accepted = false;
    bool committed = false;
    Conv3ConfigError error = Conv3ConfigError::None;
};

class Conv3CsrConfig
{
  public:
    using Payloads = std::array<uint64_t, 4>;

    Conv3CsrConfig(uint32_t dataMemoryBase, uint64_t dataMemorySize);

    void reset();

    // The endpoint calls this only for a CPU-side SAU write instruction. The
    // operation must be Set1..Set4; all sequence and commit checks happen
    // here before an active configuration becomes visible.
    Conv3WriteResult write(SauInstruction operation, uint64_t payload);

    // Marks the active operation complete. The active snapshot remains
    // queryable, while the next transaction must still rewrite all four
    // shadow slots.
    bool completeActiveOperation();

    static Conv3DecodeResult decode(
        const Payloads &payloads,
        uint32_t dataMemoryBase,
        uint64_t dataMemorySize);

    // Pure validation entry point used by decode and boundary-focused tests.
    // It recomputes all derived shape, size, and address-range fields.
    static Conv3DecodeResult validateFields(
        Conv3Config fields,
        uint32_t dataMemoryBase,
        uint64_t dataMemorySize);

    bool busy() const { return operationBusy; }
    bool hasActiveConfig() const { return activeValid; }
    const Conv3Config *activeConfig() const
    {
        return activeValid ? &active : nullptr;
    }

    uint8_t shadowValidMask() const { return validMask; }
    uint64_t shadowWord(uint8_t slot) const;
    const Payloads &shadowWords() const { return shadow; }
    uint32_t dataMemoryBase() const { return memoryBase; }
    uint64_t dataMemorySize() const { return memorySize; }

  private:
    uint8_t nextSlot() const;

    uint32_t memoryBase = 0;
    uint64_t memorySize = 0;
    Payloads shadow{};
    uint8_t validMask = 0;
    Conv3Config active;
    bool activeValid = false;
    bool operationBusy = false;
};

} // namespace brs
} // namespace gem5

#endif // __BRS_SAU_CONV3_CSR_CONFIG_HH__

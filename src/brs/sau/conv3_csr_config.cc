#include "brs/sau/conv3_csr_config.hh"

#include <cstddef>
#include <initializer_list>
#include <limits>

namespace gem5
{
namespace brs
{
namespace
{

constexpr uint64_t AddressLimit = uint64_t{1} << 32;

struct AddressRange
{
    uint64_t begin = 0;
    uint64_t end = 0;
};

bool
checkedAdd(uint64_t lhs, uint64_t rhs, uint64_t &result)
{
    if (rhs > std::numeric_limits<uint64_t>::max() - lhs) {
        return false;
    }
    result = lhs + rhs;
    return true;
}

bool
checkedMultiply(uint64_t lhs, uint64_t rhs, uint64_t &result)
{
    if (lhs != 0 && rhs > std::numeric_limits<uint64_t>::max() / lhs) {
        return false;
    }
    result = lhs * rhs;
    return true;
}

bool
checkedProduct(std::initializer_list<uint64_t> factors, uint64_t &result)
{
    result = 1;
    for (const uint64_t factor : factors) {
        if (!checkedMultiply(result, factor, result)) {
            return false;
        }
    }
    return true;
}

bool
overlaps(const AddressRange &lhs, const AddressRange &rhs)
{
    return lhs.begin < rhs.end && rhs.begin < lhs.end;
}

Conv3DecodeResult
failure(Conv3ConfigError error, const Conv3Config &config)
{
    Conv3DecodeResult result;
    result.config = config;
    result.error = error;
    return result;
}

Conv3DecodeResult
failure(Conv3ConfigError error)
{
    return failure(error, Conv3Config{});
}

} // namespace

const char *
conv3ConfigErrorString(Conv3ConfigError error)
{
    switch (error) {
      case Conv3ConfigError::None:
        return "none";
      case Conv3ConfigError::Busy:
        return "Conv3 operation is busy";
      case Conv3ConfigError::UnsupportedOperation:
        return "operation is not Conv3 msetins1..4";
      case Conv3ConfigError::InvalidSequence:
        return "Conv3 slots must be written exactly once in order 1,2,3,4";
      case Conv3ConfigError::InvalidDataMemoryRange:
        return "configured data-memory range is empty or exceeds 32-bit address space";
      case Conv3ConfigError::InvalidAbiVersion:
        return "ABI version must be 1";
      case Conv3ConfigError::InvalidMagic:
        return "ABI magic must be 0xc3";
      case Conv3ConfigError::StartNotSet:
        return "start bit must be set";
      case Conv3ConfigError::InvalidPadding:
        return "padding must be 0 or 1";
      case Conv3ConfigError::InvalidStride:
        return "stride-minus-one must be 0 or 1";
      case Conv3ConfigError::InvalidCutbit:
        return "cutbit must be in the range 0..23";
      case Conv3ConfigError::InvalidKernelSize:
        return "kernel size must be 3";
      case Conv3ConfigError::ReservedShapeBits:
        return "msetins3 reserved bits are non-zero";
      case Conv3ConfigError::ReservedControlBits:
        return "msetins4 reserved bits are non-zero";
      case Conv3ConfigError::InvalidShape:
        return "Conv3 shape is outside the supported range or has no output";
      case Conv3ConfigError::ShapeOverflow:
        return "Conv3 shape or tensor size arithmetic overflowed";
      case Conv3ConfigError::AddressOverflow:
        return "Conv3 tensor address range overflowed 32 bits";
      case Conv3ConfigError::AddressOutOfRange:
        return "Conv3 tensor range is outside configured data memory";
      case Conv3ConfigError::BiasUnaligned:
        return "bias base must be 2-byte aligned";
      case Conv3ConfigError::OverlappingRanges:
        return "Conv3 tensor ranges overlap";
    }
    return "unknown Conv3 configuration error";
}

Conv3DecodeResult
Conv3CsrConfig::validateFields(
    Conv3Config fields,
    uint32_t dataMemoryBase,
    uint64_t dataMemorySize)
{
    if (fields.abiVersion != 1) {
        return failure(Conv3ConfigError::InvalidAbiVersion, fields);
    }
    if (fields.padding > 1) {
        return failure(Conv3ConfigError::InvalidPadding, fields);
    }
    if (fields.strideMinus1 > 1) {
        return failure(Conv3ConfigError::InvalidStride, fields);
    }
    if (fields.cutbit > 23) {
        return failure(Conv3ConfigError::InvalidCutbit, fields);
    }
    if (fields.kernelSize != 3) {
        return failure(Conv3ConfigError::InvalidKernelSize, fields);
    }
    if (fields.inputH == 0 || fields.inputH > 65535 ||
        fields.inputW == 0 || fields.inputW > 65535 ||
        fields.batchN == 0 || fields.batchN > 65535 ||
        fields.inputC == 0 || fields.inputC > 63 ||
        fields.outputC == 0 || fields.outputC > 16) {
        return failure(Conv3ConfigError::InvalidShape, fields);
    }

    uint64_t paddedH = 0;
    uint64_t paddedW = 0;
    uint64_t paddedExtent = 0;
    if (!checkedMultiply(fields.padding, 2, paddedExtent) ||
        !checkedAdd(fields.inputH, paddedExtent, paddedH) ||
        !checkedAdd(fields.inputW, paddedExtent, paddedW)) {
        return failure(Conv3ConfigError::ShapeOverflow, fields);
    }
    if (paddedH < fields.kernelSize || paddedW < fields.kernelSize) {
        return failure(Conv3ConfigError::InvalidShape, fields);
    }

    const uint64_t stride = fields.strideMinus1 + 1;
    const uint64_t outputNumeratorH = paddedH - fields.kernelSize;
    const uint64_t outputNumeratorW = paddedW - fields.kernelSize;
    if (!checkedAdd(outputNumeratorH / stride, 1, fields.outputH) ||
        !checkedAdd(outputNumeratorW / stride, 1, fields.outputW) ||
        fields.outputH == 0 || fields.outputW == 0) {
        return failure(Conv3ConfigError::ShapeOverflow, fields);
    }

    if (!checkedProduct({fields.batchN, fields.inputC, fields.inputH,
                         fields.inputW}, fields.inputBytes) ||
        !checkedProduct({fields.inputC, fields.kernelSize, fields.kernelSize,
                         fields.outputC}, fields.weightBytes) ||
        !checkedMultiply(fields.outputC, 2, fields.biasBytes) ||
        !checkedProduct({fields.batchN, fields.outputC, fields.outputH,
                         fields.outputW}, fields.outputBytes)) {
        return failure(Conv3ConfigError::ShapeOverflow, fields);
    }

    uint64_t dataMemoryEnd = 0;
    if (dataMemorySize == 0 ||
        !checkedAdd(dataMemoryBase, dataMemorySize, dataMemoryEnd) ||
        dataMemoryEnd > AddressLimit) {
        return failure(Conv3ConfigError::InvalidDataMemoryRange, fields);
    }

    if ((fields.biasBase & 1u) != 0) {
        return failure(Conv3ConfigError::BiasUnaligned, fields);
    }

    const std::array<uint32_t, 4> bases{
        fields.inputBase, fields.weightBase, fields.biasBase,
        fields.outputBase};
    const std::array<uint64_t, 4> sizes{
        fields.inputBytes, fields.weightBytes, fields.biasBytes,
        fields.outputBytes};
    std::array<AddressRange, 4> ranges{};
    for (size_t index = 0; index < ranges.size(); ++index) {
        ranges[index].begin = bases[index];
        if (!checkedAdd(ranges[index].begin, sizes[index],
                        ranges[index].end) ||
            ranges[index].end > AddressLimit) {
            return failure(Conv3ConfigError::AddressOverflow, fields);
        }
        if (ranges[index].begin < dataMemoryBase ||
            ranges[index].end > dataMemoryEnd) {
            return failure(Conv3ConfigError::AddressOutOfRange, fields);
        }
    }

    for (size_t lhs = 0; lhs < ranges.size(); ++lhs) {
        for (size_t rhs = lhs + 1; rhs < ranges.size(); ++rhs) {
            if (overlaps(ranges[lhs], ranges[rhs])) {
                return failure(Conv3ConfigError::OverlappingRanges, fields);
            }
        }
    }

    fields.inputEnd = ranges[0].end;
    fields.weightEnd = ranges[1].end;
    fields.biasEnd = ranges[2].end;
    fields.outputEnd = ranges[3].end;

    Conv3DecodeResult result;
    result.valid = true;
    result.config = fields;
    return result;
}

Conv3DecodeResult
Conv3CsrConfig::decode(
    const Payloads &payloads,
    uint32_t dataMemoryBase,
    uint64_t dataMemorySize)
{
    Conv3Config fields;
    fields.inputBase = static_cast<uint32_t>(payloads[0]);
    fields.weightBase = static_cast<uint32_t>(payloads[0] >> 32);
    fields.biasBase = static_cast<uint32_t>(payloads[1]);
    fields.outputBase = static_cast<uint32_t>(payloads[1] >> 32);

    fields.inputH = static_cast<uint32_t>(payloads[2] & 0xffff);
    fields.inputW = static_cast<uint32_t>((payloads[2] >> 16) & 0xffff);
    fields.batchN = static_cast<uint32_t>((payloads[2] >> 32) & 0xffff);
    fields.inputC = static_cast<uint32_t>((payloads[2] >> 48) & 0x3f);
    fields.outputC = static_cast<uint32_t>((payloads[2] >> 54) & 0x1f);
    if ((payloads[2] >> 59) != 0) {
        Conv3DecodeResult result =
            failure(Conv3ConfigError::ReservedShapeBits, fields);
        result.payloads = payloads;
        return result;
    }

    fields.abiVersion = static_cast<uint32_t>(payloads[3] & 0xf);
    fields.padding = static_cast<uint32_t>((payloads[3] >> 4) & 0x1);
    fields.strideMinus1 = static_cast<uint32_t>((payloads[3] >> 5) & 0x1);
    fields.cutbit = static_cast<uint32_t>((payloads[3] >> 6) & 0x1f);
    fields.kernelSize = static_cast<uint32_t>((payloads[3] >> 11) & 0x7);
    if (((payloads[3] >> 14) & 0x1ffff) != 0) {
        Conv3DecodeResult result =
            failure(Conv3ConfigError::ReservedControlBits, fields);
        result.payloads = payloads;
        return result;
    }
    if (((payloads[3] >> 32) & 0xffffff) != 0) {
        Conv3DecodeResult result =
            failure(Conv3ConfigError::ReservedControlBits, fields);
        result.payloads = payloads;
        return result;
    }
    if (((payloads[3] >> 31) & 0x1) == 0) {
        Conv3DecodeResult result =
            failure(Conv3ConfigError::StartNotSet, fields);
        result.payloads = payloads;
        return result;
    }
    if ((payloads[3] >> 56) != 0xc3) {
        Conv3DecodeResult result =
            failure(Conv3ConfigError::InvalidMagic, fields);
        result.payloads = payloads;
        return result;
    }

    Conv3DecodeResult result =
        validateFields(fields, dataMemoryBase, dataMemorySize);
    result.payloads = payloads;
    return result;
}

Conv3CsrConfig::Conv3CsrConfig(
    uint32_t dataMemoryBase,
    uint64_t dataMemorySize) :
    memoryBase(dataMemoryBase), memorySize(dataMemorySize)
{
    reset();
}

void
Conv3CsrConfig::reset()
{
    shadow.fill(0);
    validMask = 0;
    active = {};
    activeValid = false;
    operationBusy = false;
}

uint8_t
Conv3CsrConfig::nextSlot() const
{
    for (uint8_t slot = 1; slot <= 4; ++slot) {
        if ((validMask & (uint8_t{1} << (slot - 1))) == 0) {
            return slot;
        }
    }
    return 0;
}

Conv3WriteResult
Conv3CsrConfig::write(SauInstruction operation, uint64_t payload)
{
    Conv3WriteResult result;
    if (operationBusy) {
        result.error = Conv3ConfigError::Busy;
        return result;
    }
    if (!isSauSet(operation) || sauSlot(operation) > 4) {
        result.error = Conv3ConfigError::UnsupportedOperation;
        return result;
    }

    const uint8_t slot = sauSlot(operation);
    if (slot != nextSlot()) {
        result.error = Conv3ConfigError::InvalidSequence;
        return result;
    }

    if (slot < 4) {
        shadow[slot - 1] = payload;
        validMask |= uint8_t{1} << (slot - 1);
        result.accepted = true;
        return result;
    }

    Payloads candidate = shadow;
    candidate[3] = payload;
    const Conv3DecodeResult decoded =
        decode(candidate, memoryBase, memorySize);
    if (!decoded.valid) {
        result.error = decoded.error;
        return result;
    }

    active = decoded.config;
    activeValid = true;
    operationBusy = true;
    shadow.fill(0);
    validMask = 0;
    result.accepted = true;
    result.committed = true;
    return result;
}

bool
Conv3CsrConfig::completeActiveOperation()
{
    if (!operationBusy) {
        return false;
    }
    operationBusy = false;
    return true;
}

uint64_t
Conv3CsrConfig::shadowWord(uint8_t slot) const
{
    return slot >= 1 && slot <= 4 ? shadow[slot - 1] : 0;
}

} // namespace brs
} // namespace gem5

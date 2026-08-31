#include "brs/sau/sau_n_memory_view.hh"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

#include "sau_n/im2col_address.hh"

namespace gem5::brs
{
namespace
{

struct ExternalRegion
{
    uint64_t base = 0;
    uint64_t size = 0;
    std::string_view name;
};

uint64_t
checkedTensorSize(
    const sau_n::PipelineResolvedConfig &config,
    const sau_n::PipelineDerivedConfig &derived,
    std::string_view name)
{
    if (name == "A") {
        return sau_n::checkedMultiply(
            sau_n::checkedMultiply(
                sau_n::checkedMultiply(
                    config.im2col.n, config.im2col.c, "A tensor size"),
                config.im2col.h, "A tensor size"),
            config.im2col.w, "A tensor size");
    }
    if (name == "B") {
        return sau_n::checkedMultiply(
            derived.k, config.outChannels, "B tensor size");
    }
    if (name == "C") {
        return sau_n::checkedMultiply(
            config.outChannels, 2, "C tensor size");
    }
    if (name == "D") {
        return sau_n::checkedMultiply(
            sau_n::checkedMultiply(
                sau_n::checkedMultiply(
                    config.im2col.n, config.outChannels, "D tensor size"),
                config.im2col.outH, "D tensor size"),
            config.im2col.outW, "D tensor size");
    }
    throw std::invalid_argument("unknown external tensor region");
}

void
validateExternalRegions(
    const std::array<ExternalRegion, 4> &regions)
{
    auto ordered = regions;
    for (const auto &region : ordered) {
        const uint64_t end = sau_n::checkedAdd(
            region.base, region.size,
            std::string(region.name) + " external range");
        if (region.base > std::numeric_limits<uint32_t>::max() ||
            end > uint64_t{std::numeric_limits<uint32_t>::max()} + 1) {
            throw std::invalid_argument(
                std::string(region.name) +
                " external range exceeds 32-bit address space");
        }
    }
    std::sort(
        ordered.begin(), ordered.end(),
        [](const ExternalRegion &left, const ExternalRegion &right) {
            return left.base < right.base;
        });
    for (std::size_t index = 1; index < ordered.size(); ++index) {
        const uint64_t previousEnd = sau_n::checkedAdd(
            ordered[index - 1].base, ordered[index - 1].size,
            std::string(ordered[index - 1].name) + " external range");
        if (ordered[index].base < previousEnd) {
            throw std::invalid_argument(
                std::string("external tensor ranges overlap: ") +
                std::string(ordered[index - 1].name) + " and " +
                std::string(ordered[index].name));
        }
    }
}

uint64_t
chwOffset(
    const sau_n::PipelineResolvedConfig &config,
    uint64_t n, uint64_t c, uint64_t h, uint64_t w)
{
    uint64_t offset = sau_n::checkedAdd(
        sau_n::checkedMultiply(n, config.im2col.c, "A tensor offset"),
        c, "A tensor offset");
    offset = sau_n::checkedAdd(
        sau_n::checkedMultiply(offset, config.im2col.h, "A tensor offset"),
        h, "A tensor offset");
    offset = sau_n::checkedAdd(
        sau_n::checkedMultiply(offset, config.im2col.w, "A tensor offset"),
        w, "A tensor offset");
    return offset;
}

uint64_t
dOffset(
    const sau_n::PipelineResolvedConfig &config,
    uint64_t n, uint64_t outputChannel, uint64_t oh, uint64_t ow)
{
    uint64_t offset = sau_n::checkedAdd(
        sau_n::checkedMultiply(
            n, config.outChannels, "D tensor offset"),
        outputChannel, "D tensor offset");
    offset = sau_n::checkedAdd(
        sau_n::checkedMultiply(
            offset, config.im2col.outH, "D tensor offset"),
        oh, "D tensor offset");
    offset = sau_n::checkedAdd(
        sau_n::checkedMultiply(
            offset, config.im2col.outW, "D tensor offset"),
        ow, "D tensor offset");
    return offset;
}

} // anonymous namespace

SauNMemoryView::SauNMemoryView(
    const sau_n::PipelineResolvedConfig &config,
    DutKuiMemoryModel &memory,
    SauNTensorBaseAddresses bases)
    : memory(memory), tensorBases(bases),
      dimensions(sau_n::validateStreamingConfig(config))
{
    const std::array<ExternalRegion, 4> regions = {{
        {tensorBases.inputBase,
         checkedTensorSize(config, dimensions, "A"), "A"},
        {tensorBases.weightBase,
         checkedTensorSize(config, dimensions, "B"), "B"},
        {tensorBases.biasBase,
         checkedTensorSize(config, dimensions, "C"), "C"},
        {tensorBases.outputBase,
         checkedTensorSize(config, dimensions, "D"), "D"},
    }};
    validateExternalRegions(regions);

    const sau_n::ChwAddressMapper mapper(config.im2col);
    for (uint64_t n = 0; n < config.im2col.n; ++n) {
        for (uint64_t c = 0; c < config.im2col.c; ++c) {
            for (uint64_t h = 0; h < config.im2col.h; ++h) {
                for (uint64_t w = 0; w < config.im2col.w; ++w) {
                    const auto location = mapper.locate(n, c, h, w);
                    bind(
                        location.bank, location.row,
                        sau_n::checkedAdd(
                            tensorBases.inputBase,
                            chwOffset(config, n, c, h, w),
                            "A external address"),
                        "A");
                }
            }
        }
    }

    for (uint64_t k = 0; k < dimensions.k; ++k) {
        for (uint64_t oc = 0; oc < config.outChannels; ++oc) {
            const auto location = sau_n::bAddress(
                config, dimensions, k, oc);
            bind(
                location.bank, location.row,
                sau_n::checkedAdd(
                    tensorBases.weightBase,
                    sau_n::checkedAdd(
                        sau_n::checkedMultiply(
                            k, config.outChannels, "B external offset"),
                        oc, "B external offset"),
                    "B external address"),
                "B");
        }
    }

    for (uint64_t oc = 0; oc < config.outChannels; ++oc) {
        for (uint64_t byte = 0; byte < 2; ++byte) {
            const auto location = sau_n::cAddress(
                config, dimensions, oc, byte);
            bind(
                location.bank, location.row,
                sau_n::checkedAdd(
                    tensorBases.biasBase,
                    sau_n::checkedAdd(
                        sau_n::checkedMultiply(
                            oc, 2, "C external offset"),
                        byte, "C external offset"),
                    "C external address"),
                "C");
        }
    }

    for (uint64_t n = 0; n < config.im2col.n; ++n) {
        for (uint64_t oc = 0; oc < config.outChannels; ++oc) {
            for (uint64_t oh = 0; oh < config.im2col.outH; ++oh) {
                for (uint64_t ow = 0; ow < config.im2col.outW; ++ow) {
                    const auto location = sau_n::dAddress(
                        config, dimensions, n, oh, ow, oc);
                    bind(
                        location.bank, location.row,
                        sau_n::checkedAdd(
                            tensorBases.outputBase,
                            dOffset(config, n, oc, oh, ow),
                            "D external address"),
                        "D");
                }
            }
        }
    }
}

void
SauNMemoryView::bind(
    uint64_t bank, uint64_t row, uint64_t externalAddress,
    const char *tensor)
{
    if (bank >= sau_n::ScratchpadBanks ||
        row >= sau_n::ScratchpadRows) {
        throw std::out_of_range(
            std::string(tensor) + " mapping exceeds scratchpad bounds");
    }
    if (externalAddress > std::numeric_limits<uint32_t>::max()) {
        throw std::invalid_argument(
            std::string(tensor) + " mapping exceeds 32-bit address space");
    }
    const auto address = static_cast<uint32_t>(externalAddress);
    if (!memory.dataMapped(address)) {
        throw std::invalid_argument(
            std::string(tensor) + " mapping is outside configured data SRAM");
    }
    if (mapped[bank][row]) {
        throw std::logic_error(
            std::string("multiple tensor bytes map to scratchpad bank/row for ") +
            tensor);
    }
    mapped[bank][row] = true;
    addresses[bank][row] = address;
}

uint32_t
SauNMemoryView::requireAddress(uint64_t bank, uint64_t row) const
{
    if (bank >= sau_n::ScratchpadBanks ||
        row >= sau_n::ScratchpadRows) {
        throw std::out_of_range("scratchpad bank/row is out of range");
    }
    if (!mapped[bank][row]) {
        throw std::out_of_range(
            "scratchpad bank/row has no external tensor mapping");
    }
    return addresses[bank][row];
}

uint32_t
SauNMemoryView::address(uint64_t bank, uint64_t row) const
{
    return requireAddress(bank, row);
}

uint8_t
SauNMemoryView::read(uint64_t bank, uint64_t row) const
{
    return memory.readByte(requireAddress(bank, row));
}

void
SauNMemoryView::write(uint64_t bank, uint64_t row, uint8_t value)
{
    memory.writeByte(requireAddress(bank, row), value);
}

} // namespace gem5::brs

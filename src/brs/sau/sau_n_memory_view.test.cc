#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>
#include <vector>

#include "brs/sau/sau_n_memory_view.hh"
#include "sau_n/im2col_address.hh"
#include "sau_n/streaming_conv_pipeline_model.hh"

namespace gem5::brs
{
namespace
{

sau_n::PipelineResolvedConfig
simpleConfig()
{
    sau_n::PipelineResolvedConfig config;
    config.name = "external_memory_view";
    config.im2col.name = "external_memory_view_im2col";
    config.im2col.n = 1;
    config.im2col.c = 1;
    config.im2col.h = 3;
    config.im2col.w = 3;
    config.im2col.outH = 1;
    config.im2col.outW = 1;
    config.im2col.kernelH = 3;
    config.im2col.kernelW = 3;
    config.im2col.strideH = 1;
    config.im2col.strideW = 1;
    config.im2col.dilationH = 1;
    config.im2col.dilationW = 1;
    config.im2col.padTop = 0;
    config.im2col.padLeft = 0;
    config.outChannels = 1;
    config.cutbit = 0;
    return config;
}

SauNTensorBaseAddresses
bases()
{
    return {
        0x29130000,
        0x29132000,
        0x29132900,
        0x29132920,
    };
}

void
writeSimpleTensors(
    DutKuiMemoryModel &memory,
    const SauNTensorBaseAddresses &tensorBases,
    uint8_t activation)
{
    for (uint32_t index = 0; index < 9; ++index) {
        memory.writeByte(tensorBases.inputBase + index, activation);
        memory.writeByte(tensorBases.weightBase + index, 0xfe);
    }
    memory.writeByte(tensorBases.biasBase, 0xff);
    memory.writeByte(tensorBases.biasBase + 1, 0xff);
    memory.writeByte(tensorBases.outputBase, 0xa5);
}

void
runToDrain(sau_n::StreamingConvPipelineModel &model)
{
    for (uint64_t attempt = 0; attempt < 100000; ++attempt) {
        if (model.tick().drained) {
            return;
        }
    }
    throw std::runtime_error("external-backed streaming model did not drain");
}

TEST(SauNMemoryView, MapsAllTensorLayoutsToSharedByteAddresses)
{
    auto config = simpleConfig();
    config.im2col.n = 2;
    config.im2col.c = 2;
    config.im2col.w = 20;
    config.im2col.outW = 18;
    config.outChannels = 3;
    const auto tensorBases = bases();
    DutKuiMemoryModel memory;
    SauNMemoryView view(config, memory, tensorBases);
    const auto derived = view.derived();

    const sau_n::ChwAddressMapper mapper(config.im2col);
    for (uint64_t n = 0; n < config.im2col.n; ++n) {
        for (uint64_t c = 0; c < config.im2col.c; ++c) {
            for (uint64_t h = 0; h < config.im2col.h; ++h) {
                for (uint64_t w = 0; w < config.im2col.w; ++w) {
                    const auto location = mapper.locate(n, c, h, w);
                    const uint64_t offset =
                        (((n * config.im2col.c + c) *
                          config.im2col.h + h) *
                         config.im2col.w + w);
                    EXPECT_EQ(
                        view.address(location.bank, location.row),
                        tensorBases.inputBase + offset);
                }
            }
        }
    }

    for (uint64_t k = 0; k < derived.k; ++k) {
        for (uint64_t oc = 0; oc < config.outChannels; ++oc) {
            const auto location = sau_n::bAddress(
                config, derived, k, oc);
            EXPECT_EQ(
                view.address(location.bank, location.row),
                tensorBases.weightBase + k * config.outChannels + oc);
        }
    }

    for (uint64_t oc = 0; oc < config.outChannels; ++oc) {
        for (uint64_t byte = 0; byte < 2; ++byte) {
            const auto location = sau_n::cAddress(
                config, derived, oc, byte);
            EXPECT_EQ(
                view.address(location.bank, location.row),
                tensorBases.biasBase + oc * 2 + byte);
        }
    }

    for (uint64_t n = 0; n < config.im2col.n; ++n) {
        for (uint64_t oc = 0; oc < config.outChannels; ++oc) {
            for (uint64_t oh = 0; oh < config.im2col.outH; ++oh) {
                for (uint64_t ow = 0; ow < config.im2col.outW; ++ow) {
                    const auto output = sau_n::dAddress(
                        config, derived, n, oh, ow, oc);
                    const uint64_t offset =
                        (((n * config.outChannels + oc) *
                          config.im2col.outH + oh) *
                         config.im2col.outW + ow);
                    EXPECT_EQ(
                        view.address(output.bank, output.row),
                        tensorBases.outputBase + offset);
                }
            }
        }
    }
    EXPECT_THROW(view.address(15, 4095), std::out_of_range);
}

TEST(SauNMemoryView, RejectsOverlappingOrUnmappedExternalRanges)
{
    const auto config = simpleConfig();
    auto tensorBases = bases();
    DutKuiMemoryModel memory;

    tensorBases.weightBase = tensorBases.inputBase + 1;
    EXPECT_THROW(
        SauNMemoryView(config, memory, tensorBases), std::invalid_argument);

    tensorBases = bases();
    tensorBases.inputBase = 0x29200000;
    EXPECT_THROW(
        SauNMemoryView(config, memory, tensorBases), std::invalid_argument);
}

TEST(SauNMemoryView, ExternalModelUsesAndWritesSharedSram)
{
    const auto config = simpleConfig();
    const auto tensorBases = bases();
    DutKuiMemoryModel memory;
    writeSimpleTensors(memory, tensorBases, 1);

    {
        SauNMemoryView view(config, memory, tensorBases);
        sau_n::StreamingConvPipelineModel model(config, view);

        EXPECT_EQ(memory.readByte(tensorBases.outputBase), uint8_t{0xa5});
        runToDrain(model);
        EXPECT_EQ(model.outputs(), (std::vector<int8_t>{-19}));
        EXPECT_EQ(memory.readByte(tensorBases.outputBase), uint8_t{0xed});
    }

    memory.reset();
    writeSimpleTensors(memory, tensorBases, 2);
    {
        SauNMemoryView view(config, memory, tensorBases);
        sau_n::StreamingConvPipelineModel model(config, view);

        EXPECT_EQ(memory.readByte(tensorBases.outputBase), uint8_t{0xa5});
        runToDrain(model);
        EXPECT_EQ(model.outputs(), (std::vector<int8_t>{-37}));
        EXPECT_EQ(memory.readByte(tensorBases.outputBase), uint8_t{0xdb});
    }
}

} // anonymous namespace
} // namespace gem5::brs

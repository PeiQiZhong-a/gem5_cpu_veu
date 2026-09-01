#include <gtest/gtest.h>

#include <sstream>
#include <vector>

#include "sau_mikui/mikui_sau_cycle_model.hh"

namespace gem5::sau_mikui
{
namespace
{

enum class MemoryPattern
{
    Ones,
    ZeroBias,
    RowPattern,
};

class TestMemoryDriver
{
  public:
    explicit TestMemoryDriver(MemoryPattern pattern) : pattern(pattern) {}

    void
    clock(MikuiSauCycleModel &model, const brs::SauRequest &request = {})
    {
        if (owner != &model || model.cycle() == 0) {
            owner = &model;
            pending = {};
        }

        brs::SauMemoryResponse response;
        if (pending.valid) {
            response.valid = true;
            if (!pending.isWrite()) {
                fillReadData(response.readData);
            }
        }

        const auto visibleRequest = model.evaluateMemory().request;
        model.clockEdge(request, response);
        pending = visibleRequest;
    }

  private:
    void
    fillReadData(Beat128 &data) const
    {
        if (pattern == MemoryPattern::Ones) {
            data.fill(1);
        } else if (pattern == MemoryPattern::ZeroBias) {
            if (pending.address < 0x20024000 ||
                pending.address >= 0x20024020) {
                data.fill(1);
            }
        } else if (pending.address >= 0x20018000 &&
                   pending.address < 0x20019000) {
            const uint8_t row = static_cast<uint8_t>(
                ((pending.address - 0x20018000) >> 4) + 1);
            data.fill(row);
        }
    }

    MemoryPattern pattern;
    const MikuiSauCycleModel *owner = nullptr;
    brs::Sram128Request pending{};
};

void
clock(MikuiSauCycleModel &model, const brs::SauRequest &request = {})
{
    static TestMemoryDriver memory(MemoryPattern::Ones);
    memory.clock(model, request);
}

void
clockWithZeroBias(MikuiSauCycleModel &model,
                  const brs::SauRequest &request = {})
{
    static TestMemoryDriver memory(MemoryPattern::ZeroBias);
    memory.clock(model, request);
}

void
clockWithRowPattern(MikuiSauCycleModel &model,
                    const brs::SauRequest &request = {})
{
    static TestMemoryDriver memory(MemoryPattern::RowPattern);
    memory.clock(model, request);
}

brs::SauRequest
write(uint16_t address, uint64_t data, uint8_t writeType = 0)
{
    brs::SauRequest request;
    request.csrAddr = address;
    request.csrWrite = true;
    request.writeData = data;
    request.writeType = writeType;
    return request;
}

TEST(MikuiSauCycleModelTest, RunsCsrMemoryArrayAndWritebackClosedLoop)
{
    MikuiSauCycleModel model;
    model.reset();
    std::ostringstream trace;
    model.writeTraceHeader(trace);
    auto tracedClock = [&](const brs::SauRequest &request = {}) {
        clock(model, request);
        model.writeTrace(trace);
    };

    tracedClock(write(0x200, uint64_t{1} << 39));
    tracedClock();
    const uint64_t steps = uint64_t{1} | (uint64_t{1} << 8) |
                           (uint64_t{1} << 16) | (uint64_t{16} << 32) |
                           (uint64_t{16} << 40) | (uint64_t{1} << 48);
    tracedClock(write(0x202, steps));
    tracedClock();
    tracedClock(
        write(0x204, uint64_t{0x10000} | (uint64_t{0x18000} << 32)));
    tracedClock();
    const uint64_t final =
        uint64_t{1} | (uint64_t{0x20000} << 9) | (uint64_t{1} << 56);
    tracedClock(write(0x206, final, 1));
    bool sawStart = model.evaluateMemory().crossbarStart;
    tracedClock();

    bool sawDone = false;
    std::vector<Beat128> writes;
    for (unsigned cycle = 0; cycle < 2000 && !sawDone; ++cycle) {
        const auto memory = model.evaluateMemory();
        sawStart |= memory.crossbarStart;
        sawDone |= memory.crossbarDone;
        if (memory.request.valid && memory.request.isWrite()) {
            writes.push_back(memory.request.writeData);
        }
        tracedClock();
    }

    EXPECT_TRUE(sawStart);
    EXPECT_TRUE(sawDone);
    EXPECT_EQ(model.stats().acceptedCommands, 1u);
    EXPECT_EQ(model.stats().completedCommands, 1u);
    EXPECT_GT(model.stats().sramReadBeats, 0u);
    EXPECT_EQ(model.stats().sramWriteBeats, 16u);
    EXPECT_FALSE(model.active());
    ASSERT_EQ(writes.size(), 16u);
    for (const auto &beat : writes) {
        for (const auto value : beat) {
            EXPECT_EQ(value, 16u);
        }
    }
    EXPECT_FALSE(model.memoryErrors().any());
    const auto traceText = trace.str();
    for (const auto *event : {"start", "first_read", "first_array_input",
                              "first_result", "last_write", "done"}) {
        EXPECT_NE(traceText.find(event), std::string::npos) << event;
    }
}

TEST(MikuiSauCycleModelTest, ResetDropsAllVisibleState)
{
    MikuiSauCycleModel model(false);
    clock(model, write(0x200, 0x1234));
    ASSERT_TRUE(model.evaluate().valid);
    model.reset();
    EXPECT_FALSE(model.evaluate().valid);
    EXPECT_FALSE(model.evaluateMemory().request.valid);
    EXPECT_EQ(model.cycle(), 0u);
}

TEST(MikuiSauCycleModelTest, SupportsAllAbdTransposeModes)
{
    for (const auto mode :
         {TransposeMode::Atbd, TransposeMode::Abtd, TransposeMode::Abdt}) {
        MikuiSauCycleModel model;
        clock(model, write(0x200, (uint64_t{1} << 39) |
                                      (static_cast<uint64_t>(mode) << 32)));
        clock(model);
        const uint64_t steps = uint64_t{1} | (uint64_t{1} << 8) |
                               (uint64_t{1} << 16) | (uint64_t{16} << 32) |
                               (uint64_t{16} << 40) | (uint64_t{1} << 48);
        clock(model, write(0x202, steps));
        clock(model);
        clock(model,
              write(0x204, uint64_t{0x10000} | (uint64_t{0x18000} << 32)));
        clock(model);
        clock(model, write(0x206,
                           uint64_t{1} | (uint64_t{0x20000} << 9) |
                               (uint64_t{1} << 56),
                           1));
        clock(model);

        std::vector<Beat128> writes;
        bool done = false;
        for (unsigned cycle = 0; cycle < 2500 && !done; ++cycle) {
            const auto memory = model.evaluateMemory();
            done |= memory.crossbarDone;
            if (memory.request.valid && memory.request.isWrite()) {
                writes.push_back(memory.request.writeData);
            }
            clock(model);
        }
        ASSERT_TRUE(done) << static_cast<unsigned>(mode);
        ASSERT_EQ(writes.size(), 16u) << static_cast<unsigned>(mode);
        for (const auto &beat : writes) {
            for (const auto value : beat) {
                EXPECT_EQ(value, 16u) << static_cast<unsigned>(mode);
            }
        }
    }
}

TEST(MikuiSauCycleModelTest, RunsStridedStandardThreeByThreeConvolution)
{
    MikuiSauCycleModel model;
    const uint64_t command0 =
        (uint64_t{3} << 2) | (uint64_t{1} << 4) | (uint64_t{1} << 39);
    clockWithZeroBias(model, write(0x200, command0));
    clockWithZeroBias(model);
    const uint64_t steps = uint64_t{1} | (uint64_t{1} << 8) |
                           (uint64_t{1} << 16) | (uint64_t{16} << 32) |
                           (uint64_t{16} << 40) | (uint64_t{1} << 48);
    clockWithZeroBias(model, write(0x202, steps));
    clockWithZeroBias(model);
    clockWithZeroBias(
        model, write(0x204, uint64_t{0x10000} | (uint64_t{0x18000} << 32)));
    clockWithZeroBias(model);
    clockWithZeroBias(
        model, write(0x206,
                     uint64_t{1} | (uint64_t{0x20000} << 9) |
                         (uint64_t{0x24000} << 32) |
                         (static_cast<uint64_t>(CalculateMode::Conv) << 52) |
                         (uint64_t{1} << 56),
                     1));
    clockWithZeroBias(model);

    std::vector<Beat128> writes;
    bool done = false;
    for (unsigned cycle = 0; cycle < 3000 && !done; ++cycle) {
        const auto memory = model.evaluateMemory();
        done |= memory.crossbarDone;
        if (memory.request.valid && memory.request.isWrite()) {
            writes.push_back(memory.request.writeData);
        }
        clockWithZeroBias(model);
    }
    EXPECT_TRUE(done);
    ASSERT_EQ(writes.size(), 16u);
    for (unsigned row = 0; row < writes.size(); ++row) {
        for (unsigned col = 0; col < writes[row].size(); ++col) {
            EXPECT_EQ(writes[row][col], 9u) << "row=" << row << " col=" << col;
        }
    }
}

TEST(MikuiSauCycleModelTest, RunsMatrixAddAndStandaloneTransposer)
{
    for (const auto operation :
         {CalculateMode::Add, CalculateMode::Transposer}) {
        MikuiSauCycleModel model;
        clock(model, write(0x200, uint64_t{1} << 39));
        clock(model);
        const uint64_t steps = uint64_t{1} | (uint64_t{1} << 8) |
                               (uint64_t{1} << 16) | (uint64_t{16} << 32) |
                               (uint64_t{16} << 40) | (uint64_t{1} << 48);
        clock(model, write(0x202, steps));
        clock(model);
        clock(model,
              write(0x204, uint64_t{0x10000} | (uint64_t{0x18000} << 32)));
        clock(model);
        clock(model, write(0x206,
                           uint64_t{1} | (uint64_t{0x20000} << 9) |
                               (static_cast<uint64_t>(operation) << 52) |
                               (uint64_t{1} << 56),
                           1));
        clock(model);

        std::vector<Beat128> writes;
        bool done = false;
        for (unsigned cycle = 0; cycle < 2500 && !done; ++cycle) {
            const auto memory = model.evaluateMemory();
            done |= memory.crossbarDone;
            if (memory.request.valid && memory.request.isWrite()) {
                writes.push_back(memory.request.writeData);
            }
            clock(model);
        }
        ASSERT_TRUE(done) << static_cast<unsigned>(operation);
        ASSERT_EQ(writes.size(), 16u) << static_cast<unsigned>(operation);
        const uint8_t expected = operation == CalculateMode::Add ? 2 : 1;
        for (const auto &beat : writes) {
            for (const auto value : beat) {
                EXPECT_EQ(value, expected) << static_cast<unsigned>(operation);
            }
        }
    }
}

TEST(MikuiSauCycleModelTest, RetainDefersWritebackAndAccumulatesNextCommand)
{
    for (const auto retainFlow : {FlowMode::Retain, FlowMode::Tretain}) {
        MikuiSauCycleModel model;
        const uint64_t steps = uint64_t{1} | (uint64_t{1} << 8) |
                               (uint64_t{1} << 16) | (uint64_t{16} << 32) |
                               (uint64_t{16} << 40) | (uint64_t{1} << 48);
        const uint64_t bases = uint64_t{0x10000} | (uint64_t{0x18000} << 32);

        auto issue = [&](FlowMode flow, bool last) {
            clock(model, write(0x200, last ? (uint64_t{1} << 39) : 0));
            clock(model);
            clock(model, write(0x202, steps));
            clock(model);
            clock(model, write(0x204, bases));
            clock(model);
            const uint64_t final = uint64_t{1} | (uint64_t{0x20000} << 9) |
                                   (static_cast<uint64_t>(flow) << 54) |
                                   (uint64_t{1} << 56);
            clock(model, write(0x206, final, 1));
            clock(model);
        };

        issue(retainFlow, false);
        bool firstDone = false;
        unsigned firstWrites = 0;
        for (unsigned cycle = 0; cycle < 2500 && !firstDone; ++cycle) {
            const auto memory = model.evaluateMemory();
            firstDone |= memory.crossbarDone;
            firstWrites += memory.request.valid && memory.request.isWrite();
            clock(model);
        }
        ASSERT_TRUE(firstDone) << static_cast<unsigned>(retainFlow);
        EXPECT_EQ(firstWrites, 0u) << static_cast<unsigned>(retainFlow);

        issue(FlowMode::Cnormal, true);
        bool secondDone = false;
        std::vector<Beat128> writes;
        std::ostringstream secondTrace;
        for (unsigned cycle = 0; cycle < 2500 && !secondDone; ++cycle) {
            const auto memory = model.evaluateMemory();
            secondDone |= memory.crossbarDone;
            if (memory.request.valid && memory.request.isWrite()) {
                writes.push_back(memory.request.writeData);
            }
            if (cycle < 300) {
                model.writeTrace(secondTrace);
            }
            clock(model);
        }
        std::ostringstream finalTrace;
        model.writeTrace(finalTrace);
        ASSERT_TRUE(secondDone)
            << "accepted=" << model.stats().acceptedCommands
            << " completed=" << model.stats().completedCommands
            << " reads=" << model.stats().sramReadBeats
            << " writes=" << model.stats().sramWriteBeats
            << " active=" << model.active() << " trace=" << finalTrace.str()
            << "\nsecond-command trace:\n" << secondTrace.str();
        ASSERT_EQ(writes.size(), 16u);
        for (const auto &beat : writes) {
            for (const auto value : beat) {
                EXPECT_EQ(value, 32u);
            }
        }
    }
}

TEST(MikuiSauCycleModelTest, RunsPointwiseAndDepthwiseWithoutBiasSaturation)
{
    for (const bool depthwise : {false, true}) {
        MikuiSauCycleModel model;
        const uint8_t kernel = depthwise ? 3 : 1;
        const uint8_t registerMode = depthwise ? 2 : 0;
        const uint64_t command0 = registerMode |
                                  (static_cast<uint64_t>(kernel) << 2) |
                                  (uint64_t{1} << 39);
        clockWithZeroBias(model, write(0x200, command0));
        clockWithZeroBias(model);
        const uint64_t steps = uint64_t{1} | (uint64_t{1} << 8) |
                               (uint64_t{1} << 16) | (uint64_t{16} << 32) |
                               (uint64_t{16} << 40) | (uint64_t{1} << 48);
        clockWithZeroBias(model, write(0x202, steps));
        clockWithZeroBias(model);
        clockWithZeroBias(model, write(0x204, uint64_t{0x10000} |
                                                  (uint64_t{0x18000} << 32)));
        clockWithZeroBias(model);
        const uint8_t flows = depthwise ? 16 : 1;
        clockWithZeroBias(
            model,
            write(0x206,
                  uint64_t{1} | (uint64_t{0x20000} << 9) |
                      (uint64_t{0x24000} << 32) |
                      (static_cast<uint64_t>(CalculateMode::Conv) << 52) |
                      (static_cast<uint64_t>(flows) << 56),
                  1));
        clockWithZeroBias(model);

        std::vector<Beat128> writes;
        bool done = false;
        for (unsigned cycle = 0; cycle < 6000 && !done; ++cycle) {
            const auto memory = model.evaluateMemory();
            done |= memory.crossbarDone;
            if (memory.request.valid && memory.request.isWrite()) {
                writes.push_back(memory.request.writeData);
            }
            clockWithZeroBias(model);
        }
        ASSERT_TRUE(done) << "depthwise=" << depthwise;

        ASSERT_EQ(writes.size(), 16u) << "depthwise=" << depthwise;
        const uint8_t expected = depthwise ? 9 : 16;
        for (unsigned row = 0; row < writes.size(); ++row) {
            for (unsigned col = 0; col < writes[row].size(); ++col) {
                EXPECT_EQ(writes[row][col], expected)
                    << "depthwise=" << depthwise << " row=" << row
                    << " col=" << col;
            }
        }
    }
}

TEST(MikuiSauCycleModelTest, AppliesCutbitAndWritesSixteenBitShiftResults)
{
    for (const bool shift : {false, true}) {
        MikuiSauCycleModel model;
        const uint64_t command0 = (static_cast<uint64_t>(shift) << 5) |
                                  (uint64_t{1} << 34) | (uint64_t{1} << 39);
        clock(model, write(0x200, command0));
        clock(model);
        const uint64_t steps = uint64_t{1} | (uint64_t{1} << 8) |
                               (uint64_t{1} << 16) | (uint64_t{16} << 32) |
                               (uint64_t{16} << 40) | (uint64_t{1} << 48);
        clock(model, write(0x202, steps));
        clock(model);
        clock(model,
              write(0x204, uint64_t{0x10000} | (uint64_t{0x18000} << 32)));
        clock(model);
        clock(model, write(0x206,
                           uint64_t{1} | (uint64_t{0x20000} << 9) |
                               (uint64_t{1} << 56),
                           1));
        clock(model);

        std::vector<Beat128> writes;
        bool done = false;
        for (unsigned cycle = 0; cycle < 4000 && !done; ++cycle) {
            const auto memory = model.evaluateMemory();
            done |= memory.crossbarDone;
            if (memory.request.valid && memory.request.isWrite()) {
                writes.push_back(memory.request.writeData);
            }
            clock(model);
        }
        ASSERT_TRUE(done) << "shift=" << shift;
        ASSERT_EQ(writes.size(), shift ? 32u : 16u);
        if (!shift) {
            for (const auto &beat : writes) {
                for (const auto value : beat) {
                    EXPECT_EQ(value, 8u);
                }
            }
        } else {
            for (unsigned beat = 0; beat < writes.size(); ++beat) {
                for (unsigned byte = 0; byte < writes[beat].size(); ++byte) {
                    EXPECT_EQ(writes[beat][byte], 4u)
                        << "beat=" << beat << " byte=" << byte;
                }
            }
        }
    }
}

TEST(MikuiSauCycleModelTest, CtransTransposesFinalMatrixWriteback)
{
    MikuiSauCycleModel model;
    clockWithRowPattern(model, write(0x200, uint64_t{1} << 39));
    clockWithRowPattern(model);
    const uint64_t steps = uint64_t{1} | (uint64_t{1} << 8) |
                           (uint64_t{1} << 16) | (uint64_t{16} << 32) |
                           (uint64_t{16} << 40) | (uint64_t{1} << 48);
    clockWithRowPattern(model, write(0x202, steps));
    clockWithRowPattern(model);
    clockWithRowPattern(
        model, write(0x204, uint64_t{0x10000} | (uint64_t{0x18000} << 32)));
    clockWithRowPattern(model);
    clockWithRowPattern(
        model, write(0x206,
                     uint64_t{1} | (uint64_t{0x20000} << 9) |
                         (static_cast<uint64_t>(CalculateMode::Add) << 52) |
                         (static_cast<uint64_t>(FlowMode::Ctrans) << 54) |
                         (uint64_t{1} << 56),
                     1));
    clockWithRowPattern(model);

    std::vector<Beat128> writes;
    bool done = false;
    for (unsigned cycle = 0; cycle < 3000 && !done; ++cycle) {
        const auto memory = model.evaluateMemory();
        done |= memory.crossbarDone;
        if (memory.request.valid && memory.request.isWrite()) {
            writes.push_back(memory.request.writeData);
        }
        clockWithRowPattern(model);
    }
    ASSERT_TRUE(done);
    ASSERT_EQ(writes.size(), 16u);
    for (const auto &beat : writes) {
        for (unsigned col = 0; col < beat.size(); ++col) {
            EXPECT_EQ(beat[col], col + 1);
        }
    }
}

} // namespace
} // namespace gem5::sau_mikui

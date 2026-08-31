#ifndef __SAU_N_SAU_MODEL_HH__
#define __SAU_N_SAU_MODEL_HH__

#include <array>
#include <cstdint>
#include <map>
#include <string_view>
#include <vector>

#include "sau_n/sau_types.hh"

namespace gem5::sau_n
{

inline constexpr int32_t Accumulator24Min = -(1 << 23);
inline constexpr int32_t Accumulator24Max = (1 << 23) - 1;

int16_t multiplySignedInt8(int8_t activation, int8_t weight);
int32_t saturatingAddSigned24(int32_t accumulator, int32_t addend);
int8_t quantizeSignedInt8(int32_t accumulator, uint64_t cutbit);
uint16_t signExtendedInt8Slot(int8_t value);

enum class SauNumericPhase : uint8_t
{
    Empty = 0,
    Accumulating = 1,
    Finalized = 2,
};

struct SauPeNumericState
{
    int8_t activation = 0;
    int8_t weight = 0;
    int16_t product = 0;
    int32_t accumulator = 0;
    bool valid = false;
    bool biasApplied = false;

    bool operator==(const SauPeNumericState &other) const
    {
        return activation == other.activation && weight == other.weight &&
            product == other.product && accumulator == other.accumulator &&
            valid == other.valid && biasApplied == other.biasApplied;
    }
};

struct SauNumericOutput
{
    int32_t accumulator = 0;
    int8_t value = 0;
    uint16_t rtlSlot = 0;
    bool valid = false;
};

class SauNumericCore
{
  public:
    using Int8Lanes = std::array<int8_t, SauRows>;
    using BiasLanes = std::array<int16_t, SauColumns>;
    using PeStates = std::array<SauPeNumericState, SauRows * SauColumns>;
    using Outputs = std::array<SauNumericOutput, SauRows * SauColumns>;

    void begin(uint64_t validRows, uint64_t validColumns);
    void macStep(const Int8Lanes &activations, const Int8Lanes &weights);
    void addBias(const BiasLanes &biases);
    Outputs outputSnapshot(uint64_t cutbit) const;
    void clear();
    void reset();

    SauNumericPhase phase() const { return numericPhase; }
    uint64_t validRows() const { return rows; }
    uint64_t validColumns() const { return columns; }
    uint64_t macSteps() const { return completedMacSteps; }
    const SauPeNumericState &pe(uint64_t row, uint64_t column) const;
    const PeStates &peStates() const { return states; }

  private:
    PeStates states{};
    SauNumericPhase numericPhase = SauNumericPhase::Empty;
    uint64_t rows = 0;
    uint64_t columns = 0;
    uint64_t completedMacSteps = 0;
};

/** Cycle anchors measured against the frozen fused RTL VCS traces. */
inline constexpr uint64_t ArrayMacCommitDelay = 2;
inline constexpr uint64_t ArrayDrainToBiasDelay = 33;

enum class SauEngineState : uint8_t
{
    Idle = 0,
    Start = 1,
    Work = 2,
    Storage = 3,
    Done = 4,
};

std::string_view sauEngineStateName(SauEngineState state);

using SauPeMask = std::array<uint64_t, 4>;

bool peMaskBit(const SauPeMask &mask, uint64_t row, uint64_t column);

struct SauCycleConfig
{
    uint64_t calcCycles = 0;
    uint64_t validRows = 0;
    uint64_t validColumns = 0;
    uint64_t cutbit = 0;
    SauNumericCore::BiasLanes biases{};
};

struct SauCycleInputs
{
    bool insValid = false;
    SauCycleConfig config{};
    bool inputValid = false;
    SauNumericCore::Int8Lanes activations{};
    SauNumericCore::Int8Lanes weights{};
    bool outputRequest = false;
    bool outputGrant = true;
};

struct SauCyclePeState
{
    int8_t activation = 0;
    int8_t weight = 0;
    int32_t accumulator = 0;
};

struct SauCycleObservation
{
    uint64_t cycle = 0;
    SauEngineState state = SauEngineState::Idle;
    uint64_t dataInCount = 0;
    uint64_t outputCounter = 0;
    bool storageReady = false;
    uint16_t rowReadyMask = 0;
    bool peFinish = false;
    uint16_t osValidMask = 0;
    bool internalOutputValid = false;
    bool engineOutputFire = false;
    bool rowScoreValid = false;
    uint64_t rowSequence = 0;
    std::array<uint16_t, SauColumns> outputSlots{};
    bool calFinish = false;
    SauPeMask peValidMask{};
    SauPeMask macCommitMask{};
    SauPeMask addCommitMask{};
    std::array<SauCyclePeState, SauRows * SauColumns> peStates{};
};

/**
 * Frozen fused-array cycle model. tick() observes the current stable cycle,
 * applies all events due at its closing edge, and returns the newly committed
 * register view. Strict input timing remains the default frozen RTL behavior;
 * the elastic mode only permits gaps between otherwise identical input fires.
 * Both modes keep the fixed CONV/CNORMAL/INT8 numeric protocol.
 */
class SauCycleModel
{
  public:
    explicit SauCycleModel(
        SauInputProtocol protocol = SauInputProtocol::StrictRtlContinuous);
    SauCycleObservation tick(const SauCycleInputs &inputs = {});
    void reset();

    uint64_t cycle() const { return currentCycle; }
    SauEngineState state() const { return engineState; }
    SauInputProtocol protocol() const { return inputProtocol; }
    bool cycleAnchorsProvisional() const { return false; }

  private:
    struct ScheduledMac
    {
        uint64_t row = 0;
        uint64_t column = 0;
        int8_t activation = 0;
        int8_t weight = 0;
    };

    struct ScheduledBias
    {
        uint64_t row = 0;
        uint64_t column = 0;
        int16_t bias = 0;
    };

    void validateConfig(const SauCycleConfig &candidate) const;
    void scheduleInput(const SauCycleInputs &inputs);
    void scheduleCompletion();
    void setMaskBit(SauPeMask &mask, uint64_t row, uint64_t column) const;

    uint64_t currentCycle = 0;
    const SauInputProtocol inputProtocol;
    SauEngineState engineState = SauEngineState::Idle;
    SauCycleConfig activeConfig{};
    bool configLoaded = false;
    uint64_t acceptedInputs = 0;
    bool storageReady = false;
    bool clearRowReadyNext = false;
    uint64_t outputRow = 0;
    std::array<bool, SauRows> rowReady{};
    std::array<std::array<uint16_t, SauColumns>, SauRows> rowOutputs{};
    std::array<SauCyclePeState, SauRows * SauColumns> cyclePeStates{};
    std::map<uint64_t, std::vector<ScheduledMac>> scheduledMacs;
    std::map<uint64_t, std::vector<ScheduledBias>> scheduledBiases;
    std::map<uint64_t, std::vector<uint64_t>> scheduledRowsReady;
    uint64_t storageReadyCycle = UINT64_MAX;
    uint64_t calFinishCycle = UINT64_MAX;
};

} // namespace gem5::sau_n

#endif // __SAU_N_SAU_MODEL_HH__

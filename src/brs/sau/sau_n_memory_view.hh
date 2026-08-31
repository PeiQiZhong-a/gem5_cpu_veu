#ifndef __BRS_SAU_SAU_N_MEMORY_VIEW_HH__
#define __BRS_SAU_SAU_N_MEMORY_VIEW_HH__

#include <array>
#include <cstdint>

#include "brs/memory/dut_kui_memory_model.hh"
#include "sau_n/streaming_pipeline_contract.hh"

namespace gem5::brs
{

struct SauNTensorBaseAddresses
{
    uint32_t inputBase = 0;
    uint32_t weightBase = 0;
    uint32_t biasBase = 0;
    uint32_t outputBase = 0;
};

// Maps sau_n's internal bank/row view onto the byte-addressed SRAM visible to
// the CPU.  The view does not own either the memory model or the scratchpad
// model; the caller must keep both alive for the duration of all accesses.
class SauNMemoryView final : public sau_n::ScratchpadBacking
{
  public:
    SauNMemoryView(
        const sau_n::PipelineResolvedConfig &config,
        DutKuiMemoryModel &memory,
        SauNTensorBaseAddresses bases);

    uint8_t read(uint64_t bank, uint64_t row) const override;
    void write(uint64_t bank, uint64_t row, uint8_t value) override;

    uint32_t address(uint64_t bank, uint64_t row) const;
    const sau_n::PipelineDerivedConfig &derived() const
    {
        return dimensions;
    }
    const SauNTensorBaseAddresses &bases() const { return tensorBases; }

  private:
    using AddressBank =
        std::array<uint32_t, sau_n::ScratchpadRows>;
    using ValidBank =
        std::array<bool, sau_n::ScratchpadRows>;

    void bind(
        uint64_t bank, uint64_t row, uint64_t address,
        const char *tensor);
    uint32_t requireAddress(uint64_t bank, uint64_t row) const;

    DutKuiMemoryModel &memory;
    SauNTensorBaseAddresses tensorBases;
    sau_n::PipelineDerivedConfig dimensions;
    std::array<AddressBank, sau_n::ScratchpadBanks> addresses{};
    std::array<ValidBank, sau_n::ScratchpadBanks> mapped{};
};

} // namespace gem5::brs

#endif // __BRS_SAU_SAU_N_MEMORY_VIEW_HH__

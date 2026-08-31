#include "sau_n/banked_scratchpad.hh"

#include <stdexcept>

#include "sau_n/im2col_address.hh"

namespace gem5::sau_n
{
namespace
{

void
validateIndex(uint64_t bank, uint64_t row)
{
    if (bank >= SpBanks) {
        throw std::out_of_range("scratchpad bank must be in [0, 15]");
    }
    if (row >= SpBankEntries) {
        throw std::out_of_range("scratchpad row must be in [0, 4095]");
    }
}

} // anonymous namespace

void
BankedScratchpad::clear()
{
    if (backing != nullptr) {
        throw std::logic_error(
            "external scratchpad backing cannot be cleared");
    }
    for (auto &bank : storage) {
        bank.fill(0);
    }
}

void
BankedScratchpad::write(uint64_t bank, uint64_t row, uint8_t value)
{
    validateIndex(bank, row);
    if (backing != nullptr) {
        backing->write(bank, row, value);
        return;
    }
    storage[bank][row] = value;
}

uint8_t
BankedScratchpad::read(uint64_t bank, uint64_t row) const
{
    validateIndex(bank, row);
    if (backing != nullptr) {
        return backing->read(bank, row);
    }
    return storage[bank][row];
}

void
BankedScratchpad::preload(const ResolvedConfig &config)
{
    if (backing != nullptr) {
        throw std::logic_error(
            "external scratchpad backing cannot use generator preload");
    }
    const ChwAddressMapper mapper(config);
    clear();
    for (uint64_t n = 0; n < config.n; ++n) {
        for (uint64_t c = 0; c < config.c; ++c) {
            for (uint64_t h = 0; h < config.h; ++h) {
                for (uint64_t w = 0; w < config.w; ++w) {
                    const auto location = mapper.locate(n, c, h, w);
                    write(
                        location.bank, location.row,
                        tbActValueV1(n, c, h, w));
                }
            }
        }
    }
}

SramResponse
BankedScratchpad::combinationalResponse(const SramRequest &request) const
{
    SramResponse response;
    for (std::size_t bank = 0; bank < ScratchpadBanks; ++bank) {
        if (!request.valid[bank]) {
            continue;
        }
        response.valid[bank] = true;
        response.data[bank] = read(bank, request.address[bank]);
    }
    return response;
}

} // namespace gem5::sau_n

#include "brs/pipeline/frontend_fetch_unit.hh"

#include <algorithm>

#include "base/logging.hh"

namespace gem5
{

namespace
{

constexpr uint32_t
alignDown(uint32_t value, uint32_t align)
{
    return value & ~(align - 1);
}

bool
isRvc(uint32_t instr)
{
    return (instr & 0x3) != 0x3;
}

} // anonymous namespace

void
FrontendAligner::reset(uint32_t pcReset)
{
    stopFetch = false;
    fetchInstrBitsH = 0;
    instrNeedConcat = false;
    alignedInstrBits = 0;
    nextInstrAddr = pcReset;
    alignedInstrValid = false;
    lowHalfWordInvalid = false;
}

FrontendAligner::Output
FrontendAligner::step(const Input &in)
{
    const bool highHalfWordNotRvc = ((in.fetchInstrBits >> 16) & 0x3) == 0x3;
    const bool lowHalfWordNotRvc = (in.fetchInstrBits & 0x3) == 0x3;

    Output out;
    out.alignedInstrBits = alignedInstrBits;
    out.nextInstrAddr = nextInstrAddr;
    out.stopFetch = stopFetch;
    out.alignedInstrValid = alignedInstrValid;
    out.lowHalfWordInvalid = lowHalfWordInvalid;
    out.instrNeedConcat = instrNeedConcat;

    bool nextStopFetch = stopFetch;
    uint16_t nextFetchInstrBitsH = fetchInstrBitsH;
    bool nextInstrNeedConcat = instrNeedConcat;
    uint32_t nextAlignedInstrBits = alignedInstrBits;
    uint32_t nextNextInstrAddr = nextInstrAddr;
    bool nextAlignedInstrValid = alignedInstrValid;
    bool nextLowHalfWordInvalid = lowHalfWordInvalid;

    if (stopFetch) {
        out.stopFetch = false;
        out.alignedInstrBits = fetchInstrBitsH;
        out.nextInstrAddr = in.fetchInstrAddr + 2;
        out.alignedInstrValid = true;

        nextStopFetch = out.stopFetch;
        nextAlignedInstrBits = out.alignedInstrBits;
        nextNextInstrAddr = out.nextInstrAddr;
        nextAlignedInstrValid = out.alignedInstrValid;
    } else if (in.fetchInstrValid) {
        if (instrNeedConcat) {
            nextLowHalfWordInvalid = false;
            out.alignedInstrBits =
                ((in.fetchInstrBits & 0xffff) << 16) | fetchInstrBitsH;
            out.nextInstrAddr = in.fetchInstrAddr + 4;
            out.alignedInstrValid = true;

            if (highHalfWordNotRvc) {
                nextInstrNeedConcat = !in.flush;
                out.stopFetch = false;
            } else {
                nextInstrNeedConcat = false;
                out.stopFetch = !in.flush;
            }
        } else if ((in.fetchInstrAddr & 0x3) != 0) {
            nextLowHalfWordInvalid = true;
            if (highHalfWordNotRvc) {
                out.alignedInstrBits = in.fetchInstrBits;
                out.nextInstrAddr = in.fetchInstrAddr;
                nextInstrNeedConcat = true;
                out.stopFetch = false;
                out.alignedInstrValid = false;
            } else {
                out.alignedInstrBits = in.fetchInstrBits >> 16;
                out.nextInstrAddr = in.fetchInstrAddr + 2;
                out.stopFetch = false;
                out.alignedInstrValid = true;
            }
        } else if (lowHalfWordNotRvc) {
            nextLowHalfWordInvalid = false;
            out.alignedInstrBits = in.fetchInstrBits;
            out.nextInstrAddr = in.fetchInstrAddr + 4;
            nextInstrNeedConcat = false;
            out.stopFetch = false;
            out.alignedInstrValid = true;
        } else {
            nextLowHalfWordInvalid = false;
            out.alignedInstrBits = in.fetchInstrBits & 0xffff;
            out.nextInstrAddr = in.fetchInstrAddr + 2;
            out.alignedInstrValid = true;

            if (highHalfWordNotRvc) {
                nextInstrNeedConcat = !in.flush;
                out.stopFetch = false;
            } else {
                nextInstrNeedConcat = false;
                out.stopFetch = !in.flush;
            }
        }

        nextStopFetch = out.stopFetch;
        nextAlignedInstrBits = out.alignedInstrBits;
        nextNextInstrAddr = out.nextInstrAddr;
        nextAlignedInstrValid = out.alignedInstrValid;
    } else if (in.flush) {
        nextStopFetch = false;
        nextAlignedInstrValid = false;
        nextInstrNeedConcat = false;
        nextLowHalfWordInvalid = false;
        out.alignedInstrBits = 0;
        out.nextInstrAddr = 0;
        out.stopFetch = false;
        out.alignedInstrValid = false;
    }

    if (in.fetchInstrValid) {
        nextFetchInstrBitsH = static_cast<uint16_t>(in.fetchInstrBits >> 16);
    }

    stopFetch = nextStopFetch;
    fetchInstrBitsH = nextFetchInstrBitsH;
    instrNeedConcat = nextInstrNeedConcat;
    alignedInstrBits = nextAlignedInstrBits;
    nextInstrAddr = nextNextInstrAddr;
    alignedInstrValid = nextAlignedInstrValid;
    lowHalfWordInvalid = nextLowHalfWordInvalid;

    return out;
}

FetchFifo::FetchFifo(unsigned depth)
    : depth(depth), entries(depth)
{
}

void
FetchFifo::reset()
{
    readPointer = 0;
    writePointer = 0;
    cntStatus = 0;
}

void
FetchFifo::flush()
{
    reset();
}

bool
FetchFifo::pushBlock(const FetchBlock &block)
{
    const unsigned start = (block.fetchAddr & 0xf) / sizeof(uint32_t);
    const unsigned wordsToPush = 4 - start;
    CycleInput in;
    in.pushCount = wordsToPush;
    for (unsigned i = 0; i < wordsToPush; ++i) {
        const unsigned wordIndex = start + i;
        in.pushEntries[i] = {
            static_cast<uint32_t>(block.blockAddr +
                                  wordIndex * sizeof(uint32_t)),
            block.words[wordIndex]
        };
    }

    if (capacityLeft() < wordsToPush) {
        return false;
    }

    step(in);
    return true;
}

FetchFifo::CycleOutput
FetchFifo::step(const CycleInput &in)
{
    panic_if(in.pushCount > 4, "Fetch FIFO push count exceeds four words");

    CycleOutput out;
    out.empty = empty();
    out.full = full();
    out.count = cntStatus;
    if (!out.empty) {
        out.rdata = entries[readPointer];
    }

    if (in.flushAll) {
        reset();
        return out;
    }

    if (in.flushExceptFirst) {
        if (cntStatus > 0) {
            writePointer = (readPointer + 1) % depth;
            cntStatus = 1;
        } else {
            reset();
        }
        return out;
    }

    const bool fifoUpdate = in.pushCount != 0 && !out.full;
    panic_if(in.pushCount != 0 && out.full,
             "Fetch FIFO push while full");
    panic_if(fifoUpdate && capacityLeft() < in.pushCount,
             "Fetch FIFO does not have room for pushed words");

    unsigned nextReadPointer = readPointer;
    if (in.pop && !out.empty) {
        nextReadPointer = (readPointer + 1) % depth;
    }

    unsigned nextWritePointer = writePointer;
    if (fifoUpdate) {
        for (unsigned i = 0; i < in.pushCount; ++i) {
            entries[(writePointer + i) % depth] = in.pushEntries[i];
        }
        nextWritePointer = (writePointer + in.pushCount) % depth;
    }

    unsigned nextCntStatus = cntStatus;
    if (fifoUpdate && in.pop && !out.empty) {
        nextCntStatus = cntStatus + in.pushCount - 1;
    } else if (in.pop && !out.empty) {
        nextCntStatus = cntStatus - 1;
    } else if (fifoUpdate) {
        nextCntStatus = cntStatus + in.pushCount;
    }

    readPointer = nextReadPointer;
    writePointer = nextWritePointer;
    cntStatus = nextCntStatus;

    return out;
}

void
FetchFifo::pop()
{
    panic_if(empty(), "Fetch FIFO pop while empty");
    CycleInput in;
    in.pop = true;
    step(in);
}

void
FetchBusUnit::reset(uint32_t pc)
{
    inFlight = false;
    discardInFlight = false;
    inFlightFetchAddr = 0;
    inFlightBlockAddr = 0;
    nextFetchAddr = pc;
}

void
FetchBusUnit::flush(uint32_t pc)
{
    if (inFlight) {
        discardInFlight = true;
    }
    nextFetchAddr = pc;
}

uint32_t
FetchBusUnit::requestBlockAddr() const
{
    // The Spirit testbench returns four words beginning at
    // ibus_out_addr[31:2], rather than at a 16-byte burst boundary.
    return alignDown(nextFetchAddr, sizeof(uint32_t));
}

void
FetchBusUnit::markRequestIssued()
{
    panic_if(inFlight, "Fetch request issued while another is in flight");
    inFlight = true;
    discardInFlight = false;
    inFlightFetchAddr = nextFetchAddr;
    inFlightBlockAddr = requestBlockAddr();
}

bool
FetchBusUnit::acceptResponse(const FetchBlock &block)
{
    if (!inFlight || block.blockAddr != inFlightBlockAddr) {
        return false;
    }

    const bool use = !discardInFlight;
    if (use) {
        nextFetchAddr = alignDown(inFlightFetchAddr, 16) + 16;
    }

    inFlight = false;
    discardInFlight = false;
    inFlightFetchAddr = 0;
    inFlightBlockAddr = 0;
    return use;
}

FrontendFetchUnit::FrontendFetchUnit()
    : FrontendFetchUnit(Config{})
{
}

FrontendFetchUnit::FrontendFetchUnit(const Config &config)
    : config(config), fifo(config.fifoDepth)
{
    panic_if(config.burstBytes != 16,
             "FrontendFetchUnit currently models the RV-NEW 16-byte IBU burst");
}

void
FrontendFetchUnit::configure(const Config &newConfig)
{
    panic_if(newConfig.burstBytes != 16,
             "FrontendFetchUnit currently models the RV-NEW 16-byte IBU burst");
    config = newConfig;
    fifo = FetchFifo(config.fifoDepth);
}

void
FrontendFetchUnit::clearIfOutput()
{
    ifReadyQ = false;
    ifPcQ = 0;
    ifInstrQ = 0;
    ifInstrLenQ = 4;
}

void
FrontendFetchUnit::reset(uint32_t startPc)
{
    pc = startPc;
    resetEnd = 0;
    fifo.reset();
    ibu.reset(startPc);
    aligner.reset(startPc);
    pfuInstrNeedUpdate = false;
    clearIfOutput();
    ibusReqCount = 0;
    fifoFlushCount = 0;
    alignedInstrCount = 0;
}

void
FrontendFetchUnit::flush(uint32_t targetPc)
{
    pc = targetPc;
    fifo.flush();
    ibu.flush(targetPc);
    aligner.reset(targetPc);
    pfuInstrNeedUpdate = false;
    clearIfOutput();
    ++fifoFlushCount;
}

FrontendFetchUnit::Output
FrontendFetchUnit::step(const Input &in)
{
    Output out;
    const bool resetEndReady = resetEnd == 2;
    // The RTL IBU decides w_ibus_out_req from the registered state at the
    // beginning of the edge.  A response may retire that state on this edge,
    // but it cannot make a replacement request visible until the next edge.
    // In particular, an old response coincident with a redirect must be
    // discarded without also issuing the redirected request one cycle early.
    const bool ibuCouldRequestAtEdgeStart = ibu.canRequest();

    if (in.redirect) {
        flush(in.redirectTarget);
    }

    out.instValid = ifReadyQ;
    out.pc = ifPcQ;
    out.instr = ifInstrQ;
    out.instrLen = ifInstrLenQ;

    const bool responseAccepted =
        in.responseValid && ibu.acceptResponse(in.response);

    FetchFifo::CycleInput fifoIn;
    std::array<FetchFifo::Entry, 4> responseEntries = {};
    unsigned fifoPushWord = 0;

    if (responseAccepted) {
        const unsigned start =
            (in.response.fetchAddr & 0xf) / sizeof(uint32_t);
        fifoPushWord = fifo.empty() && !pfuInstrNeedUpdate ? 4 - start : 4;
        for (unsigned i = 0; i < fifoPushWord; ++i) {
            const unsigned wordIndex = 4 - fifoPushWord + i;
            responseEntries[i] = {
                static_cast<uint32_t>(in.response.blockAddr +
                                      wordIndex * sizeof(uint32_t)),
                in.response.words[wordIndex]
            };
        }
    }

    bool nextIfReadyQ = false;
    uint32_t nextIfPcQ = pc;
    uint32_t nextIfInstrQ = 0;
    uint8_t nextIfInstrLenQ = 4;

    if (!in.stall && pc < in.textEnd) {
        const uint32_t oldPc = pc;
        const bool fifoWasEmpty = fifo.empty();
        const bool hasBypassWord = responseAccepted && fifoWasEmpty &&
            fifoPushWord != 0;
        const bool hasWord = !fifoWasEmpty || hasBypassWord;
        FrontendAligner::Input alignIn;
        alignIn.fetchInstrValid = hasWord;
        alignIn.fetchInstrAddr = pc;
        alignIn.flush = false;
        if (hasWord) {
            alignIn.fetchInstrBits =
                fifoWasEmpty ? responseEntries[0].word : fifo.front().word;
        }

        const auto aligned = aligner.step(alignIn);
        pfuInstrNeedUpdate = aligner.needsConcat();
        if (hasWord && !aligned.stopFetch) {
            fifoIn.pop = true;
        }

        // FrontendAligner keeps the last aligned value in its registered
        // state while a 32-bit instruction is waiting for the next word.
        // That value is not a new instruction when the FIFO is empty: only
        // the stop-fetch half-word path is allowed to produce an instruction
        // without a fetch word on this cycle.
        const bool alignedInstrProduced = aligned.alignedInstrValid &&
            (hasWord || aligned.stopFetch);
        if (alignedInstrProduced) {
            nextIfReadyQ = true;
            nextIfPcQ = oldPc;
            nextIfInstrQ = aligned.alignedInstrBits;
            nextIfInstrLenQ = isRvc(aligned.alignedInstrBits) ? 2 : 4;
            pc = aligned.nextInstrAddr;
            ++alignedInstrCount;
        }
    }

    if (responseAccepted) {
        const bool consumeBypassWord = fifo.empty() && fifoIn.pop &&
            fifoPushWord != 0;
        fifoIn.pushCount = consumeBypassWord ? fifoPushWord - 1 :
            fifoPushWord;
        const unsigned firstEntry = consumeBypassWord ? 1 : 0;
        for (unsigned i = 0; i < fifoIn.pushCount; ++i) {
            fifoIn.pushEntries[i] = responseEntries[firstEntry + i];
        }
    }
    fifo.step(fifoIn);

    if (!in.stall) {
        ifReadyQ = nextIfReadyQ;
        ifPcQ = nextIfPcQ;
        ifInstrQ = nextIfInstrQ;
        ifInstrLenQ = nextIfInstrLenQ;
    }

    // PFU only permits the initial IBus request after r_reset_end advances
    // 0 -> 1 -> 2. A redirect is the IBU fetch-address-update path and is
    // allowed to request independently of the normal PFU allow-input gate.
    if ((resetEndReady || in.redirect) && fifo.count() < 4 &&
        ibuCouldRequestAtEdgeStart && ibu.canRequest() && pc < in.textEnd) {
        out.requestValid = true;
        // Spirit exposes the true fetch address on ibus_out_addr.  The
        // response block base is tracked separately at 32-bit word alignment.
        out.requestAddr = ibu.requestFetchAddr();
        out.requestFetchAddr = ibu.requestFetchAddr();
    }

    if (resetEnd < 2) {
        ++resetEnd;
    }

    return out;
}

void
FrontendFetchUnit::markRequestIssued()
{
    ibu.markRequestIssued();
    ++ibusReqCount;
}

} // namespace gem5

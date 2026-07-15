#include "brs/veu/timing_veu.hh"

#include <algorithm>
#include <limits>
#include <utility>

namespace gem5
{
namespace brs
{

TimingVeu::TimingVeu()
{
    reset();
}

void
TimingVeu::configure(const VeuTimingConfig &config)
{
    timing = config;
    if (timing.inputFifoDepth == 0) {
        timing.inputFifoDepth = 1;
    }
}

void
TimingVeu::setMemoryRequestCallback(MemoryRequestFn callback)
{
    memoryRequest = std::move(callback);
}

void
TimingVeu::reset()
{
    currentState = State::Idle;
    controlState = ControlState::Idle;
    csr = {};
    csr.mask = FullWriteMask;
    requestReg = {};
    responseData = 0;
    remainingCycles = 0;
    chunksRemaining = 0;
    chunkIndex = 0;
    currentReadAddr1 = 0;
    currentReadAddr2 = 0;
    currentReadAddr3 = 0;
    currentWriteAddr = 0;
    currentWriteMask = FullWriteMask;
    pendingMemory = PendingMemory::None;
    memoryResponseReady = false;
    pendingThreeSourceStart = false;
    pendingThreeSourceVeStart = 0;
    input1 = {};
    input2 = {};
    input3 = {};
    result = {};
    storeMerge = {};
    acceptedRequests = 0;
    responses = 0;
    startedOperations = 0;
    completedOperations = 0;
    processedChunks = 0;
    busyCycles = 0;
    loadWaitCycles = 0;
    executeCycles = 0;
    storeWaitCycles = 0;
    memoryReads = 0;
    memoryWrites = 0;
}

VeuResponse
TimingVeu::evaluate() const
{
    VeuResponse response;
    response.valid = controlState == ControlState::Respond;
    response.readData = response.valid ? responseData : 0;
    return response;
}

uint32_t
TimingVeu::readCsr(uint16_t addr) const
{
    switch (static_cast<VeuCsr>(addr)) {
      case VeuCsr::Status:
        return csr.status;
      case VeuCsr::ReadAddress1:
        return csr.raddr1;
      case VeuCsr::ReadAddress2:
        return csr.raddr2;
      case VeuCsr::WriteAddress:
        return csr.waddr;
      case VeuCsr::Config:
        return csr.config;
      case VeuCsr::VectorLength:
        return csr.vlen;
      case VeuCsr::Mask:
        return csr.mask;
      case VeuCsr::ReadAddress3:
        return csr.raddr3;
      default:
        return 0;
    }
}

void
TimingVeu::writeCsr(uint16_t addr, uint32_t value, VeuWriteType writeType)
{
    uint32_t *target = nullptr;
    switch (static_cast<VeuCsr>(addr)) {
      case VeuCsr::Status:
        target = &csr.status;
        break;
      case VeuCsr::ReadAddress1:
        target = &csr.raddr1;
        break;
      case VeuCsr::ReadAddress2:
        target = &csr.raddr2;
        break;
      case VeuCsr::WriteAddress:
        target = &csr.waddr;
        break;
      case VeuCsr::Config:
        target = &csr.config;
        break;
      case VeuCsr::VectorLength:
        target = &csr.vlen;
        break;
      case VeuCsr::Mask:
        target = &csr.mask;
        break;
      case VeuCsr::ReadAddress3:
        target = &csr.raddr3;
        break;
      default:
        break;
    }

    if (!target) {
        return;
    }

    switch (writeType) {
      case VeuWriteType::Write:
      case VeuWriteType::VectorStart:
        *target = value;
        break;
      case VeuWriteType::Set:
        *target |= value;
        break;
      case VeuWriteType::Clear:
        *target &= value;
        break;
    }
}

VeuInstruction
TimingVeu::currentInstruction() const
{
    const uint32_t start = csr.status >> 1;
    for (uint8_t bit = 0; bit < 32; ++bit) {
        if ((start & (uint32_t{1} << bit)) == 0) {
            continue;
        }
        switch (bit) {
          case 0: return VeuInstruction::Add;
          case 1: return VeuInstruction::Sub;
          case 2: return VeuInstruction::Min;
          case 3: return VeuInstruction::Max;
          case 4: return VeuInstruction::ReduceMin;
          case 5: return VeuInstruction::ReduceMax;
          case 6: return VeuInstruction::And;
          case 7: return VeuInstruction::Or;
          case 8: return VeuInstruction::Xor;
          case 11: return VeuInstruction::Move;
          case 12: return VeuInstruction::ShiftRightLogical;
          case 13: return VeuInstruction::ShiftRightArithmetic;
          case 16: return VeuInstruction::ReduceSum;
          case 18: return VeuInstruction::MultiplySubtract;
          case 19: return VeuInstruction::MultiplyAdd;
          case 20: return VeuInstruction::Multiply;
          case 21: return VeuInstruction::MultiplyHighSignedUnsigned;
          case 22: return VeuInstruction::MultiplyHigh;
          default: return VeuInstruction::Unknown;
        }
    }
    return VeuInstruction::Unknown;
}

bool
TimingVeu::needsSecondOperand() const
{
    switch (currentInstruction()) {
      case VeuInstruction::Move:
        return false;
      default:
        return true;
    }
}

bool
TimingVeu::needsThirdOperand() const
{
    return currentInstruction() == VeuInstruction::MultiplyAdd ||
           currentInstruction() == VeuInstruction::MultiplySubtract;
}

bool
TimingVeu::storesResult() const
{
    return currentInstruction() != VeuInstruction::Unknown;
}

bool
TimingVeu::writeMaskIsFull() const
{
    return currentWriteMask == FullWriteMask;
}

void
TimingVeu::acceptRequest(const VeuRequest &request)
{
    requestReg = request;
    ++acceptedRequests;

    const auto writeType = isVeuWriteType(request.writeType) ?
        static_cast<VeuWriteType>(request.writeType) : VeuWriteType::Write;

    responseData = request.csrRead ? readCsr(request.csrAddr) : 0;

    if (request.csrWrite && writeType == VeuWriteType::VectorStart) {
        startVectorOperation(request);
    } else if (request.csrWrite) {
        writeCsr(request.csrAddr, unpackVeuOperand1(request.writeData),
                 writeType);
    }

    controlState = ControlState::Respond;
}

void
TimingVeu::startVectorOperation(const VeuRequest &request)
{
    if (operationBusy()) {
        responseData = csr.status;
        return;
    }

    const bool threeSourceStart =
        (request.veStart & veuStartMask(VeuInstruction::MultiplyAdd)) ||
        (request.veStart & veuStartMask(VeuInstruction::MultiplySubtract));

    if (threeSourceStart && !pendingThreeSourceStart) {
        csr.raddr1 = unpackVeuOperand1(request.writeData);
        csr.raddr2 = unpackVeuOperand2(request.writeData);
        pendingThreeSourceStart = true;
        pendingThreeSourceVeStart = request.veStart;
        responseData = csr.status;
        return;
    }

    if (request.csrAddr == static_cast<uint16_t>(VeuCsr::ReadAddress1)) {
        if (pendingThreeSourceStart) {
            csr.raddr3 = unpackVeuOperand1(request.writeData);
        } else {
            csr.raddr1 = unpackVeuOperand1(request.writeData);
            csr.raddr2 = unpackVeuOperand2(request.writeData);
        }
    } else if (request.csrAddr == static_cast<uint16_t>(VeuCsr::ReadAddress3)) {
        csr.raddr3 = unpackVeuOperand1(request.writeData);
    }

    const uint32_t veStart = pendingThreeSourceStart ?
        pendingThreeSourceVeStart : request.veStart;
    pendingThreeSourceStart = false;
    pendingThreeSourceVeStart = 0;
    csr.status = (veStart << 1) | 1u;
    const uint32_t totalBits = effectiveVeuLengthAtStart(csr.vlen);
    chunksRemaining = std::max<uint32_t>(1, totalBits / VeuVectorBits);
    chunkIndex = 0;
    currentReadAddr1 = csr.raddr1;
    currentReadAddr2 = csr.raddr2;
    currentReadAddr3 = csr.raddr3;
    currentWriteAddr = csr.waddr;
    currentWriteMask = csr.mask;
    responseData = csr.status;
    ++startedOperations;

    if (timing.startupCycles > 0) {
        remainingCycles = timing.startupCycles;
        currentState = State::Startup;
    } else {
        currentState = State::IssueLoad1;
    }
}

bool
TimingVeu::issueMemory(PendingMemory pending, uint32_t addr, bool isWrite,
                       const std::array<uint8_t, VeuVectorBytes> &data)
{
    if (!memoryRequest) {
        return false;
    }

    TimingVeuMemoryRequest request;
    request.addr = addr;
    request.isWrite = isWrite;
    request.data = data;
    pendingMemory = pending;
    memoryResponseReady = false;
    if (!memoryRequest(request)) {
        pendingMemory = PendingMemory::None;
        return false;
    }

    if (isWrite) {
        ++memoryWrites;
    } else {
        ++memoryReads;
    }
    return true;
}

void
TimingVeu::completeMemoryRead(
    uint32_t addr, const std::array<uint8_t, VeuVectorBytes> &data)
{
    (void)addr;
    switch (pendingMemory) {
      case PendingMemory::Load1:
        input1 = data;
        break;
      case PendingMemory::Load2:
        input2 = data;
        break;
      case PendingMemory::Load3:
        input3 = data;
        break;
      case PendingMemory::StoreRead:
        storeMerge = data;
        break;
      default:
        break;
    }
    memoryResponseReady = true;
}

void
TimingVeu::completeMemoryWrite(uint32_t addr)
{
    (void)addr;
    memoryResponseReady = true;
}

uint32_t
TimingVeu::loadLane(const std::array<uint8_t, VeuVectorBytes> &data,
                    uint32_t lane)
{
    const uint32_t offset = lane * sizeof(uint32_t);
    return static_cast<uint32_t>(data[offset]) |
           (static_cast<uint32_t>(data[offset + 1]) << 8) |
           (static_cast<uint32_t>(data[offset + 2]) << 16) |
           (static_cast<uint32_t>(data[offset + 3]) << 24);
}

void
TimingVeu::storeLane(std::array<uint8_t, VeuVectorBytes> &data,
                     uint32_t lane, uint32_t value)
{
    const uint32_t offset = lane * sizeof(uint32_t);
    data[offset] = static_cast<uint8_t>(value & 0xff);
    data[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xff);
    data[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xff);
    data[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xff);
}

int32_t
TimingVeu::asSigned(uint32_t value)
{
    return static_cast<int32_t>(value);
}

void
TimingVeu::computeCurrentChunk()
{
    const VeuInstruction instruction = currentInstruction();
    result = {};

    int64_t reduction = 0;
    uint32_t reductionValue = 0;
    bool haveReduction = false;

    for (uint32_t lane = 0; lane < VeuLaneCount; ++lane) {
        const uint32_t a = loadLane(input1, lane);
        const uint32_t b = loadLane(input2, lane);
        const uint32_t c = loadLane(input3, lane);
        uint32_t out = 0;

        switch (instruction) {
          case VeuInstruction::Add:
            out = a + b;
            break;
          case VeuInstruction::Sub:
            out = a - b;
            break;
          case VeuInstruction::Min:
            out = asSigned(a) < asSigned(b) ? a : b;
            break;
          case VeuInstruction::Max:
            out = asSigned(a) > asSigned(b) ? a : b;
            break;
          case VeuInstruction::And:
            out = a & b;
            break;
          case VeuInstruction::Or:
            out = a | b;
            break;
          case VeuInstruction::Xor:
            out = a ^ b;
            break;
          case VeuInstruction::Move:
            out = a;
            break;
          case VeuInstruction::ShiftRightLogical:
            out = b >> (a & 0x1f);
            break;
          case VeuInstruction::ShiftRightArithmetic:
            out = static_cast<uint32_t>(asSigned(b) >> (a & 0x1f));
            break;
          case VeuInstruction::Multiply:
            out = static_cast<uint32_t>(
                static_cast<uint64_t>(a) * static_cast<uint64_t>(b));
            break;
          case VeuInstruction::MultiplyHigh:
            out = static_cast<uint32_t>(
                (static_cast<uint64_t>(a) * static_cast<uint64_t>(b)) >> 32);
            break;
          case VeuInstruction::MultiplyHighSignedUnsigned:
            out = static_cast<uint32_t>(
                (static_cast<int64_t>(asSigned(a)) *
                 static_cast<uint64_t>(b)) >> 32);
            break;
          case VeuInstruction::MultiplyAdd:
            out = static_cast<uint32_t>(
                static_cast<uint64_t>(a) * static_cast<uint64_t>(b) + c);
            break;
          case VeuInstruction::MultiplySubtract:
            out = static_cast<uint32_t>(
                static_cast<uint64_t>(a) * static_cast<uint64_t>(b) - c);
            break;
          case VeuInstruction::ReduceSum:
          case VeuInstruction::WidenReduceSum:
            reduction += asSigned(a);
            out = 0;
            break;
          case VeuInstruction::ReduceMin:
            if (!haveReduction || asSigned(a) < asSigned(reductionValue)) {
                reductionValue = a;
                haveReduction = true;
            }
            out = 0;
            break;
          case VeuInstruction::ReduceMax:
            if (!haveReduction || asSigned(a) > asSigned(reductionValue)) {
                reductionValue = a;
                haveReduction = true;
            }
            out = 0;
            break;
          default:
            out = 0;
            break;
        }
        storeLane(result, lane, out);
    }

    if (instruction == VeuInstruction::ReduceSum ||
        instruction == VeuInstruction::WidenReduceSum) {
        storeLane(result, VeuLaneCount - 1, static_cast<uint32_t>(reduction));
    } else if (instruction == VeuInstruction::ReduceMin ||
               instruction == VeuInstruction::ReduceMax) {
        storeLane(result, VeuLaneCount - 1, reductionValue);
    }
}

void
TimingVeu::advanceAfterStore()
{
    ++processedChunks;
    ++chunkIndex;
    if (chunksRemaining > 0) {
        --chunksRemaining;
    }

    currentReadAddr1 += VeuVectorBytes;
    currentReadAddr2 += VeuVectorBytes;
    currentReadAddr3 += VeuVectorBytes;
    currentWriteAddr += VeuVectorBytes;

    if (chunksRemaining == 0) {
        if (timing.finishCycles > 0) {
            remainingCycles = timing.finishCycles;
            currentState = State::Finish;
        } else {
            completeOperation();
        }
    } else {
        currentState = State::IssueLoad1;
    }
}

void
TimingVeu::completeOperation()
{
    csr.status &= ~1u;
    ++completedOperations;
    currentState = State::Idle;
}

void
TimingVeu::advanceOperation()
{
    switch (currentState) {
      case State::Idle:
        break;

      case State::Startup:
        if (remainingCycles > 1) {
            --remainingCycles;
        } else {
            remainingCycles = 0;
            currentState = State::IssueLoad1;
        }
        break;

      case State::IssueLoad1:
        if (issueMemory(PendingMemory::Load1, currentReadAddr1, false)) {
            currentState = State::WaitLoad1;
        }
        break;

      case State::WaitLoad1:
        ++loadWaitCycles;
        if (memoryResponseReady) {
            pendingMemory = PendingMemory::None;
            memoryResponseReady = false;
            currentState = needsSecondOperand() ? State::IssueLoad2 :
                State::Execute;
            remainingCycles = timing.executeLatency;
        }
        break;

      case State::IssueLoad2:
        if (issueMemory(PendingMemory::Load2, currentReadAddr2, false)) {
            currentState = State::WaitLoad2;
        }
        break;

      case State::WaitLoad2:
        ++loadWaitCycles;
        if (memoryResponseReady) {
            pendingMemory = PendingMemory::None;
            memoryResponseReady = false;
            currentState = needsThirdOperand() ? State::IssueLoad3 :
                State::Execute;
            remainingCycles = timing.executeLatency;
        }
        break;

      case State::IssueLoad3:
        if (issueMemory(PendingMemory::Load3, currentReadAddr3, false)) {
            currentState = State::WaitLoad3;
        }
        break;

      case State::WaitLoad3:
        ++loadWaitCycles;
        if (memoryResponseReady) {
            pendingMemory = PendingMemory::None;
            memoryResponseReady = false;
            currentState = State::Execute;
            remainingCycles = timing.executeLatency;
        }
        break;

      case State::Execute:
        ++executeCycles;
        if (remainingCycles > 1) {
            --remainingCycles;
        } else {
            remainingCycles = 0;
            computeCurrentChunk();
            if (!storesResult()) {
                advanceAfterStore();
            } else if (writeMaskIsFull()) {
                currentState = State::IssueStore;
            } else {
                currentState = State::IssueStoreRead;
            }
        }
        break;

      case State::IssueStoreRead:
        if (issueMemory(PendingMemory::StoreRead, currentWriteAddr, false)) {
            currentState = State::WaitStoreRead;
        }
        break;

      case State::WaitStoreRead:
        ++storeWaitCycles;
        if (memoryResponseReady) {
            pendingMemory = PendingMemory::None;
            memoryResponseReady = false;
            for (uint32_t i = 0; i < VeuVectorBytes; ++i) {
                if ((currentWriteMask & (uint32_t{1} << i)) != 0) {
                    storeMerge[i] = result[i];
                }
            }
            result = storeMerge;
            currentState = State::IssueStore;
        }
        break;

      case State::IssueStore:
        if (issueMemory(PendingMemory::StoreWrite, currentWriteAddr, true,
                        result)) {
            currentState = State::WaitStore;
        }
        break;

      case State::WaitStore:
        ++storeWaitCycles;
        if (memoryResponseReady) {
            pendingMemory = PendingMemory::None;
            memoryResponseReady = false;
            advanceAfterStore();
        }
        break;

      case State::Finish:
        if (remainingCycles > 1) {
            --remainingCycles;
        } else {
            remainingCycles = 0;
            completeOperation();
        }
        break;
    }
}

void
TimingVeu::clock(const VeuRequest &request)
{
    if (operationBusy()) {
        ++busyCycles;
    }
    advanceOperation();

    switch (controlState) {
      case ControlState::Idle:
        if (request.hasTransaction()) {
            acceptRequest(request);
        }
        break;

      case ControlState::Respond:
        ++responses;
        controlState = ControlState::Recovery;
        break;

      case ControlState::Recovery:
        if (request.hasTransaction()) {
            acceptRequest(request);
        } else {
            controlState = ControlState::Idle;
        }
        break;
    }
}

} // namespace brs
} // namespace gem5

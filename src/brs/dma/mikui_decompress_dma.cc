#include "brs/dma/mikui_decompress_dma.hh"

#include <algorithm>

#include "base/logging.hh"
#include "brs/dma/mikui_decompressor.hh"
#include "mem/packet_access.hh"

namespace gem5
{

MikuiDecompressDma::DeviceStats::DeviceStats(statistics::Group *parent)
  : statistics::Group(parent),
    ADD_STAT(pioReads, "Mikui decompression DMA PIO reads"),
    ADD_STAT(pioWrites, "Mikui decompression DMA PIO writes"),
    ADD_STAT(readWords, "Mikui decompression DMA 32-bit source reads"),
    ADD_STAT(writeWords, "Mikui decompression DMA 32-bit destination writes"),
    ADD_STAT(inputBytes, "Compressed bytes read by the DMA"),
    ADD_STAT(outputBytes, "Decompressed bytes written by the DMA"),
    ADD_STAT(completedOperations, "Completed decompression operations"),
    ADD_STAT(decodeErrors, "Rejected or malformed decompression operations"),
    ADD_STAT(irqAssertions, "DMA interrupt assertions"),
    ADD_STAT(outputChecksum, "FNV-1a checksum of the latest output")
{}

MikuiDecompressDma::MikuiDecompressDma(const Params &params)
  : DmaDevice(params),
    stats(this),
    pioAddr(params.pio_addr),
    pioSize(params.pio_size),
    pioDelay(params.pio_latency),
    maxInputBytes(params.max_input_bytes),
    maxOutputBytes(params.max_output_bytes),
    irqPort(name() + ".irq", 0, this),
    startEvent([this] { start(); }, name() + ".start"),
    issueReadEvent([this] { issueRead(); }, name() + ".issueRead"),
    readDoneEvent([this] { readDone(); }, name() + ".readDone"),
    issueWriteEvent([this] { issueWrite(); }, name() + ".issueWrite"),
    writeDoneEvent([this] { writeDone(); }, name() + ".writeDone")
{
    fatal_if(pioSize < LengthOffset + AhbWordBytes,
             "Mikui decompression DMA PIO window is too small");
    fatal_if(maxInputBytes < AhbWordBytes || maxOutputBytes < AhbWordBytes,
             "Mikui decompression DMA buffers must hold at least one word");
}

AddrRangeList
MikuiDecompressDma::getAddrRanges() const
{
    return {RangeSize(pioAddr, pioSize)};
}

uint32_t
MikuiDecompressDma::mergeBytes(
    uint32_t oldValue, uint32_t newValue, uint8_t byteEnable)
{
    uint32_t merged = oldValue;
    for (unsigned byte = 0; byte < AhbWordBytes; ++byte) {
        if (byteEnable & (1u << byte)) {
            const uint32_t mask = 0xffu << (byte * 8);
            merged = (merged & ~mask) | (newValue & mask);
        }
    }
    return merged;
}

uint32_t
MikuiDecompressDma::readRegister(Addr offset) const
{
    switch (offset) {
      case CtrlOffset:
        return control;
      case SourceOffset:
        return static_cast<uint32_t>(source);
      case DestinationOffset:
        return static_cast<uint32_t>(destination);
      case LengthOffset:
        return length;
      default:
        return 0;
    }
}

Tick
MikuiDecompressDma::read(PacketPtr pkt)
{
    panic_if(pkt->getSize() != AhbWordBytes || (pkt->getAddr() & 3),
             "Mikui decompression DMA requires aligned 32-bit PIO reads");
    const Addr offset = pkt->getAddr() - pioAddr;
    panic_if(offset >= pioSize, "Mikui decompression DMA PIO read out of range");
    pkt->setLE<uint32_t>(readRegister(offset));
    pkt->makeResponse();
    ++stats.pioReads;
    return pioDelay;
}

void
MikuiDecompressDma::writeRegister(
    Addr offset, uint32_t value, uint8_t byteEnable)
{
    switch (offset) {
      case CtrlOffset:
        control = mergeBytes(control, value, byteEnable);
        if ((control & 1u) && state == State::Idle && !startPending) {
            startPending = true;
            setIrq(false);
            scheduleNext(startEvent);
        }
        break;
      case SourceOffset:
        source = mergeBytes(
            static_cast<uint32_t>(source), value, byteEnable);
        break;
      case DestinationOffset:
        destination = mergeBytes(
            static_cast<uint32_t>(destination), value, byteEnable);
        break;
      case LengthOffset:
        length = mergeBytes(length, value, byteEnable);
        break;
      default:
        break;
    }
}

Tick
MikuiDecompressDma::write(PacketPtr pkt)
{
    panic_if(pkt->getSize() != AhbWordBytes || (pkt->getAddr() & 3),
             "Mikui decompression DMA requires aligned 32-bit PIO writes");
    const Addr offset = pkt->getAddr() - pioAddr;
    panic_if(offset >= pioSize, "Mikui decompression DMA PIO write out of range");

    uint8_t byteEnable = 0xf;
    if (!pkt->req->getByteEnable().empty()) {
        byteEnable = 0;
        const auto &enables = pkt->req->getByteEnable();
        for (unsigned byte = 0;
             byte < std::min<size_t>(AhbWordBytes, enables.size()); ++byte) {
            byteEnable |= enables[byte] ? (1u << byte) : 0u;
        }
    }
    writeRegister(offset, pkt->getLE<uint32_t>(), byteEnable);
    pkt->makeResponse();
    ++stats.pioWrites;
    return pioDelay;
}

Port &
MikuiDecompressDma::getPort(const std::string &ifName, PortID idx)
{
    if (ifName == "irq") {
        return irqPort;
    }
    return DmaDevice::getPort(ifName, idx);
}

void
MikuiDecompressDma::scheduleNext(Event &event)
{
    panic_if(event.scheduled(), "Mikui decompression DMA event scheduled twice");
    schedule(event, clockEdge(Cycles(1)));
}

void
MikuiDecompressDma::start()
{
    startPending = false;
    control &= ~uint32_t{1};
    if (length == 0 || (length & 3) || length > maxInputBytes ||
        (source & 3) || (destination & 3)) {
        fail("source, destination, and non-zero length must be word aligned; "
             "length must fit max_input_bytes");
        return;
    }

    input.assign(length, 0);
    output.clear();
    transferOffset = 0;
    state = State::Reading;
    issueRead();
}

void
MikuiDecompressDma::issueRead()
{
    panic_if(state != State::Reading || transferOffset >= input.size(),
             "invalid Mikui decompression DMA read state");
    dmaRead(source + transferOffset, AhbWordBytes, &readDoneEvent,
            input.data() + transferOffset);
    ++stats.readWords;
    stats.inputBytes += AhbWordBytes;
}

void
MikuiDecompressDma::readDone()
{
    transferOffset += AhbWordBytes;
    if (transferOffset < input.size()) {
        scheduleNext(issueReadEvent);
        return;
    }
    state = State::Decoding;
    decode();
}

void
MikuiDecompressDma::decode()
{
    const auto result = brs::MikuiDecompressor::decode(
        input, maxOutputBytes);
    if (!result.success) {
        fail(result.error);
        return;
    }

    output.resize(result.outputWords.size() * AhbWordBytes);
    uint32_t checksum = 2166136261u;
    for (size_t word = 0; word < result.outputWords.size(); ++word) {
        const uint32_t value = result.outputWords[word];
        for (unsigned byte = 0; byte < AhbWordBytes; ++byte) {
            const uint8_t valueByte = value >> (byte * 8);
            output[word * AhbWordBytes + byte] = valueByte;
            checksum = (checksum ^ valueByte) * 16777619u;
        }
    }
    stats.outputChecksum = checksum;
    transferOffset = 0;
    state = State::Writing;
    if (output.empty()) {
        finish();
    } else {
        scheduleNext(issueWriteEvent);
    }
}

void
MikuiDecompressDma::issueWrite()
{
    panic_if(state != State::Writing || transferOffset >= output.size(),
             "invalid Mikui decompression DMA write state");
    dmaWrite(destination + transferOffset, AhbWordBytes, &writeDoneEvent,
             output.data() + transferOffset);
    ++stats.writeWords;
    stats.outputBytes += AhbWordBytes;
}

void
MikuiDecompressDma::writeDone()
{
    transferOffset += AhbWordBytes;
    if (transferOffset < output.size()) {
        scheduleNext(issueWriteEvent);
    } else {
        finish();
    }
}

void
MikuiDecompressDma::finish()
{
    state = State::Idle;
    ++stats.completedOperations;
    setIrq(true);
}

void
MikuiDecompressDma::fail(const std::string &reason)
{
    state = State::Idle;
    ++stats.decodeErrors;
    warn("Mikui decompression DMA rejected operation: %s", reason);
    setIrq(true);
}

void
MikuiDecompressDma::setIrq(bool level)
{
    if (irqAsserted == level) {
        return;
    }
    irqAsserted = level;
    if (level) {
        irqPort.raise();
        ++stats.irqAssertions;
    } else {
        irqPort.lower();
    }
}

} // namespace gem5

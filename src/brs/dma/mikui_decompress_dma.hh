#ifndef __BRS_DMA_MIKUI_DECOMPRESS_DMA_HH__
#define __BRS_DMA_MIKUI_DECOMPRESS_DMA_HH__

#include <cstdint>
#include <vector>

#include "base/statistics.hh"
#include "dev/dma_device.hh"
#include "dev/intpin.hh"
#include "params/MikuiDecompressDma.hh"
#include "sim/eventq.hh"

namespace gem5
{

// Independent gem5 representation of the dma_decompress/dt_dma AHB
// peripheral selected by Mikui's VCS file list.  Software reaches the PIO
// slave registers, while all source reads and destination writes leave via
// DmaDevice::dmaPort exactly as an independent RTL AHB master would.
class MikuiDecompressDma : public DmaDevice
{
  public:
    PARAMS(MikuiDecompressDma);
    explicit MikuiDecompressDma(const Params &params);

    AddrRangeList getAddrRanges() const override;
    Tick read(PacketPtr pkt) override;
    Tick write(PacketPtr pkt) override;
    Port &getPort(
        const std::string &ifName,
        PortID idx = InvalidPortID) override;

  private:
    static constexpr Addr CtrlOffset = 0x00;
    static constexpr Addr SourceOffset = 0x04;
    static constexpr Addr DestinationOffset = 0x08;
    static constexpr Addr LengthOffset = 0x0c;
    static constexpr unsigned AhbWordBytes = 4;

    enum class State : uint8_t
    {
        Idle,
        Reading,
        Decoding,
        Writing,
    };

    struct DeviceStats : public statistics::Group
    {
        statistics::Scalar pioReads;
        statistics::Scalar pioWrites;
        statistics::Scalar readWords;
        statistics::Scalar writeWords;
        statistics::Scalar inputBytes;
        statistics::Scalar outputBytes;
        statistics::Scalar completedOperations;
        statistics::Scalar decodeErrors;
        statistics::Scalar irqAssertions;
        statistics::Scalar outputChecksum;

        explicit DeviceStats(statistics::Group *parent);
    } stats;

    const Addr pioAddr;
    const Addr pioSize;
    const Tick pioDelay;
    const uint32_t maxInputBytes;
    const uint32_t maxOutputBytes;
    IntSourcePin<MikuiDecompressDma> irqPort;

    uint32_t control = 0;
    Addr source = 0;
    Addr destination = 0;
    uint32_t length = 0;
    State state = State::Idle;
    bool startPending = false;
    bool irqAsserted = false;
    size_t transferOffset = 0;
    std::vector<uint8_t> input;
    std::vector<uint8_t> output;

    EventFunctionWrapper startEvent;
    EventFunctionWrapper issueReadEvent;
    EventFunctionWrapper readDoneEvent;
    EventFunctionWrapper issueWriteEvent;
    EventFunctionWrapper writeDoneEvent;

    uint32_t readRegister(Addr offset) const;
    void writeRegister(Addr offset, uint32_t value, uint8_t byteEnable);
    void start();
    void issueRead();
    void readDone();
    void decode();
    void issueWrite();
    void writeDone();
    void finish();
    void fail(const std::string &reason);
    void setIrq(bool level);
    void scheduleNext(Event &event);
    static uint32_t mergeBytes(
        uint32_t oldValue, uint32_t newValue, uint8_t byteEnable);
};

} // namespace gem5

#endif // __BRS_DMA_MIKUI_DECOMPRESS_DMA_HH__

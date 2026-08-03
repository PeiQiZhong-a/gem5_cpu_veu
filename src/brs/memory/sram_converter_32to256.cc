#include "brs/memory/sram_converter_32to256.hh"

namespace gem5
{
namespace brs
{

void
SramConverter32To256::reset()
{
    currentState = State::Idle;
    requestReg = {};
    wordOffsetReg = 0;
    writeReg = false;
    responseReg = {};
}

SramConverter32To256Output
SramConverter32To256::evaluate() const
{
    SramConverter32To256Output output;
    output.master = responseReg;
    if (currentState == State::WaitAck) {
        output.sram = requestReg;
    }
    return output;
}

void
SramConverter32To256::clock(
    const Sram32Request &masterRequest,
    const Sram256Response &sramResponse)
{
    responseReg = {};

    switch (currentState) {
      case State::Idle:
        if (masterRequest.valid) {
            const uint8_t wordOffset =
                static_cast<uint8_t>((masterRequest.address >> 2) & 0x7);
            requestReg = {};
            requestReg.valid = true;
            requestReg.address = masterRequest.address & ~uint32_t{0x1f};
            wordOffsetReg = wordOffset;
            writeReg = masterRequest.isWrite();
            if (writeReg) {
                const uint32_t byteOffset = wordOffset * 4;
                requestReg.writeStrobe =
                    static_cast<uint32_t>(masterRequest.writeStrobe) <<
                    byteOffset;
                for (uint32_t byte = 0; byte < 4; ++byte) {
                    requestReg.writeData[byteOffset + byte] =
                        static_cast<uint8_t>(
                            masterRequest.writeData >> (byte * 8));
                }
            }
            currentState = State::WaitAck;
        }
        break;

      case State::WaitAck:
        if (sramResponse.valid) {
            responseReg.valid = true;
            responseReg.isWrite = writeReg;
            if (!writeReg) {
                const uint32_t byteOffset = wordOffsetReg * 4;
                for (uint32_t byte = 0; byte < 4; ++byte) {
                    responseReg.readData |=
                        static_cast<uint32_t>(
                            sramResponse.readData[byteOffset + byte]) <<
                        (byte * 8);
                }
            }
            requestReg = {};
            currentState = State::Idle;
        }
        break;
    }
}

} // namespace brs
} // namespace gem5

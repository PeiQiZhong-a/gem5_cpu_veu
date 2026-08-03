#include "brs/memory/sram_converter_32to128.hh"

namespace gem5
{
namespace brs
{

void
SramConverter32To128::reset()
{
    currentState = State::Idle;
    addressReg = 0;
    writeDataReg.fill(0);
    writeStrobeReg = 0;
    wordOffsetReg = 0;
    writeReg = false;
    sramOutputReg = {};
    masterOutputReg = {};
}

SramConverter32To128Output
SramConverter32To128::evaluate() const
{
    return {sramOutputReg, masterOutputReg};
}

void
SramConverter32To128::clock(
    const Sram32Request &masterRequest,
    const Sram128Response &sramResponse)
{
    State nextState = currentState;
    switch (currentState) {
      case State::Idle:
        sramOutputReg.valid = false;
        masterOutputReg.valid = false;
        if (masterRequest.valid) {
            addressReg = masterRequest.address & ~uint32_t{0x0f};
            wordOffsetReg =
                static_cast<uint8_t>((masterRequest.address >> 2) & 0x3);
            writeReg = masterRequest.isWrite();
            writeDataReg.fill(0);
            writeStrobeReg = 0;
            if (writeReg) {
                const uint8_t byteOffset = wordOffsetReg * 4;
                writeStrobeReg =
                    static_cast<uint16_t>(masterRequest.writeStrobe) <<
                    byteOffset;
                for (uint8_t byte = 0; byte < 4; ++byte) {
                    writeDataReg[byteOffset + byte] =
                        static_cast<uint8_t>(
                            masterRequest.writeData >> (byte * 8));
                }
            }
            nextState = State::Convert;
        }
        break;

      case State::Convert:
        sramOutputReg.valid = true;
        sramOutputReg.address = addressReg;
        sramOutputReg.writeData = writeDataReg;
        sramOutputReg.writeStrobe = writeStrobeReg;
        nextState = State::WaitAck;
        break;

      case State::WaitAck:
        if (sramResponse.valid) {
            sramOutputReg.valid = false;
            masterOutputReg.valid = true;
            masterOutputReg.isWrite = writeReg;
            masterOutputReg.readData = 0;
            if (!writeReg) {
                const uint8_t byteOffset = wordOffsetReg * 4;
                for (uint8_t byte = 0; byte < 4; ++byte) {
                    masterOutputReg.readData |=
                        static_cast<uint32_t>(
                            sramResponse.readData[byteOffset + byte]) <<
                        (byte * 8);
                }
            }
            nextState = State::Idle;
        }
        break;
    }
    currentState = nextState;
}

} // namespace brs
} // namespace gem5

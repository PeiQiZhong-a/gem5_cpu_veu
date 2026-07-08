#ifndef __BRS_PIPELINE_COMPRESSED_DECODE_HH__
#define __BRS_PIPELINE_COMPRESSED_DECODE_HH__

#include <cstdint>

namespace gem5
{

bool isCompressedInstr(uint32_t instr);
bool expandCompressedInstr(uint32_t instr, uint32_t &expanded);

} // namespace gem5

#endif

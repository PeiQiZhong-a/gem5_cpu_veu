#ifndef __BRS_PIPELINE_TYPES_HH__
#define __BRS_PIPELINE_TYPES_HH__

#include <cstdint>

namespace gem5
{

//回头梳理一下
enum class InstrKind 
{
    NOP,
    ADDI,
    ADD,
    SUB,
    LW,
    SW,
    BEQ,
    JAL,
    BNE,
    BLT,
    BGE,
    BLTU,
    BGEU,
    JALR,

    LUI,
    AUIPC,

    XORI,
    ORI,
    ANDI,
    SLTI,
    SLTIU,
    SLLI,
    SRLI,
    SRAI,
    SLL,
    SRL,
    SRA,

    XOR,
    OR,
    AND,
    SLT,
    SLTU,

    MUL,
    MULH,
    MULHSU,
    MULHU,
    DIV,
    DIVU,
    REM,
    REMU,

    LB,
    LBU,
    LH,
    LHU,
    SB,
    SH,

    FLW,
    FSW,
    FMV_X_W,
    FMV_W_X,
    FP_ARITH,

    FENCE,
    FENCE_I,
    CSRRW,
    CSRRS,
    CSRRC,
    CSRRWI,
    CSRRSI,
    CSRRCI,
    ECALL,
    EBREAK,
    MRET,
    WFI,

    VEU,
    SAU,

    INVALID
};


enum class AluOp
{
    NONE,
    ADD,
    SUB,
    MUL,
    MULH,
    MULHSU,
    MULHU,
    DIV,
    DIVU,
    REM,
    REMU
};

enum class CsrWriteType
{
    WRITE,
    SET,
    CLEAR
};

enum class WbSel
{
    NONE,
    ALU,
    MEM,
    PC4,
    VEU,
    SAU
};

} // namespace gem5

#endif

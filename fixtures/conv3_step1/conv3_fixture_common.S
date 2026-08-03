# Step 1 comparison fixture body.
#
# The entry files select FIXTURE_MODE:
#   0 = generated_legacy_control
#   1 = generated_four_ins_control_matched
#   2 = generated_four_ins_full_offload
#
# This is deliberately a small, reviewable RV32I program. The generated
# instruction images are comparison artifacts, not output from the original
# Conv compiler. All custom SAU words are explicit .word values so the
# instruction transport remains auditable.

.option norvc

.equ INPUT_BASE,       0x29130000
.equ WEIGHT_BASE,      0x29132000
.equ BIAS_BASE,        0x29132900
.equ OUTPUT_BASE,      0x29132920
.equ PADDED_BASE,      0x29136000
.equ INPUT_BYTES,      8192
.equ PADDED_BYTES,     13824
.equ CONFIG_START,     0x00004000
.equ PADDING_START,    0x00000200
.equ SHARED_TAIL_PC,   0x00005000

# The custom instruction uses t0=x5 as rs1 and t1=x6 as rs2. The rd field is
# x0, matching normal MSETINS use. funct7 selects slot 1..4 and funct3=1 is
# the SAU write encoding used by the gem5 decoder.
.equ MSET1_T0_T1,      0x0062906b
.equ MSET2_T0_T1,      0x0662906b
.equ MSET3_T0_T1,      0x0c62906b
.equ MSET4_T0_T1,      0x1262906b

.macro LEGACY_TILE kaddr, aaddr, daddr, bias
    # Legacy msetins3 carries low address fields for weight/input.
    li      t0, \kaddr
    li      t1, \aaddr
    .word   MSET3_T0_T1

    # Legacy msetins4 carries start/id/destination and mode/flow fields.
    li      t0, \daddr
    li      t1, \bias
    .word   MSET4_T0_T1
.endm

.section .text
.globl _start
.type _start,@function

_start:
    # Keep the baseline stack convention and materialize the fixed allocation
    # map used by all three artifacts.
    li      sp, 0x2914fffc
    li      s0, INPUT_BASE
    li      s1, WEIGHT_BASE
    li      s2, BIAS_BASE
    li      s3, OUTPUT_BASE
    li      s4, PADDED_BASE

    # Baseline input initialization: input[i] = i / 16 for 8192 INT8 bytes.
    li      t0, 0
    li      t1, INPUT_BYTES
input_init:
    srli    t2, t0, 4
    add     t3, s0, t0
    sb      t2, 0(t3)
    addi    t0, t0, 1
    bne     t0, t1, input_init

    # The software-padding region is intentionally fixed in the image. The
    # full-offload artifact leaves it zero-filled and lets future Conv3 handle
    # padding internally.
    .org    PADDING_START
.if FIXTURE_MODE != 2
    # Clear C*18*48 bytes so the one-byte border is deterministic zero.
    li      t0, 0
    li      t1, PADDED_BYTES
pad_clear:
    add     t2, s4, t0
    sb      zero, 0(t2)
    addi    t0, t0, 1
    bne     t0, t1, pad_clear

    # Copy each logical C*H*W plane into the interior of the padded C*18*48
    # plane. This is the software preprocessing retained by the control-
    # matched artifact.
    li      t0, 0
    li      t1, 16
    mv      t2, s0
    mv      t3, s4
pad_channel:
    mv      t4, t2
    addi    t5, t3, 49       # one padded row plus one padded column
    li      t6, 16
pad_row:
    li      a0, 32
pad_col:
    lbu     a1, 0(t4)
    sb      a1, 0(t5)
    addi    t4, t4, 1
    addi    t5, t5, 1
    addi    a0, a0, -1
    bne     a0, zero, pad_col
    addi    t5, t5, 16       # skip the right-hand border
    addi    t6, t6, -1
    bne     t6, zero, pad_row
    addi    t2, t2, 512      # 16*32 input plane
    addi    t3, t3, 864      # 18*48 padded plane
    addi    t0, t0, 1
    bne     t0, t1, pad_channel
.endif

    # Leave a fixed, easy-to-audit gap between preprocessing and configuration.
    .org    CONFIG_START
.if FIXTURE_MODE == 0
    # Legacy header: ins1/ins2 and ins3/ins4 are written once.
    li      t0, 0x0000000c  # register_mode=0, kernel=3, stride=1
    li      t1, 0x00000030  # cutbit=12, transpose_mode=0
    .word   MSET1_T0_T1
    li      t0, 0x00020301  # k_x=1, a_x=3, d_x=2
    li      t1, 0x00101209  # k_ch=9, a_ch=18, d_ch=16
    .word   MSET2_T0_T1

    # 16 output rows x 2 spatial tiles = 32 legacy starts.
    LEGACY_TILE 0x132000, 0x136000, 0x26524001, 0x10612900
    LEGACY_TILE 0x132000, 0x136010, 0x26526001, 0x10612900
    LEGACY_TILE 0x132000, 0x136030, 0x26528001, 0x10612900
    LEGACY_TILE 0x132000, 0x136040, 0x2652a001, 0x10612900
    LEGACY_TILE 0x132000, 0x136060, 0x2652c001, 0x10612900
    LEGACY_TILE 0x132000, 0x136070, 0x2652e001, 0x10612900
    LEGACY_TILE 0x132000, 0x136090, 0x26530001, 0x10612900
    LEGACY_TILE 0x132000, 0x1360a0, 0x26532001, 0x10612900
    LEGACY_TILE 0x132000, 0x1360c0, 0x26534001, 0x10612900
    LEGACY_TILE 0x132000, 0x1360d0, 0x26536001, 0x10612900
    LEGACY_TILE 0x132000, 0x1360f0, 0x26538001, 0x10612900
    LEGACY_TILE 0x132000, 0x136100, 0x2653a001, 0x10612900
    LEGACY_TILE 0x132000, 0x136120, 0x2653c001, 0x10612900
    LEGACY_TILE 0x132000, 0x136130, 0x2653e001, 0x10612900
    LEGACY_TILE 0x132000, 0x136150, 0x26540001, 0x10612900
    LEGACY_TILE 0x132000, 0x136160, 0x26542001, 0x10612900
    LEGACY_TILE 0x132000, 0x136180, 0x26544001, 0x10612900
    LEGACY_TILE 0x132000, 0x136190, 0x26546001, 0x10612900
    LEGACY_TILE 0x132000, 0x1361b0, 0x26548001, 0x10612900
    LEGACY_TILE 0x132000, 0x1361c0, 0x2654a001, 0x10612900
    LEGACY_TILE 0x132000, 0x1361e0, 0x2654c001, 0x10612900
    LEGACY_TILE 0x132000, 0x1361f0, 0x2654e001, 0x10612900
    LEGACY_TILE 0x132000, 0x136210, 0x26550001, 0x10612900
    LEGACY_TILE 0x132000, 0x136220, 0x26552001, 0x10612900
    LEGACY_TILE 0x132000, 0x136240, 0x26554001, 0x10612900
    LEGACY_TILE 0x132000, 0x136250, 0x26556001, 0x10612900
    LEGACY_TILE 0x132000, 0x136270, 0x26558001, 0x10612900
    LEGACY_TILE 0x132000, 0x136280, 0x2655a001, 0x10612900
    LEGACY_TILE 0x132000, 0x1362a0, 0x2655c001, 0x10612900
    LEGACY_TILE 0x132000, 0x1362b0, 0x2655e001, 0x10612900
    LEGACY_TILE 0x132000, 0x1362d0, 0x26560001, 0x10612900
    LEGACY_TILE 0x132000, 0x1362e0, 0x26562001, 0x10612900
.else
    # New Conv3 ABI: exactly one ordered msetins1..4 transaction.
    li      t0, 0x29130000  # input_base
    li      t1, 0x29132000  # weight_base
    .word   MSET1_T0_T1
    li      t0, 0x29132900  # bias_base
    li      t1, 0x29132920  # output_base
    .word   MSET2_T0_T1
    li      t0, 0x00200010  # H=16, W=32
    li      t1, 0x04100001  # N=1, C=16, OC=16
    .word   MSET3_T0_T1
    li      t0, 0x80001b11  # start=1, kernel=3, cutbit=12, P=1, S=1, v1
    li      t1, 0xc3000000  # magic=0xc3
    .word   MSET4_T0_T1
.endif

    # Keep the post-configuration control flow identical and at a fixed PC.
    jal     zero, shared_tail

.org    SHARED_TAIL_PC
shared_tail:
    ebreak
.size _start, . - _start

from m5.params import *
from m5.proxy import *
from m5.objects.ClockedObject import ClockedObject

class PipelineMiniCPU(ClockedObject):
    type = 'PipelineMiniCPU'
    cxx_header = "brs/pipeline_mini_cpu.hh"
    cxx_class = "gem5::PipelineMiniCPU"
    system = Param.System(Parent.any, "System this CPU belongs to")
    program_file = Param.String("", "Path to workload hex file")
    elf_file = Param.String("", "Path to ELF binary to load into memory")
    preloaded_program = Param.Bool(False, "Use an already-initialized instruction memory image")
    preloaded_program_size = Param.Addr(0, "Instruction memory image size in bytes")

    dmem_hex_file = Param.String("", "Spirit-style text hex file for DMEM initialization (byte granularity)")

    max_cycles = Param.UInt64(20, "Maximum cycles to run")
    reset_cycles = Param.UInt64(
        10,
        "Clock edges held in reset before the first active CPU cycle",
    )
    fake_veu_latency = Param.UInt32(
        1,
        "FakeVEU response latency in CPU cycles",
    )
    fake_veu_response_data = Param.UInt32(
        0,
        "Fixed csr_rdata returned by FakeVEU",
    )

    text_base    = Param.Addr(0x80000000, "Code base address")
    dmem_base    = Param.Addr(0x80200000, "DMEM base address")

    icache_enabled = Param.Bool(
        False,
        "Enable the optional simple direct-mapped I-cache (disabled in the Spirit testbench profile)",
    )
    icache_size = Param.UInt32(4096, "Simple I-cache capacity in bytes")
    icache_line_size = Param.UInt32(64, "Simple I-cache line size in bytes")
    frontend_burst_bytes = Param.UInt32(16, "RV-NEW-style instruction fetch burst size")
    instr_fifo_depth = Param.UInt32(12, "RV-NEW-style instruction FIFO depth")

    inst_port = RequestPort("Instruction fetch port")
    data_port = RequestPort("Data access port")

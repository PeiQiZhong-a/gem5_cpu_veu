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

    dmem_hex_file = Param.String(
        "",
        "DMEM text image: byte tokens for spirit-like or dut-kui memory",
    )

    tb_memory_enabled = Param.Bool(
        False,
        "Route CPU IBus, DBus, VEU, and SAU through an RTL memory model",
    )
    tb_memory_kind = Param.String(
        "dut-kui",
        "RTL memory model: dut-kui or npu-lpnpu-mikui",
    )
    tb_imem_image_file = Param.String(
        "", "Raw image loaded into the selected RTL instruction SRAM")
    tb_dmem_image_file = Param.String(
        "", "Raw image loaded into the selected RTL data SRAM")
    tb_inst_base = Param.Addr(0x00000000, "RTL instruction SRAM base")
    tb_inst_size = Param.Addr(0x00040000, "RTL instruction SRAM size")
    tb_data_base = Param.Addr(0x29120000, "RTL data SRAM base")
    tb_data_size = Param.Addr(0x00040000, "RTL decoded data window size")

    max_cycles = Param.UInt64(
        2_000_000,
        "Maximum cycles: total clock edges including reset in RTL-testbench mode, active CPU cycles otherwise",
    )
    reset_cycles = Param.UInt64(
        100,
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
    irq_external = Param.UInt32(0, "External IRQ input bitmap")
    irq_software = Param.Bool(False, "Software IRQ input level")
    irq_timer = Param.Bool(False, "Timer IRQ input level")
    debug_halt = Param.Bool(False, "Debug halt request input level")
    debug_halt_on_reset = Param.Bool(False, "Debug halt-on-reset input level")
    debug_resume = Param.Bool(False, "Debug resume request input level")
    debug_data0 = Param.UInt32(0, "External debug data0 read value")
    debug_instr = Param.UInt32(0, "Injected debug instruction bits")
    debug_instr_valid = Param.Bool(False, "Injected debug instruction valid")
    cycle_trace_file = Param.String(
        "", "Optional per-cycle RTL-comparison trace written in gem5 output")
    veu_model = Param.String(
        "fake",
        "VEU backend model: fake or timing",
    )
    veu_input_fifo_depth = Param.UInt32(
        4,
        "TimingVEU input FIFO depth in 256-bit chunks",
    )
    veu_execute_latency = Param.UInt32(
        3,
        "TimingVEU execute latency per 256-bit chunk in CPU cycles",
    )
    veu_execute_ii = Param.UInt32(
        1,
        "TimingVEU default VFU initiation interval in CPU cycles",
    )
    veu_vsu_latency = Param.UInt32(
        1,
        "TimingVEU VSU latency in CPU cycles",
    )
    veu_timing_profile = Param.String(
        "",
        "Normalized TimingVEU per-operation timing profile CSV",
    )
    veu_cycle_trace = Param.String(
        "",
        "Optional TimingVEU structural cycle trace CSV output",
    )
    veu_startup_cycles = Param.UInt32(
        0,
        "TimingVEU startup cycles before issuing the first load",
    )
    veu_finish_cycles = Param.UInt32(
        0,
        "TimingVEU finish cycles before reporting completion",
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
    veu_port = RequestPort("VEU data access port")

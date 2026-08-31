import argparse

import m5
from m5.objects import *
from m5.objects import PipelineMiniCPU
from m5.util.convert import toMemorySize


parser = argparse.ArgumentParser(
    description="Run BRS PipelineMiniCPU with selectable gem5 memory platform."
)
parser.add_argument("--binary", default="", help="Path to RISC-V ELF binary")
parser.add_argument("--program-file", default="", help="Path to instruction hex file")
parser.add_argument(
    "--max-cycles",
    type=int,
    default=None,
    help=("Timeout in cycles. Defaults to 2,000,000 total clock edges "
          "including reset for RTL testbench modes, or 200 active CPU cycles "
          "for other memory modes."),
)
parser.add_argument(
    "--clock-frequency",
    default="100MHz",
    help="CPU clock. The Spirit testbench reference clock is 100MHz.",
)
parser.add_argument(
    "--reset-cycles",
    type=int,
    default=None,
    help="Reset clock edges before CPU cycle 1. Defaults to 10.",
)
parser.add_argument(
    "--fake-veu-latency",
    type=int,
    default=1,
    help="FakeVEU response latency in CPU cycles.",
)
parser.add_argument(
    "--fake-veu-response-data",
    type=lambda value: int(value, 0),
    default=0,
    help="Fixed FakeVEU csr_rdata value (decimal or 0x-prefixed).",
)
parser.add_argument(
    "--irq-external", type=lambda value: int(value, 0), default=0,
    help="External IRQ input bitmap (decimal or 0x-prefixed).",
)
parser.add_argument("--irq-software", action="store_true")
parser.add_argument("--irq-timer", action="store_true")
parser.add_argument("--debug-halt", action="store_true")
parser.add_argument("--debug-halt-on-reset", action="store_true")
parser.add_argument("--debug-resume", action="store_true")
parser.add_argument(
    "--debug-data0", type=lambda value: int(value, 0), default=0,
    help="External DMDATA0 read value (decimal or 0x-prefixed).",
)
parser.add_argument(
    "--debug-instr", type=lambda value: int(value, 0), default=0,
    help="Injected debug instruction bits (decimal or 0x-prefixed).",
)
parser.add_argument("--debug-instr-valid", action="store_true")
parser.add_argument(
    "--cycle-trace", default="",
    help="Write a machine-readable per-cycle CPU/bus trace to this output file.",
)
parser.add_argument(
    "--cycle-trace-compact",
    action="store_true",
    help=("Keep one compact CPU-PC record per cycle and add full fields only "
          "for HC/SAU/retire events."),
)
parser.add_argument(
    "--terminate-on-ebreak",
    action="store_true",
    help="Treat a retired EBREAK as the normal workload termination point.",
)
parser.add_argument(
    "--veu-model",
    choices=["fake", "timing"],
    default="fake",
    help="VEU backend model. fake preserves current tests; timing runs the VEU state-machine model.",
)
parser.add_argument(
    "--sau-model",
    choices=["stub", "sau_n"],
    default="stub",
    help=("SAU backend model. stub preserves current behavior; sau_n uses "
          "StreamingConvPipelineModel."),
)
parser.add_argument("--veu-input-fifo-depth", type=int, default=4)
parser.add_argument("--veu-execute-latency", type=int, default=3)
parser.add_argument("--veu-execute-ii", type=int, default=1)
parser.add_argument("--veu-vsu-latency", type=int, default=1)
parser.add_argument("--veu-timing-profile", default="")
parser.add_argument("--veu-terminal-behavior", default="")
parser.add_argument("--veu-cycle-trace", default="")
parser.add_argument("--veu-startup-cycles", type=int, default=4)
parser.add_argument("--veu-lock-start-delay-cycles", type=int, default=1)
parser.add_argument("--veu-finish-cycles", type=int, default=0)
parser.add_argument("--mem-size", default="64MiB")
parser.add_argument(
    "--mem-system",
    choices=[
        "ddr3", "simple", "split", "spirit-like",
        "rtl-dut-kui-tb", "rtl-npu-lpnpu-mikui",
        "rtl-npu-lpnpu-mikui-decompress-dma",
    ],
    default="rtl-dut-kui-tb",
    help=("RTL choices: rtl-dut-kui-tb keeps the legacy 256-bit platform; "
          "rtl-npu-lpnpu-mikui reproduces the two-bank 128-bit dut_mikui; "
          "rtl-npu-lpnpu-mikui-decompress-dma selects the three-bank "
          "dut_mikui_dma topology and adds the independent Mikui "
          "VCS-selected decompression AHB DMA."),
)
parser.add_argument(
    "--mem-latency",
    default="10ns",
    help="Fixed latency for simple memory modes. At the 100MHz Spirit reference clock, 10ns is one CPU cycle.",
)
parser.add_argument("--imem-latency", default="", help="Override IMEM latency in split mode")
parser.add_argument("--dmem-latency", default="", help="Override DMEM latency in split mode")
parser.add_argument(
    "--imem-image",
    default="",
    help="Raw instruction memory image for spirit-like IMEM SimpleMemory initialization.",
)
parser.add_argument(
    "--dmem-image",
    default="",
    help="Optional raw image to initialize DMEM. In split mode it defaults to --binary when provided; in spirit-like mode it is used only when explicitly set.",
)
parser.add_argument(
    "--dmem-hex",
    default="",
    help=(
        "DMEM text image: rtl-dut-kui-tb uses one 32-bit $readmemh word "
        "per token; spirit-like uses one byte per token."
    ),
)
parser.add_argument(
    "--dma-input-image",
    default="",
    help=("Raw compressed image loaded at 0x60000000 for the Mikui "
          "decompression-DMA mode."),
)
parser.add_argument(
    "--rtl-data-base",
    type=lambda value: int(value, 0),
    default=0x29120000,
    help="RTL-testbench data window base address.",
)
parser.add_argument(
    "--rtl-data-size",
    type=lambda value: int(value, 0),
    default=0x00040000,
    help="RTL-testbench decoded data window size.",
)
parser.add_argument(
    "--rtl-data-bank-size",
    type=lambda value: int(value, 0),
    default=0x00010000,
    help="RTL-testbench data bank size.",
)
parser.add_argument(
    "--rtl-data-bank-count",
    type=int,
    default=4,
    help="RTL-testbench decoded data bank count (at most four).",
)
parser.add_argument(
    "--rtl-data-real-bank-count",
    type=int,
    default=3,
    help="RTL-testbench physically backed data bank count.",
)
parser.add_argument(
    "--imem-base",
    default="0x0",
    help="IMEM base address for spirit-like mode. Must match --dmem-base.",
)
parser.add_argument(
    "--dmem-base",
    default="0x0",
    help="DMEM base address for spirit-like mode. Must match --imem-base.",
)
parser.add_argument(
    "--imem-size",
    default="8KiB",
    help="IMEM size for spirit-like mode. Must match --dmem-size.",
)
parser.add_argument(
    "--dmem-size",
    default="8KiB",
    help="DMEM size for spirit-like mode. Must match --imem-size.",
)
parser.add_argument("--icache-size", type=int, default=4096)
parser.add_argument("--icache-line-size", type=int, default=64)
icache_group = parser.add_mutually_exclusive_group()
icache_group.add_argument(
    "--icache",
    dest="icache_enabled",
    action="store_true",
    help="Enable the optional gem5-side I-cache.",
)
icache_group.add_argument(
    "--no-icache",
    dest="icache_enabled",
    action="store_false",
    help="Disable the optional gem5-side I-cache (Spirit testbench default).",
)
parser.set_defaults(icache_enabled=False)
parser.add_argument("--frontend-burst-bytes", type=int, default=16)
parser.add_argument("--instr-fifo-depth", type=int, default=12)
args = parser.parse_args()

dut_kui_tb_mode = args.mem_system == "rtl-dut-kui-tb"
mikui_dma_tb_mode = (
    args.mem_system == "rtl-npu-lpnpu-mikui-decompress-dma")
mikui_tb_mode = (
    args.mem_system == "rtl-npu-lpnpu-mikui" or mikui_dma_tb_mode)
rtl_tb_mode = dut_kui_tb_mode or mikui_tb_mode

max_cycles = args.max_cycles
if max_cycles is None:
    max_cycles = 2_000_000 if rtl_tb_mode else 200
if max_cycles <= 0:
    parser.error("--max-cycles must be positive")

reset_cycles = args.reset_cycles
if reset_cycles is None:
    reset_cycles = 10


def parse_addr(value):
    return int(value, 0)


if (args.mem_system == "spirit-like" or rtl_tb_mode) and args.binary:
    parser.error(
        "This memory mode uses --imem-image/--program-file and "
        "optional --dmem-image instead of --binary ELF preload"
    )

if (args.mem_system == "spirit-like" or rtl_tb_mode) and args.imem_image and args.program_file:
    parser.error("Use only one of raw --imem-image or hex --program-file")

if rtl_tb_mode and not (args.imem_image or args.program_file):
    parser.error("RTL testbench modes require --imem-image or --program-file")

if mikui_tb_mode and (args.irq_external or args.irq_software or args.irq_timer):
    parser.error(
        "rtl-npu-lpnpu-mikui ties external, software, and timer IRQ inputs to 0"
    )

if args.mem_system == "spirit-like" and parse_addr(args.imem_base) != parse_addr(args.dmem_base):
    parser.error("spirit-like mode requires --imem-base and --dmem-base to be identical")

if args.mem_system == "spirit-like" and toMemorySize(args.imem_size) != toMemorySize(args.dmem_size):
    parser.error("spirit-like mode requires --imem-size and --dmem-size to be identical")

if args.dmem_hex and args.dmem_image:
    parser.error("Use only one of --dmem-hex or raw --dmem-image")

if args.dmem_hex and args.mem_system != "spirit-like" and not rtl_tb_mode:
    parser.error(
        "--dmem-hex is supported only with spirit-like or RTL testbench modes")

if reset_cycles < 0:
    parser.error("--reset-cycles must be non-negative")

if dut_kui_tb_mode:
    if args.rtl_data_base < 0 or args.rtl_data_size <= 0:
        parser.error("RTL data base/size must describe a non-empty window")
    if args.rtl_data_bank_size <= 0:
        parser.error("--rtl-data-bank-size must be positive")
    if not 1 <= args.rtl_data_bank_count <= 4:
        parser.error("--rtl-data-bank-count must be in the range 1..4")
    if not 0 <= args.rtl_data_real_bank_count <= args.rtl_data_bank_count:
        parser.error("invalid RTL real-bank count")
    if args.rtl_data_bank_size * args.rtl_data_bank_count != args.rtl_data_size:
        parser.error(
            "--rtl-data-size must equal bank-size * bank-count")

if args.fake_veu_latency < 1:
    parser.error("--fake-veu-latency must be at least one cycle")

if args.veu_input_fifo_depth < 1:
    parser.error("--veu-input-fifo-depth must be at least one")

if args.veu_execute_latency < 1:
    parser.error("--veu-execute-latency must be at least one")

if args.veu_execute_ii < 1:
    parser.error("--veu-execute-ii must be at least one")

if args.veu_vsu_latency < 1:
    parser.error("--veu-vsu-latency must be at least one")

if (args.veu_startup_cycles < 0 or
        args.veu_lock_start_delay_cycles < 0 or
        args.veu_finish_cycles < 0):
    parser.error("VEU startup, lock-start-delay, and finish cycles must be non-negative")


system = System()

system.clk_domain = SrcClockDomain()
system.clk_domain.clock = args.clock_frequency
system.clk_domain.voltage_domain = VoltageDomain()

system.mem_mode = "timing"

if rtl_tb_mode:
    rtl_inst_base = 0x00000000
    if mikui_tb_mode:
        rtl_inst_size = 0x00004000
        rtl_data_base = 0x20010000
        if mikui_dma_tb_mode:
            rtl_data_size = 0x00018000
            system.mem_ranges = [
                AddrRange(start=rtl_inst_base, size=rtl_inst_size),
                AddrRange(start=0x20010000, size=0x00008000),
                AddrRange(start=0x20018000, size=0x00008000),
                AddrRange(start=0x20020000, size=0x00008000),
                AddrRange(start=0x40019C00, size=0x00000100),
                AddrRange(start=0x60000000, size=0x00001000),
                AddrRange(start=0x60001000, size=0x00001000),
            ]
        else:
            rtl_data_size = 0x00010000
            system.mem_ranges = [
                AddrRange(start=rtl_inst_base, size=rtl_inst_size),
                AddrRange(start=0x20010000, size=0x00010000),
                AddrRange(start=0x20020000, size=0x00010000),
            ]
    else:
        rtl_inst_size = 0x00040000
        rtl_data_base = args.rtl_data_base
        rtl_data_size = args.rtl_data_size
        system.mem_ranges = [
            AddrRange(start=rtl_inst_base, size=rtl_inst_size),
            AddrRange(start=rtl_data_base, size=rtl_data_size),
        ]
    pipeline_program_file = args.program_file
    pipeline_elf_file = ""
    pipeline_text_base = rtl_inst_base
    pipeline_preloaded_program = bool(args.imem_image)
    # PipelineMiniCPU replaces this placeholder with the raw file's byte size.
    pipeline_preloaded_program_size = 1 if args.imem_image else 0
    pipeline_dmem_hex_file = args.dmem_hex
    pipeline_dmem_base = rtl_data_base
    dmem_image_file = args.dmem_image
elif args.mem_system == "spirit-like":
    imem_base = parse_addr(args.imem_base)
    spirit_local_range = AddrRange(start=imem_base, size=args.imem_size)
    system.mem_ranges = [spirit_local_range]
    pipeline_program_file = args.program_file
    pipeline_elf_file = ""
    pipeline_text_base = imem_base
    pipeline_preloaded_program = bool(args.imem_image)
    pipeline_preloaded_program_size = toMemorySize(args.imem_size) if args.imem_image else 0
    pipeline_dmem_hex_file = args.dmem_hex
    pipeline_dmem_base = imem_base
    if not args.dmem_hex:
        dmem_image_file = args.dmem_image
    else:
        dmem_image_file = ""
else:
    system.mem_ranges = [AddrRange(start=0x80000000, size=args.mem_size)]
    pipeline_program_file = args.program_file
    pipeline_elf_file = args.binary
    pipeline_text_base = 0x80000000
    pipeline_preloaded_program = False
    pipeline_preloaded_program_size = 0
    pipeline_dmem_hex_file = ""
    pipeline_dmem_base = 0
    dmem_image_file = args.dmem_image

system.pipeline = PipelineMiniCPU(
    max_cycles=max_cycles,
    reset_cycles=reset_cycles,
    fake_veu_latency=args.fake_veu_latency,
    fake_veu_response_data=args.fake_veu_response_data,
    irq_external=args.irq_external,
    irq_software=args.irq_software,
    irq_timer=args.irq_timer,
    debug_halt=args.debug_halt,
    debug_halt_on_reset=args.debug_halt_on_reset,
    debug_resume=args.debug_resume,
    debug_data0=args.debug_data0,
    debug_instr=args.debug_instr,
    debug_instr_valid=args.debug_instr_valid,
    cycle_trace_file=args.cycle_trace,
    cycle_trace_compact=args.cycle_trace_compact,
    ebreak_terminates=args.terminate_on_ebreak,
    veu_model=args.veu_model,
    sau_model=args.sau_model,
    veu_input_fifo_depth=args.veu_input_fifo_depth,
    veu_execute_latency=args.veu_execute_latency,
    veu_execute_ii=args.veu_execute_ii,
    veu_vsu_latency=args.veu_vsu_latency,
    veu_timing_profile=args.veu_timing_profile,
    veu_terminal_behavior=args.veu_terminal_behavior,
    veu_cycle_trace=args.veu_cycle_trace,
    veu_startup_cycles=args.veu_startup_cycles,
    veu_lock_start_delay_cycles=args.veu_lock_start_delay_cycles,
    veu_finish_cycles=args.veu_finish_cycles,
    program_file=pipeline_program_file,
    elf_file=pipeline_elf_file,
    preloaded_program=pipeline_preloaded_program,
    preloaded_program_size=pipeline_preloaded_program_size,
    text_base=pipeline_text_base,
    dmem_base=pipeline_dmem_base,
    dmem_hex_file=pipeline_dmem_hex_file,
    icache_enabled=args.icache_enabled,
    icache_size=args.icache_size,
    icache_line_size=args.icache_line_size,
    frontend_burst_bytes=args.frontend_burst_bytes,
    instr_fifo_depth=args.instr_fifo_depth,
    tb_memory_enabled=rtl_tb_mode,
    tb_memory_kind=(
        "npu-lpnpu-mikui-dma" if mikui_dma_tb_mode else
        ("npu-lpnpu-mikui" if mikui_tb_mode else "dut-kui")),
    dma_pio_enabled=mikui_dma_tb_mode,
    tb_imem_image_file=args.imem_image if rtl_tb_mode else "",
    tb_dmem_image_file=args.dmem_image if rtl_tb_mode else "",
    tb_inst_base=0x00000000,
    tb_inst_size=rtl_inst_size if rtl_tb_mode else 0x00040000,
    tb_data_base=rtl_data_base if rtl_tb_mode else 0x20010000,
    tb_data_size=rtl_data_size if rtl_tb_mode else 0x00040000,
    tb_data_bank_size=(
        args.rtl_data_bank_size if dut_kui_tb_mode else 0x00010000),
    tb_data_bank_count=(
        args.rtl_data_bank_count if dut_kui_tb_mode else 1),
    tb_data_real_bank_count=(
        args.rtl_data_real_bank_count if dut_kui_tb_mode else 1),
)
system.pipeline.clk_domain = system.clk_domain

if args.mem_system == "ddr3":
    system.membus = SystemXBar()
    system.pipeline.inst_port = system.membus.cpu_side_ports
    system.pipeline.data_port = system.membus.cpu_side_ports
    system.pipeline.veu_port = system.membus.cpu_side_ports
    system.system_port = system.membus.cpu_side_ports

    system.mem_ctrl = MemCtrl()
    system.mem_ctrl.dram = DDR3_1600_8x8()
    system.mem_ctrl.dram.range = system.mem_ranges[0]
    system.mem_ctrl.port = system.membus.mem_side_ports

elif args.mem_system == "simple":
    system.membus = SystemXBar()
    system.pipeline.inst_port = system.membus.cpu_side_ports
    system.pipeline.data_port = system.membus.cpu_side_ports
    system.pipeline.veu_port = system.membus.cpu_side_ports
    system.system_port = system.membus.cpu_side_ports

    system.memory = SimpleMemory(
        range=system.mem_ranges[0],
        latency=args.mem_latency,
    )
    system.memory.port = system.membus.mem_side_ports

elif args.mem_system == "split":
    imem_latency = args.imem_latency if args.imem_latency else args.mem_latency
    dmem_latency = args.dmem_latency if args.dmem_latency else args.mem_latency
    dmem_image = args.dmem_image if args.dmem_image else args.binary

    system.ibus = SystemXBar()
    system.dbus = SystemXBar()

    system.pipeline.inst_port = system.ibus.cpu_side_ports
    system.pipeline.data_port = system.dbus.cpu_side_ports
    system.pipeline.veu_port = system.dbus.cpu_side_ports

    # Keep the system port on IMEM so PipelineMiniCPU::preloadElf() writes code
    # through system->physProxy into the globally visible instruction memory.
    system.system_port = system.ibus.cpu_side_ports

    system.imem = SimpleMemory(
        range=system.mem_ranges[0],
        latency=imem_latency,
        in_addr_map=True,
    )
    system.imem.port = system.ibus.mem_side_ports

    # DMEM intentionally uses the same architectural address range as IMEM,
    # but stays out of the global address map to avoid overlapping-memory checks.
    # It still responds to data_port accesses through its private DBus.
    system.dmem = SimpleMemory(
        range=system.mem_ranges[0],
        latency=dmem_latency,
        in_addr_map=False,
        image_file=dmem_image,
    )
    system.dmem.port = system.dbus.mem_side_ports

elif rtl_tb_mode:
    # Mandatory gem5 port connections. Runtime requests and image contents
    # bypass these stubs and use PipelineMiniCPU's shared cycle model.
    system.membus = NoncoherentXBar(
        frontend_latency=0,
        forward_latency=0,
        response_latency=0,
        width=args.frontend_burst_bytes,
    )
    system.pipeline.inst_port = system.membus.cpu_side_ports
    system.pipeline.data_port = system.membus.cpu_side_ports
    system.pipeline.veu_port = system.membus.cpu_side_ports
    system.system_port = system.membus.cpu_side_ports

    system.imem_stub = SimpleMemory(range=system.mem_ranges[0])
    system.imem_stub.port = system.membus.mem_side_ports
    if mikui_tb_mode:
        system.dmem_stub0 = SimpleMemory(range=system.mem_ranges[1])
        system.dmem_stub0.port = system.membus.mem_side_ports
        system.dmem_stub1 = SimpleMemory(range=system.mem_ranges[2])
        system.dmem_stub1.port = system.membus.mem_side_ports
        if mikui_dma_tb_mode:
            system.dmem_stub2 = SimpleMemory(range=system.mem_ranges[3])
            system.dmem_stub2.port = system.membus.mem_side_ports
            system.mikui_dma = MikuiDecompressDma(
                pio_addr=0x40019C00,
                pio_size=0x100,
                pio_latency=args.mem_latency,
                max_input_bytes=0x1000,
                max_output_bytes=0x1000,
            )
            system.mikui_dma.pio = system.membus.mem_side_ports
            system.mikui_dma.dma = system.membus.cpu_side_ports
            system.mikui_dma.irq = system.pipeline.dma_irq

            system.dma_source_sram = SimpleMemory(
                range=system.mem_ranges[5],
                latency=args.mem_latency,
                image_file=args.dma_input_image,
            )
            system.dma_source_sram.port = system.membus.mem_side_ports
            system.dma_destination_sram = SimpleMemory(
                range=system.mem_ranges[6],
                latency=args.mem_latency,
            )
            system.dma_destination_sram.port = system.membus.mem_side_ports
    else:
        system.dmem_stub = SimpleMemory(range=system.mem_ranges[1])
        system.dmem_stub.port = system.membus.mem_side_ports

elif args.mem_system == "spirit-like":
    imem_latency = args.imem_latency if args.imem_latency else args.mem_latency
    dmem_latency = args.dmem_latency if args.dmem_latency else args.mem_latency

    # The RTL testbench memory models add one registered response cycle and
    # no separate interconnect cycles. Keep the gem5 interconnect at zero
    # latency so the SimpleMemory latency is the complete external delay.
    system.ibus = NoncoherentXBar(
        frontend_latency=0,
        forward_latency=0,
        response_latency=0,
        width=args.frontend_burst_bytes,
    )
    system.dbus = NoncoherentXBar(
        frontend_latency=0,
        forward_latency=0,
        response_latency=0,
        width=args.frontend_burst_bytes,
    )

    system.pipeline.inst_port = system.ibus.cpu_side_ports
    system.pipeline.data_port = system.dbus.cpu_side_ports
    system.pipeline.veu_port = system.dbus.cpu_side_ports

    # Runtime access remains strictly split: IBUS reaches only IMEM and DBUS
    # reaches only DMEM. IMEM and DMEM intentionally share the same local
    # architectural window, matching the current Spirit testbench style.
    # system_port is kept on IBUS for compatibility, but spirit-like mode
    # intentionally avoids ELF physProxy preload.
    system.system_port = system.ibus.cpu_side_ports

    system.imem = SimpleMemory(
        range=system.mem_ranges[0],
        latency=imem_latency,
        in_addr_map=True,
        image_file=args.imem_image,
    )
    system.imem.port = system.ibus.mem_side_ports

    system.dmem = SimpleMemory(
        range=system.mem_ranges[0],
        latency=dmem_latency,
        in_addr_map=False,
        image_file=dmem_image_file,
    )
    system.dmem.port = system.dbus.mem_side_ports

root = Root(full_system=False, system=system)

m5.instantiate()

print("Beginning PipelineMiniCPU simulation!")
print("Clock frequency: {}".format(args.clock_frequency))
print("Reset cycles: {}".format(reset_cycles))
print("Timeout cycles: {} ({})".format(
    max_cycles,
    "total clock edges including reset" if rtl_tb_mode else "active CPU cycles",
))
print("VEU model: {}".format(args.veu_model))
print("SAU model: {}".format(args.sau_model))
print("FakeVEU latency: {} cycles".format(args.fake_veu_latency))
print("FakeVEU response data: {:#x}".format(args.fake_veu_response_data))
if args.veu_model == "timing":
    print("TimingVEU FIFO depth: {}".format(args.veu_input_fifo_depth))
    print("TimingVEU execute latency: {} cycles".format(args.veu_execute_latency))
    print("TimingVEU execute II: {} cycles".format(args.veu_execute_ii))
    print("TimingVEU VSU latency: {} cycles".format(args.veu_vsu_latency))
    print("TimingVEU profile: {}".format(args.veu_timing_profile or "<default>"))
    print("TimingVEU terminal behavior: {}".format(
        args.veu_terminal_behavior or "<disabled>"))
    print("TimingVEU cycle trace: {}".format(args.veu_cycle_trace or "<disabled>"))
    print("TimingVEU startup cycles: {}".format(args.veu_startup_cycles))
    print("TimingVEU lock start delay: {}".format(
        args.veu_lock_start_delay_cycles))
    print("TimingVEU finish cycles: {}".format(args.veu_finish_cycles))
print("Memory system: {}".format(args.mem_system))
if args.mem_system == "simple":
    print("Memory latency: {}".format(args.mem_latency))
elif args.mem_system == "split":
    print("IMEM latency: {}".format(imem_latency))
    print("DMEM latency: {}".format(dmem_latency))
    print("DMEM image: {}".format(dmem_image if dmem_image else "<none>"))
elif args.mem_system == "spirit-like":
    print("IMEM range: {}".format(system.mem_ranges[0]))
    print("DMEM range: {}".format(system.dmem.range))
    print("IMEM latency: {}".format(imem_latency))
    print("DMEM latency: {}".format(dmem_latency))
    if args.imem_image:
        print("IMEM image: {}".format(args.imem_image))
    elif pipeline_program_file:
        print("IMEM hex: {}".format(pipeline_program_file))
    else:
        print("IMEM image: <internal program>")
    if args.dmem_hex:
        print("DMEM hex: {} (loaded by CPU via dataPort)".format(args.dmem_hex))
    else:
        print("DMEM image: {}".format(dmem_image_file if dmem_image_file else "<none>"))
elif dut_kui_tb_mode:
    print("RTL IMEM range: 0x{:08x}..0x{:08x}".format(
        rtl_inst_base, rtl_inst_base + rtl_inst_size - 1))
    print("RTL DMEM range: 0x{:08x}..0x{:08x}".format(
        rtl_data_base, rtl_data_base + rtl_data_size - 1))
    print("RTL response timing: IBus/DBus external=N+1 core-visible=N+2")
    print("Shared arbitration: VEU lock blocks DBus; IBus remains independent")
elif mikui_tb_mode:
    print("RTL IMEM range: 0x00000000..0x00003fff")
    if mikui_dma_tb_mode:
        print("RTL stack SRAM: 0x20010000..0x20017fff")
        print("RTL ping SRAM:  0x20018000..0x2001ffff")
        print("RTL pong SRAM:  0x20020000..0x20027fff")
        print("RTL crossbar: crossbar_mi_full, B/C/D split = 4/8/12")
    else:
        print("RTL SRAM bank 0: 0x20010000..0x2001ffff")
        print("RTL SRAM bank 1: 0x20020000..0x2002ffff")
    print("RTL datapath: 32-to-128 DBus, native 128-bit SAU/VEU masters")
    print("RTL interrupts: external/software/timer tied to zero")
    if mikui_dma_tb_mode:
        print("RTL DMA registers: 0x40019c00..0x40019cff")
        print("RTL DMA: independent Rice/zigzag/zero-skip decompressor, 32-bit AHB master, cause-6 IRQ")
print("Internal I-cache: {}".format("enabled" if args.icache_enabled else "disabled"))

exit_event = m5.simulate()
print("Exiting @ tick {} because {}".format(m5.curTick(), exit_event.getCause()))

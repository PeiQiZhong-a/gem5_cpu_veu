"""Mikui platform module registration and connection points.

Keep module selection independent of gem5 so it can be unit-tested without
loading a simulator.  The attach callbacks import gem5 objects only when the
selected platform is actually assembled.
"""

from dataclasses import dataclass
from typing import Callable, Optional, Tuple


MIKUI_MEMORY = "rtl-npu-lpnpu-mikui"
MIKUI_DMA_MEMORY = "rtl-npu-lpnpu-mikui-decompress-dma"


@dataclass(frozen=True)
class ModuleSpec:
    name: str
    supported_memory_systems: Tuple[str, ...]
    default_memory_systems: Tuple[str, ...]
    required_memory_systems: Tuple[str, ...]
    connections: Tuple[str, ...]
    before_cpu: Optional[Callable] = None
    after_cpu: Optional[Callable] = None
    after_memory: Optional[Callable] = None
    mmio_windows: Tuple[Tuple[int, int], ...] = ()


def _create_sau(system, args):
    from m5.objects import MikuiSau, SrcClockDomain, VoltageDomain

    system.sau_clk_domain = SrcClockDomain()
    system.sau_clk_domain.clock = (
        args.sau_clock_frequency or args.clock_frequency)
    system.sau_clk_domain.voltage_domain = VoltageDomain()
    system.mikui_sau = MikuiSau(
        clk_domain=system.sau_clk_domain,
        cycle_trace_file=args.sau_cycle_trace,
        output_trace_file=args.sau_output_trace,
    )


def _connect_sau(system, _args):
    system.pipeline.mikui_sau = system.mikui_sau


def _connect_decompress_dma(system, args):
    # The DMA topology allocates the DDR4 address range at index 5.  Check
    # this contract before constructing a partially connected platform.
    if len(system.mem_ranges) != 6:
        raise ValueError("Mikui decompression DMA requires six memory ranges")
    from m5.objects import (
        DDR4_2400_8x8, MemCtrl, MikuiDecompressDma, NoncoherentXBar,
    )
    system.mikui_dma = MikuiDecompressDma(
        pio_addr=0x40019C00,
        pio_size=0x100,
        pio_latency=args.mem_latency,
        max_input_bytes=0x1000,
        max_output_bytes=0x1000,
    )
    system.mikui_dma.pio = system.membus.mem_side_ports
    system.mikui_dma.irq = system.pipeline.dma_irq

    # This private 32-bit fabric is not the CPU's 128-bit VEU/SAU path.
    system.dma_bus = NoncoherentXBar(
        frontend_latency=0,
        forward_latency=0,
        response_latency=0,
        width=4,
    )
    system.mikui_dma.dma = system.dma_bus.cpu_side_ports
    system.pipeline.dma_sram_port = system.dma_bus.mem_side_ports

    system.dma_ddr4_ctrl = MemCtrl()
    system.dma_ddr4_ctrl.dram = DDR4_2400_8x8(
        range=system.mem_ranges[5],
        image_file=args.dma_input_image,
    )
    system.dma_ddr4_ctrl.port = system.dma_bus.mem_side_ports


MODULES = (
    ModuleSpec(
        name="sau",
        supported_memory_systems=(MIKUI_MEMORY, MIKUI_DMA_MEMORY),
        default_memory_systems=(MIKUI_MEMORY, MIKUI_DMA_MEMORY),
        required_memory_systems=(),
        connections=(
            "pipeline.mikui_sau -> mikui_sau (HC and 128-bit SRAM endpoint)",
        ),
        before_cpu=_create_sau,
        after_cpu=_connect_sau,
    ),
    ModuleSpec(
        name="decompress_dma",
        supported_memory_systems=(MIKUI_DMA_MEMORY,),
        default_memory_systems=(MIKUI_DMA_MEMORY,),
        required_memory_systems=(MIKUI_DMA_MEMORY,),
        connections=(
            "mikui_dma.pio -> membus.mem_side_ports",
            "mikui_dma.irq -> pipeline.dma_irq",
            "mikui_dma.dma -> dma_bus.cpu_side_ports",
            "pipeline.dma_sram_port -> dma_bus.mem_side_ports",
            "dma_ddr4_ctrl.port -> dma_bus.mem_side_ports",
        ),
        after_memory=_connect_decompress_dma,
        mmio_windows=((0x40019C00, 0x100),),
    ),
)


def module_names():
    return tuple(spec.name for spec in MODULES)


def resolve_modules(memory_system, enabled=(), disabled=()):
    """Return selected modules in registration order without changing defaults."""
    known = set(module_names())
    enabled = set(enabled)
    disabled = set(disabled)
    unknown = (enabled | disabled) - known
    if unknown:
        raise ValueError("unknown Mikui module(s): " + ", ".join(sorted(unknown)))
    conflict = enabled & disabled
    if conflict:
        raise ValueError("Mikui module both enabled and disabled: " +
                         ", ".join(sorted(conflict)))

    selected = []
    for spec in MODULES:
        if spec.name in enabled and memory_system not in spec.supported_memory_systems:
            raise ValueError(
                "Mikui module {} is unavailable with {}".format(
                    spec.name, memory_system))
        if spec.name in disabled and memory_system in spec.required_memory_systems:
            raise ValueError(
                "Mikui module {} is required by {}".format(
                    spec.name, memory_system))
        if ((memory_system in spec.default_memory_systems or
             spec.name in enabled) and spec.name not in disabled):
            selected.append(spec)
    windows = []
    for spec in selected:
        for base, size in spec.mmio_windows:
            if base < 0 or size <= 0:
                raise ValueError("invalid MMIO window in Mikui module " + spec.name)
            end = base + size
            for other_name, other_base, other_end in windows:
                if base < other_end and other_base < end:
                    raise ValueError(
                        "Mikui module MMIO overlap: {} and {}".format(
                            other_name, spec.name))
            windows.append((spec.name, base, end))
    return tuple(selected)


def attach_modules(modules, stage, system, args):
    if stage not in ("before_cpu", "after_cpu", "after_memory"):
        raise ValueError("unknown Mikui module attachment stage: " + stage)
    for spec in modules:
        callback = getattr(spec, stage)
        if callback is not None:
            callback(system, args)

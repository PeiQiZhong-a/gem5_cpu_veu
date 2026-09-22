"""Module-selection and port-wiring tests; no gem5 binary is required."""

import sys
import types
import unittest
from types import SimpleNamespace
from unittest.mock import patch

from mikui_modules import (
    MIKUI_DMA_MEMORY, MIKUI_MEMORY, MODULES, ModuleSpec,
    attach_modules, module_names, resolve_modules,
)


class MikuiModuleSelectionTest(unittest.TestCase):
    def test_existing_platform_defaults_are_unchanged(self):
        self.assertEqual(resolve_modules("simple"), ())
        self.assertEqual(
            tuple(spec.name for spec in resolve_modules(MIKUI_MEMORY)),
            ("sau",),
        )
        self.assertEqual(
            tuple(spec.name for spec in resolve_modules(MIKUI_DMA_MEMORY)),
            ("sau", "decompress_dma"),
        )

    def test_optional_sau_can_be_disabled(self):
        self.assertEqual(
            tuple(spec.name for spec in resolve_modules(
                MIKUI_DMA_MEMORY, disabled=("sau",))),
            ("decompress_dma",),
        )

    def test_required_dma_cannot_be_disabled(self):
        with self.assertRaisesRegex(ValueError, "required"):
            resolve_modules(MIKUI_DMA_MEMORY, disabled=("decompress_dma",))

    def test_incompatible_unknown_and_conflicting_choices_fail(self):
        with self.assertRaisesRegex(ValueError, "unavailable"):
            resolve_modules(MIKUI_MEMORY, enabled=("decompress_dma",))
        with self.assertRaisesRegex(ValueError, "unknown"):
            resolve_modules(MIKUI_MEMORY, enabled=("typo",))
        with self.assertRaisesRegex(ValueError, "both enabled and disabled"):
            resolve_modules(MIKUI_MEMORY, enabled=("sau",), disabled=("sau",))

    def test_every_registered_module_declares_connections_and_a_hook(self):
        self.assertEqual(len(module_names()), len(set(module_names())))
        for spec in MODULES:
            self.assertTrue(spec.connections, spec.name)
            self.assertTrue(
                spec.before_cpu or spec.after_cpu or spec.after_memory,
                spec.name,
            )

    def test_module_mmio_windows_must_not_overlap(self):
        colliding = ModuleSpec(
            name="collision", supported_memory_systems=(MIKUI_DMA_MEMORY,),
            default_memory_systems=(MIKUI_DMA_MEMORY,),
            required_memory_systems=(), connections=("test.port",),
            mmio_windows=((0x40019C80, 0x40),),
        )
        with patch("mikui_modules.MODULES", MODULES + (colliding,)):
            with self.assertRaisesRegex(ValueError, "MMIO overlap"):
                resolve_modules(MIKUI_DMA_MEMORY)

    def test_attach_runs_only_the_requested_stage_in_registration_order(self):
        calls = []
        modules = (
            ModuleSpec("first", (), (), (), ("port",),
                       before_cpu=lambda *_: calls.append("first")),
            ModuleSpec("second", (), (), (), ("port",),
                       before_cpu=lambda *_: calls.append("second")),
        )
        attach_modules(modules, "before_cpu", object(), object())
        attach_modules(modules, "after_memory", object(), object())
        self.assertEqual(calls, ["first", "second"])
        with self.assertRaisesRegex(ValueError, "attachment stage"):
            attach_modules(modules, "invalid", object(), object())


class MikuiModuleWiringTest(unittest.TestCase):
    @staticmethod
    def fake_gem5_objects():
        objects = types.ModuleType("m5.objects")
        objects.SrcClockDomain = lambda: SimpleNamespace()
        objects.VoltageDomain = lambda: object()
        objects.MikuiSau = lambda **kwargs: SimpleNamespace(**kwargs)
        objects.MikuiDecompressDma = lambda **kwargs: SimpleNamespace(**kwargs)
        objects.NoncoherentXBar = lambda **kwargs: SimpleNamespace(
            cpu_side_ports=object(), mem_side_ports=object(), **kwargs)
        objects.MemCtrl = lambda: SimpleNamespace()
        objects.DDR4_2400_8x8 = lambda **kwargs: SimpleNamespace(**kwargs)
        return {"m5": types.ModuleType("m5"), "m5.objects": objects}

    def test_sau_endpoint_is_created_and_connected(self):
        system = SimpleNamespace(pipeline=SimpleNamespace())
        args = SimpleNamespace(
            sau_clock_frequency="", clock_frequency="100MHz",
            sau_cycle_trace="sau.csv", sau_output_trace="output.csv",
        )
        with patch.dict(sys.modules, self.fake_gem5_objects()):
            modules = resolve_modules(MIKUI_MEMORY)
            attach_modules(modules, "before_cpu", system, args)
            attach_modules(modules, "after_cpu", system, args)
        self.assertIs(system.pipeline.mikui_sau, system.mikui_sau)
        self.assertEqual(system.sau_clk_domain.clock, "100MHz")
        self.assertEqual(system.mikui_sau.cycle_trace_file, "sau.csv")

    def test_dma_pio_master_irq_and_ddr_ports_are_connected(self):
        system = SimpleNamespace(
            mem_ranges=list(range(6)),
            membus=SimpleNamespace(mem_side_ports=object()),
            pipeline=SimpleNamespace(dma_irq=object()),
        )
        args = SimpleNamespace(mem_latency="10ns", dma_input_image="data.bin")
        with patch.dict(sys.modules, self.fake_gem5_objects()):
            attach_modules(resolve_modules(MIKUI_DMA_MEMORY),
                           "after_memory", system, args)
        self.assertIs(system.mikui_dma.pio, system.membus.mem_side_ports)
        self.assertIs(system.mikui_dma.irq, system.pipeline.dma_irq)
        self.assertIs(system.mikui_dma.dma, system.dma_bus.cpu_side_ports)
        self.assertIs(system.pipeline.dma_sram_port,
                      system.dma_bus.mem_side_ports)
        self.assertIs(system.dma_ddr4_ctrl.port,
                      system.dma_bus.mem_side_ports)
        self.assertEqual(system.dma_ddr4_ctrl.dram.range, 5)

    def test_dma_rejects_wrong_memory_topology(self):
        system = SimpleNamespace(mem_ranges=[])
        with self.assertRaisesRegex(ValueError, "six memory ranges"):
            attach_modules(resolve_modules(MIKUI_DMA_MEMORY),
                           "after_memory", system, SimpleNamespace())


if __name__ == "__main__":
    unittest.main()

from m5.params import *
from m5.objects.ClockedObject import ClockedObject


class MikuiSau(ClockedObject):
    type = "MikuiSau"
    cxx_header = "sau_mikui/mikui_sau.hh"
    cxx_class = "gem5::sau_mikui::MikuiSau"

    rows = Param.UInt32(16, "Fixed Mikui systolic-array rows")
    cols = Param.UInt32(16, "Fixed Mikui systolic-array columns")
    sram_delay_cycles = Param.UInt32(1, "Frozen RTL SRAM response delay")
    strict_timing = Param.Bool(True, "Count and reject timing-contract errors")
    sau_clk_en = Param.Bool(True, "Enable SAU clock edges")
    cycle_trace_file = Param.String("", "Optional SAU cycle CSV trace")
    trace_internal_pe = Param.Bool(False, "Reserved opt-in PE debug trace")

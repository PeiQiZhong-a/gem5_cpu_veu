from m5.objects.Device import DmaDevice
from m5.params import *


class MikuiDecompressDma(DmaDevice):
    type = "MikuiDecompressDma"
    cxx_class = "gem5::MikuiDecompressDma"
    cxx_header = "brs/dma/mikui_decompress_dma.hh"

    pio_addr = Param.Addr(0x40019C00, "AHB slave register-window base")
    pio_size = Param.Addr(0x100, "AHB slave register-window size")
    pio_latency = Param.Latency("10ns", "Register access latency")
    max_input_bytes = Param.UInt32(
        0x1000, "Maximum compressed input transfer size")
    max_output_bytes = Param.UInt32(
        0x1000, "Maximum decompressed output transfer size")
    irq = IntSourcePin("DMA completion/error interrupt output")

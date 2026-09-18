// Complete SAU top-level with the 16-bank scratchpad attached.
//
// The compute datapath keeps its original configuration and status interface.
// The only additional interface is a per-bank DMA write port used to preload
// activation/weight/bias/output storage.  DMA writes are accepted while the
// compute engine is idle; compute writeback owns the scratchpad while busy.
module sau_compute_scratchpad_top #(
    parameter int BLOCK_SIZE = 16,
    parameter int ELEM_W = 8,
    parameter int ACC_W = 24,
    parameter int SP_BANKS = BLOCK_SIZE,
    parameter int SP_BANK_ENTRIES = 4096,
    parameter int MAX_C = 64,
    parameter int MAX_SEG_C = 16,
    parameter int MAX_KERNEL = 5,
    parameter int MAX_K = 250,
    parameter int K_W = $clog2(MAX_K + 1),
    parameter int GROUP_W = 13,
    parameter int SP_BANK_BITS = $clog2(SP_BANKS),
    parameter int SP_ROW_BITS = $clog2(SP_BANK_ENTRIES),
    parameter int SP_ADDR_BITS = SP_BANK_BITS + SP_ROW_BITS
) (
    input logic clk,
    input logic rst_n,
    input logic cfg_valid,
    input logic cfg_dw_mode,
    input logic cfg_mline_mode,
    input logic [4:0] cfg_rows_per_group,
    input logic [GROUP_W-1:0] cfg_groups_per_n,
    input logic [SP_ADDR_BITS-1:0] cfg_activation_base,
    input logic [SP_ADDR_BITS-1:0] cfg_weight_base,
    input logic [SP_ADDR_BITS-1:0] cfg_bias_base,
    input logic [SP_ADDR_BITS-1:0] cfg_output_base,
    input logic [15:0] cfg_n, cfg_c, cfg_h, cfg_w, cfg_out_h, cfg_out_w,
    input logic [3:0] cfg_kernel_h, cfg_kernel_w,
    input logic [3:0] cfg_stride_h, cfg_stride_w,
    input logic [3:0] cfg_dilation_h, cfg_dilation_w,
    input logic [15:0] cfg_pad_top, cfg_pad_left,
    input logic [31:0] cfg_kernel_pattern,
    input logic [15:0] cfg_oc,
    input logic [4:0] cfg_cutbit,
    input logic start,
    output logic busy, done, cfg_error, protocol_error,

    // DMA writes use the same logical bank/row byte addressing as the
    // original scratchpad model.  The wrapper hides the 2048x16 byte packing.
    input logic [SP_BANKS-1:0] dma_wr_valid,
    output logic [SP_BANKS-1:0] dma_wr_ready,
    input logic [SP_BANKS-1:0][SP_ROW_BITS-1:0] dma_wr_addr,
    input logic [SP_BANKS-1:0][ELEM_W-1:0] dma_wr_data
);
    logic [SP_BANKS-1:0] req_valid;
    logic [SP_BANKS-1:0][SP_ROW_BITS-1:0] req_addr;
    logic [SP_BANKS-1:0] resp_valid;
    logic [SP_BANKS-1:0][ELEM_W-1:0] resp_data;
    logic [SP_BANKS-1:0] compute_wr_valid;
    logic [SP_BANKS-1:0] compute_wr_ready;
    logic [SP_BANKS-1:0][SP_ROW_BITS-1:0] compute_wr_addr;
    logic [SP_BANKS-1:0][ELEM_W-1:0] compute_wr_data;
    logic [SP_BANKS-1:0] sp_wr_valid;
    logic [SP_BANKS-1:0] sp_wr_allow;
    logic [SP_BANKS-1:0][SP_ROW_BITS-1:0] sp_wr_addr;
    logic [SP_BANKS-1:0][ELEM_W-1:0] sp_wr_data;

    // DMA is deliberately idle-gated.  A compute writeback cycle has priority
    // over DMA, while an idle engine accepts one byte per asserted bank.
    assign dma_wr_ready = {SP_BANKS{!busy}} & ~compute_wr_valid;
    // A DMA request is ignored while compute is busy, so it must not stall a
    // writeback transaction that is already using the scratchpad port.
    assign compute_wr_ready = {SP_BANKS{busy || !(|dma_wr_valid)}};
    assign sp_wr_valid = compute_wr_valid | (dma_wr_valid & dma_wr_ready);
    assign sp_wr_allow = compute_wr_valid ? compute_wr_ready : dma_wr_ready;
    assign sp_wr_addr = compute_wr_valid ? compute_wr_addr : dma_wr_addr;
    assign sp_wr_data = compute_wr_valid ? compute_wr_data : dma_wr_data;

    sau_compute_top #(
        .BLOCK_SIZE(BLOCK_SIZE), .ELEM_W(ELEM_W), .ACC_W(ACC_W),
        .SP_BANKS(SP_BANKS), .SP_BANK_ENTRIES(SP_BANK_ENTRIES),
        .MAX_C(MAX_C), .MAX_SEG_C(MAX_SEG_C), .MAX_KERNEL(MAX_KERNEL),
        .MAX_K(MAX_K), .K_W(K_W), .GROUP_W(GROUP_W),
        .SP_BANK_BITS(SP_BANK_BITS),
        .SP_ROW_BITS(SP_ROW_BITS), .SP_ADDR_BITS(SP_ADDR_BITS)
    ) u_compute (
        .clk, .rst_n, .cfg_valid, .cfg_dw_mode,
        .cfg_mline_mode, .cfg_rows_per_group, .cfg_groups_per_n,
        .cfg_activation_base, .cfg_weight_base, .cfg_bias_base,
        .cfg_output_base, .cfg_n, .cfg_c, .cfg_h, .cfg_w,
        .cfg_out_h, .cfg_out_w, .cfg_kernel_h, .cfg_kernel_w,
        .cfg_stride_h, .cfg_stride_w, .cfg_dilation_h, .cfg_dilation_w,
        .cfg_pad_top, .cfg_pad_left, .cfg_kernel_pattern, .cfg_oc,
        .cfg_cutbit, .start, .busy, .done, .cfg_error, .protocol_error,
        .sram_req_valid(req_valid), .sram_req_addr(req_addr),
        .sram_resp_valid(resp_valid), .sram_resp_data(resp_data),
        .spad_wr_valid(compute_wr_valid), .spad_wr_ready(compute_wr_ready),
        .spad_wr_addr(compute_wr_addr), .spad_wr_data(compute_wr_data)
    );

    sau_scratchpad_16bank #(
        .SP_BANKS(SP_BANKS), .SP_BANK_ENTRIES(SP_BANK_ENTRIES),
        .ELEM_W(ELEM_W), .SP_ROW_BITS(SP_ROW_BITS)
    ) u_scratchpad (
        .clk, .rst_n, .req_valid, .req_addr,
        .resp_valid, .resp_data,
        .wr_valid(sp_wr_valid), .wr_addr(sp_wr_addr), .wr_data(sp_wr_data),
        .wr_allow(sp_wr_allow), .wr_ready()
    );
endmodule

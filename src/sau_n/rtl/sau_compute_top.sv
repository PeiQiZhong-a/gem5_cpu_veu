// C=1..64 end-to-end SAU compute path with 16-channel retain segments.
//
// The feeder loads one B tile and one bias tile, then streams complete Im2Col
// groups.  K0 is held while this top issues the corresponding array command.
// Raw accumulator columns are accepted by the writeback only after all feeder
// reads and fixed-latency responses have quiesced, keeping LOAD_B, LOAD_BIAS,
// RUN_A and WRITEBACK mutually exclusive at the shared Scratchpad boundary.
module sau_compute_top #(
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
    input  logic clk,
    input  logic rst_n,

    input  logic cfg_valid,
    // DW: OC=C, multiplier=1, compact [C][KH][KW] weights. Configure at global idle.
    input  logic cfg_dw_mode,
    // Software-predecoded spatial traversal. Configure at global idle.
    input  logic cfg_mline_mode,
    input  logic [4:0] cfg_rows_per_group,
    input  logic [GROUP_W-1:0] cfg_groups_per_n,
    input  logic [SP_ADDR_BITS-1:0] cfg_activation_base,
    input  logic [SP_ADDR_BITS-1:0] cfg_weight_base,
    input  logic [SP_ADDR_BITS-1:0] cfg_bias_base,
    input  logic [SP_ADDR_BITS-1:0] cfg_output_base,
    input  logic [15:0] cfg_n,
    input  logic [15:0] cfg_c,
    input  logic [15:0] cfg_h,
    input  logic [15:0] cfg_w,
    input  logic [15:0] cfg_out_h,
    input  logic [15:0] cfg_out_w,
    input  logic [3:0] cfg_kernel_h,
    input  logic [3:0] cfg_kernel_w,
    input  logic [3:0] cfg_stride_h,
    input  logic [3:0] cfg_stride_w,
    input  logic [3:0] cfg_dilation_h,
    input  logic [3:0] cfg_dilation_w,
    input  logic [15:0] cfg_pad_top,
    input  logic [15:0] cfg_pad_left,
    input  logic [31:0] cfg_kernel_pattern,
    input  logic [15:0] cfg_oc,
    input  logic [4:0] cfg_cutbit,

    input  logic start,
    output logic busy,
    output logic done,
    // Compatibility-only legacy status. Configuration legality is software-owned.
    output logic cfg_error,
    output logic protocol_error,

    output logic [SP_BANKS-1:0] sram_req_valid,
    output logic [SP_BANKS-1:0][SP_ROW_BITS-1:0] sram_req_addr,
    input  logic [SP_BANKS-1:0] sram_resp_valid,
    input  var logic [SP_BANKS-1:0][ELEM_W-1:0] sram_resp_data,

    output logic [SP_BANKS-1:0] spad_wr_valid,
    input  logic [SP_BANKS-1:0] spad_wr_ready,
    output logic [SP_BANKS-1:0][SP_ROW_BITS-1:0] spad_wr_addr,
    output logic [SP_BANKS-1:0][ELEM_W-1:0] spad_wr_data
);

    logic config_captured_q;
    logic start_without_config_pulse;


    logic feeder_start, feeder_busy, feeder_done, feeder_error;
    logic tile_advance_ready;
    logic [SP_BANKS-1:0] feeder_req_valid;
    logic [SP_BANKS-1:0][SP_ROW_BITS-1:0] feeder_req_addr;
    logic feeder_pe_valid, feeder_pe_ready;
    logic [BLOCK_SIZE*ELEM_W-1:0] feeder_a_data, feeder_b_data;
    logic [BLOCK_SIZE-1:0] feeder_a_mask, feeder_b_mask;
    logic [K_W-1:0] feeder_k_index;
    logic feeder_last_k;
    logic [15:0] feeder_oc_tile;
    logic [15:0] feeder_c_tile;
    logic feeder_last_c_tile;
    logic [K_W-1:0] feeder_k_count;
    logic feeder_last_group, feeder_last_oc_tile;
    logic [BLOCK_SIZE-1:0] feeder_row_mask;
    logic [15:0] feeder_n_index;
    logic [GROUP_W-1:0] feeder_group_index;
    logic [BLOCK_SIZE*32-1:0] feeder_spatial_index;
    logic [BLOCK_SIZE*16-1:0] feeder_bias_data;
    logic [BLOCK_SIZE-1:0] feeder_bias_mask;
    logic [15:0] feeder_bias_oc_tile;

    logic array_cmd_valid, array_cmd_ready;
    logic array_input_valid, array_input_ready;
    logic array_result_valid, array_result_ready;
    logic [BLOCK_SIZE*ACC_W-1:0] array_result_acc;
    logic [3:0] array_result_oc_lane;
    logic [BLOCK_SIZE-1:0] array_result_row_mask;
    logic [BLOCK_SIZE*32-1:0] array_result_spatial_index;
    logic [15:0] array_result_n_index;
    logic [GROUP_W-1:0] array_result_group_index;
    logic [15:0] array_result_oc_tile;
    logic array_result_last_column, array_result_last_command;
    logic array_busy, array_protocol_error;

    logic wb_result_valid, wb_result_ready;
    logic wb_busy, wb_done, wb_protocol_error;

    logic [1:0] quiescent_count;
    logic read_quiescent;
    logic [15:0] cfg_tile_width;

    function automatic logic [15:0] channel_limit_for_kernel(
        input logic [3:0] kernel_h,
        input logic [3:0] kernel_w
    );
        integer area;
        integer limit;
        begin
            area = kernel_h * kernel_w;
            limit = (area > 0) ? (MAX_K / area) : 1;
            if (limit < 1)
                limit = 1;
            if (limit > BLOCK_SIZE)
                limit = BLOCK_SIZE;
            channel_limit_for_kernel = limit;
        end
    endfunction

    always_comb begin
        cfg_tile_width = cfg_dw_mode ?
                         channel_limit_for_kernel(cfg_kernel_h, cfg_kernel_w) :
                         BLOCK_SIZE;
    end

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            config_captured_q <= 1'b0;
            start_without_config_pulse <= 1'b0;
        end else begin
            start_without_config_pulse <= 1'b0;
            if (cfg_valid && !busy) begin
                config_captured_q <= 1'b1;
            end
            if (start && !busy && !config_captured_q)
                start_without_config_pulse <= 1'b1;
        end
    end

    assign feeder_start = start && !busy && config_captured_q;
    assign sram_req_valid = feeder_req_valid;
    assign sram_req_addr = feeder_req_addr;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            quiescent_count <= '0;
        end else if (feeder_req_valid != '0 || sram_resp_valid != '0) begin
            quiescent_count <= '0;
        end else if (quiescent_count != 2'd3) begin
            quiescent_count <= quiescent_count + 1'b1;
        end
    end
    assign read_quiescent = quiescent_count >= 2;

    assign array_cmd_valid = feeder_pe_valid && feeder_k_index == 0 &&
                             !wb_busy;
    assign array_input_valid = feeder_pe_valid;
    assign feeder_pe_ready = array_input_ready;

    assign wb_result_valid = array_result_valid && read_quiescent;
    assign array_result_ready = wb_result_ready && read_quiescent;
    assign tile_advance_ready = !array_busy && !wb_busy &&
                                !array_result_valid && read_quiescent;

    assign busy = feeder_busy || array_busy || wb_busy;
    assign done = wb_done || start_without_config_pulse ||
                  (feeder_done && feeder_error);
    assign protocol_error = array_protocol_error || wb_protocol_error;
    assign cfg_error = 1'b0;

    sau_data_feeder #(
        .BLOCK_SIZE(BLOCK_SIZE), .ELEM_W(ELEM_W), .SP_BANKS(SP_BANKS),
        .SP_BANK_ENTRIES(SP_BANK_ENTRIES), .MAX_C(MAX_C),
        .MAX_SEG_C(MAX_SEG_C),
        .MAX_KERNEL(MAX_KERNEL), .MAX_K(MAX_K), .GROUP_W(GROUP_W)
    ) u_feeder (
        .clk, .rst_n, .cfg_valid, .cfg_dw_mode,
        .cfg_mline_mode, .cfg_rows_per_group, .cfg_groups_per_n,
        .cfg_activation_base, .cfg_weight_base, .cfg_bias_base,
        .cfg_n, .cfg_c, .cfg_h, .cfg_w, .cfg_out_h, .cfg_out_w,
        .cfg_kernel_h, .cfg_kernel_w, .cfg_stride_h, .cfg_stride_w,
        .cfg_dilation_h, .cfg_dilation_w, .cfg_pad_top, .cfg_pad_left,
        .cfg_kernel_pattern, .cfg_oc, .start(feeder_start),
        .tile_advance_ready, .busy(feeder_busy), .done(feeder_done),
        .cfg_error(feeder_error), .sram_req_valid(feeder_req_valid),
        .sram_req_addr(feeder_req_addr), .sram_resp_valid,
        .sram_resp_data, .pe_valid(feeder_pe_valid),
        .pe_ready(feeder_pe_ready), .pe_a_data(feeder_a_data),
        .pe_a_mask(feeder_a_mask), .pe_b_data(feeder_b_data),
        .pe_b_mask(feeder_b_mask), .pe_k_index(feeder_k_index),
        .pe_last_k(feeder_last_k), .pe_oc_tile(feeder_oc_tile),
        .pe_c_tile(feeder_c_tile), .pe_last_c_tile(feeder_last_c_tile),
        .pe_k_count(feeder_k_count), .pe_last_group(feeder_last_group),
        .pe_last_oc_tile(feeder_last_oc_tile),
        .pe_row_mask(feeder_row_mask), .pe_n_index(feeder_n_index),
        .pe_group_index(feeder_group_index),
        .pe_spatial_index(feeder_spatial_index),
        .pe_bias_data(feeder_bias_data), .pe_bias_mask(feeder_bias_mask),
        .pe_bias_oc_tile(feeder_bias_oc_tile)
    );

    sau_array_16x16 #(
        .BLOCK_SIZE(BLOCK_SIZE), .ELEM_W(ELEM_W), .ACC_W(ACC_W),
        .MAX_K(MAX_K), .K_W(K_W), .N_W(16), .GROUP_W(GROUP_W),
        .SPATIAL_W(32), .OC_TILE_W(16), .C_TILE_W(16)
    ) u_array (
        .clk, .rst_n, .cmd_valid(array_cmd_valid), .cmd_ready(array_cmd_ready),
        .cmd_clear_acc(feeder_c_tile == 0),
        .cmd_finalize(feeder_last_c_tile),
        .cmd_k_count(feeder_k_count), .cmd_c_tile(feeder_c_tile),
        .cmd_last_c_tile(feeder_last_c_tile), .cmd_row_mask(feeder_row_mask),
        .cmd_col_mask(feeder_b_mask), .cmd_n_index(feeder_n_index),
        .cmd_group_index(feeder_group_index),
        .cmd_spatial_index(feeder_spatial_index),
        .cmd_oc_tile(feeder_oc_tile),
        .cmd_last_group(feeder_last_group),
        .cmd_last_oc_tile(feeder_last_oc_tile),
        .input_valid(array_input_valid), .input_ready(array_input_ready),
        .input_a_data(feeder_a_data), .input_a_mask(feeder_a_mask),
        .input_b_data(feeder_b_data), .input_b_mask(feeder_b_mask),
        .input_k_index(feeder_k_index), .input_last_k(feeder_last_k),
        .result_valid(array_result_valid), .result_ready(array_result_ready),
        .result_acc_data(array_result_acc),
        .result_oc_lane(array_result_oc_lane),
        .result_row_mask(array_result_row_mask),
        .result_spatial_index(array_result_spatial_index),
        .result_n_index(array_result_n_index),
        .result_group_index(array_result_group_index),
        .result_oc_tile(array_result_oc_tile),
        .result_last_column(array_result_last_column),
        .result_last_command(array_result_last_command),
        .busy(array_busy), .protocol_error(array_protocol_error)
    );

    sau_nchw_writeback #(
        .BLOCK_SIZE(BLOCK_SIZE), .ACC_W(ACC_W), .SP_BANKS(SP_BANKS),
        .SP_BANK_ENTRIES(SP_BANK_ENTRIES), .GROUP_W(GROUP_W)
    ) u_writeback (
        .clk, .rst_n, .cfg_valid, .cfg_output_base, .cfg_n, .cfg_oc,
        .cfg_tile_width,
        .cfg_out_h, .cfg_out_w, .cfg_cutbit, .cfg_error(),
        .result_valid(wb_result_valid), .result_ready(wb_result_ready),
        .result_acc_data(array_result_acc),
        .result_oc_lane(array_result_oc_lane),
        .result_row_mask(array_result_row_mask),
        .result_spatial_index(array_result_spatial_index),
        .result_n_index(array_result_n_index),
        .result_group_index(array_result_group_index),
        .result_oc_tile(array_result_oc_tile),
        .result_last_column(array_result_last_column),
        .result_last_command(array_result_last_command),
        .bias_data(feeder_bias_data), .bias_mask(feeder_bias_mask),
        .bias_oc_tile(feeder_bias_oc_tile),
        .spad_wr_valid, .spad_wr_ready, .spad_wr_addr, .spad_wr_data,
        .busy(wb_busy), .done(wb_done),
        .protocol_error(wb_protocol_error)
    );

    logic _unused_array_cmd_ready;
    assign _unused_array_cmd_ready = array_cmd_ready;
endmodule

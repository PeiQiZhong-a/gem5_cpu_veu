// Continuous-NCHW gather-style Im2Col reference RTL.
//
// The scratchpad stores one dense NCHW byte stream without padding between
// input rows, channels, or batches:
//
//   tensor_offset = (((n * C) + c) * H + h) * W + w
//   byte_address  = cfg_spad_base + tensor_offset
//   bank          = byte_address % SP_BANKS
//   row           = byte_address / SP_BANKS
//
// cfg_spad_base is a byte address and may start at any bank. SP_BANKS must be a
// power of two. Each int8 bank has an independent row address, so one output
// vector may gather bytes from different scratchpad rows in the same cycle.
// A read request is accepted at a rising edge and returns with resp_valid one
// cycle later. Each bank keeps at most one outstanding request.
//
// Scratchpad storage and output-vector packing are independent. For example,
// W=5 stores h3.w0 in row0.bank15, but an output group still uses lanes 0..14
// for three spatial rows and emits lane15 as data=0, mask=0.
//
// This is a readable design sketch, not drop-in Gemmini RTL.

module gemmini_im2col_chw_gather_readable #(
    parameter int BLOCK_SIZE      = 16,
    parameter int ELEM_W          = 8,
    parameter int SP_BANKS        = BLOCK_SIZE,
    parameter int SP_BANK_ENTRIES = 4096,
    parameter int FIFO_DEPTH      = 4,
    parameter int KERNEL_PATTERN_BITS = 32,
    parameter int SP_BANK_BITS    = $clog2(SP_BANKS),
    parameter int SP_ROW_BITS     = $clog2(SP_BANK_ENTRIES),
    parameter int SP_ADDR_BITS    = SP_BANK_BITS + SP_ROW_BITS,
    parameter int FIFO_PTR_W      = $clog2(FIFO_DEPTH),
    parameter int LANE_W          = $clog2(BLOCK_SIZE),
    parameter int GROUP_W         = 13,
    parameter int SPATIAL_W       = 32,
    parameter int MAX_SEG_C       = 16
) (
    input  logic                         clk,
    input  logic                         rst_n,

    input  logic                         cfg_valid,
    input  logic [SP_ADDR_BITS-1:0]      cfg_spad_base,
    input  logic [15:0]                  cfg_n,
    input  logic [15:0]                  cfg_c,
    // cfg_c is the dense tensor's total C stride. The segment fields select
    // a contiguous channel slice and keep the emitted local K index at zero.
    input  logic [15:0]                  cfg_c_base,
    input  logic [15:0]                  cfg_c_count,
    // Optional one-spatial-group command used by channel-tile retain.
    input  logic                         cfg_single_group_mode,
    input  logic [15:0]                  cfg_group_n,
    input  logic [GROUP_W-1:0]           cfg_group_index,
    input  logic [15:0]                  cfg_group_oh_base,
    input  logic [15:0]                  cfg_group_ow_base,
    input  logic [15:0]                  cfg_h,
    input  logic [15:0]                  cfg_w,
    input  logic [15:0]                  cfg_out_h,
    input  logic [15:0]                  cfg_out_w,
    input  logic                         cfg_mline_mode,
    input  logic [4:0]                   cfg_rows_per_group,
    input  logic [3:0]                   cfg_kernel_h,
    input  logic [3:0]                   cfg_kernel_w,
    input  logic [3:0]                   cfg_stride_h,
    input  logic [3:0]                   cfg_stride_w,
    input  logic [3:0]                   cfg_dilation_h,
    input  logic [3:0]                   cfg_dilation_w,
    input  logic [15:0]                  cfg_pad_top,
    input  logic [15:0]                  cfg_pad_left,

    // Normal conv: lanes can represent W positions for one channel/tap.
    // DW conv: this layout is also natural because each channel is independent.
    input  logic                         cfg_dw_mode,

    // One bit per kernel tap in row-major order. 32 bits cover every tap of
    // the supported 1x1, 3x3 and 5x5 kernels.
    input  logic [KERNEL_PATTERN_BITS-1:0] cfg_kernel_pattern,
    input  logic [GROUP_W-1:0]           cfg_groups_per_n,

    input  logic                         start,
    output logic                         busy,
    output logic                         done,
    // One-cycle error pulse for a start issued before configuration capture.
    output logic                         cfg_error,

    output logic [SP_BANKS-1:0]           sram_req_valid,
    output logic [SP_BANKS-1:0][SP_ROW_BITS-1:0] sram_req_addr,
    input  logic [SP_BANKS-1:0]           sram_resp_valid,
    input  var logic [SP_BANKS-1:0][ELEM_W-1:0] sram_resp_data,

    output logic                         feed_valid,
    input  logic                         feed_ready,
    output logic [BLOCK_SIZE*ELEM_W-1:0] feed_data,
    output logic [BLOCK_SIZE-1:0]         feed_mask,
    output logic [BLOCK_SIZE-1:0]         feed_row_mask,
    output logic [15:0]                   feed_n_index,
    output logic [GROUP_W-1:0]            feed_group_index,
    output logic [BLOCK_SIZE*SPATIAL_W-1:0] feed_spatial_index
);

    // Waveform-only summary of the deepest occupied pipeline stage. The data
    // path itself is controlled by the s1/s2/s3 valid bits below.
    typedef enum logic [2:0] {
        ST_IDLE,
        ST_ISSUE,
        ST_COLLECT,
        ST_PUSH,
        ST_DONE
    } state_e;

    typedef struct packed {
        logic valid;
        logic [SP_BANK_BITS-1:0] bank;
        logic [SP_ROW_BITS-1:0] row;
        logic [LANE_W-1:0] dst_lane;
    } lane_req_t;

    localparam int DESCRIPTOR_DEPTH = 2;
    localparam int DESCRIPTOR_COUNT_W = $clog2(DESCRIPTOR_DEPTH + 1);

    state_e state;

    logic [SP_ADDR_BITS-1:0] spad_base_q;
    logic [15:0] n_q, c_q, c_base_q, c_count_q, h_q, w_q, out_h_q, out_w_q;
    logic [3:0] kernel_h_q, kernel_w_q;
    logic [3:0] stride_h_q, stride_w_q;
    logic [3:0] dilation_h_q, dilation_w_q;
    logic [15:0] pad_top_q, pad_left_q;
    logic [KERNEL_PATTERN_BITS-1:0] kernel_pattern_q;
    logic single_group_mode_q;
    logic [15:0] group_n_q;
    logic [GROUP_W-1:0] group_config_index_q;

    // Cursor for the next vector entering the coordinate pipeline.
    logic [15:0] n_idx;
    logic [15:0] c_idx;
    logic [15:0] oh_idx;
    logic [15:0] ow_base;
    logic [GROUP_W-1:0] group_idx;
    logic [3:0]  kh_idx;
    logic [3:0]  kw_idx;

    logic mline_mode_q;
    logic [4:0] rows_per_group_q;
    logic [31:0] channel_byte_stride;
    logic [31:0] batch_byte_stride;
    logic        config_captured;
    logic [GROUP_W-1:0] groups_per_n;

    // Stage 0 captures the cursor state and cheap lane/output mapping. Coord
    // then registers the padding/valid/spatial result before the existing G0
    // boundary, removing that cone from the Stage-0-to-G0 path.
    localparam int COORD_TAP_W = $clog2(KERNEL_PATTERN_BITS);
    logic stage0_valid;
    logic stage0_in_valid;
    logic stage0_ready;
    logic stage0_fire;
    logic stage0_to_coord;
    logic [15:0] stage0_n_idx, stage0_c_idx;
    logic [GROUP_W-1:0] stage0_group_idx;
    logic [3:0] stage0_kh_idx, stage0_kw_idx;
    logic stage0_last;
    logic [BLOCK_SIZE*16-1:0] stage0_out_h;
    logic [BLOCK_SIZE*16-1:0] stage0_out_w;
    logic [BLOCK_SIZE-1:0] stage0_row_eligible;
    logic [COORD_TAP_W-1:0] stage0_tap_index;
    logic [BLOCK_SIZE*16-1:0] coord_stage0_out_h;
    logic [BLOCK_SIZE*16-1:0] coord_stage0_out_w;
    logic [BLOCK_SIZE-1:0] coord_stage0_row_eligible;
    logic [COORD_TAP_W-1:0] coord_stage0_tap_index;

    // Coord is the elastic intermediate token. It holds the expensive
    // geometry results; G0 performs only the final metadata combines.
    logic coord_valid;
    logic coord_ready;
    logic coord_to_g0;
    logic coord_last;
    logic [BLOCK_SIZE*SPATIAL_W-1:0] coord_spatial_base_q;
    logic [BLOCK_SIZE*16-1:0] coord_out_w_q;
    logic [BLOCK_SIZE-1:0] coord_tap_active_q;
    logic [BLOCK_SIZE-1:0] coord_boundary_valid_q;
    logic [BLOCK_SIZE-1:0] coord_row_eligible_q;
    logic [BLOCK_SIZE-1:0] coord_c_valid_q;
    logic [BLOCK_SIZE-1:0] coord_is_padding_q;
    logic [BLOCK_SIZE*16-1:0] coord_in_h_q;
    logic [BLOCK_SIZE*16-1:0] coord_in_w_q;
    logic [15:0] coord_n_index_q;
    logic [GROUP_W-1:0] coord_group_index_q;
    logic [15:0] coord_n_idx_q;
    logic [15:0] coord_c_idx_q;

    // G0: geometry/padding token.  This elastic register remains the stage-1
    // output boundary; A1 below derives bank/row from its held coordinates.
    logic g0_valid;
    logic g0_last;
    logic [BLOCK_SIZE-1:0] g0_req_valid;
    logic [BLOCK_SIZE-1:0] g0_zero;
    logic [BLOCK_SIZE-1:0] g0_row_mask;
    logic [BLOCK_SIZE*16-1:0] g0_in_h;
    logic [BLOCK_SIZE*16-1:0] g0_in_w;
    logic [BLOCK_SIZE*SPATIAL_W-1:0] g0_spatial_index;
    logic [15:0] g0_n_index;
    logic [GROUP_W-1:0] g0_group_index;
    logic [15:0] g0_n_idx;
    logic [15:0] g0_c_idx;
    logic g0_to_s1;
    logic g0_ready;
    logic g0_in_fire;
    logic s1_ready;

    // Stage 1: A1 address generation and descriptor holding.
    lane_req_t lane_req [BLOCK_SIZE];
    logic [BLOCK_SIZE-1:0] lane_zero;
    logic [BLOCK_SIZE-1:0] lane_row_mask;
    logic [BLOCK_SIZE*SPATIAL_W-1:0] lane_spatial_index;
    logic cursor_last;
    logic producer_active;
    logic start_accept;
    logic start_reject;
    logic cursor_step;
    logic s1_valid;
    logic s1_last;
    lane_req_t s1_req [BLOCK_SIZE];
    logic [BLOCK_SIZE-1:0] s1_zero;
    logic [BLOCK_SIZE-1:0] s1_row_mask;
    logic [15:0] s1_n_index;
    logic [GROUP_W-1:0] s1_group_index;
    logic [BLOCK_SIZE*SPATIAL_W-1:0] s1_spatial_index;

    logic s1_to_buffer;
    logic buffer_in_ready;
    logic buffer_out_valid;
    logic [DESCRIPTOR_COUNT_W-1:0] descriptor_occupancy;
    logic buffer_out_last;
    logic [BLOCK_SIZE-1:0] buffer_out_req_valid;
    logic [BLOCK_SIZE*SP_BANK_BITS-1:0] buffer_out_req_bank;
    logic [BLOCK_SIZE*SP_ROW_BITS-1:0] buffer_out_req_row;
    logic [BLOCK_SIZE-1:0] buffer_out_zero;
    logic [BLOCK_SIZE-1:0] buffer_out_row_mask;
    logic [15:0] buffer_out_n_index;
    logic [GROUP_W-1:0] buffer_out_group_index;
    logic [BLOCK_SIZE*SPATIAL_W-1:0] buffer_out_spatial_index;

`ifndef SYNTHESIS
    // Simulation-only hierarchy mirrors.  The production s2/s3 state is
    // owned by im2col_gather_engine; these names preserve legacy TB probes.
    // Stage 2: one request per bank per cycle. A conflict-free vector leaves
    // this stage in one cycle; same-bank/different-row requests are serialized.
    logic s2_valid;
    logic s2_last;
    lane_req_t s2_req [BLOCK_SIZE];
    logic [BLOCK_SIZE-1:0] s2_lane_done;
    logic [BLOCK_SIZE-1:0] s2_pending_lane;
    logic [BLOCK_SIZE*ELEM_W-1:0] s2_data;
    logic [BLOCK_SIZE-1:0] s2_mask;
    logic [BLOCK_SIZE-1:0] s2_row_mask;
    logic [15:0] s2_n_index;
    logic [GROUP_W-1:0] s2_group_index;
    logic [BLOCK_SIZE*SPATIAL_W-1:0] s2_spatial_index;
    logic [SP_BANKS-1:0] s2_bank_pending;
    logic [SP_BANKS-1:0][BLOCK_SIZE-1:0] s2_bank_dst_mask;
    logic [BLOCK_SIZE-1:0] s2_lane_done_after_resp;
    logic [BLOCK_SIZE-1:0] s2_pending_after_resp;
    logic [BLOCK_SIZE*ELEM_W-1:0] s2_data_after_resp;
    logic [SP_BANKS-1:0] s2_bank_pending_after_resp;
    logic [BLOCK_SIZE-1:0] s2_response_lane_mask;
    logic [BLOCK_SIZE*ELEM_W-1:0] s2_response_lane_data;
    logic [BLOCK_SIZE-1:0] s2_issue_lane;
    logic [SP_BANKS-1:0] s2_issue_bank_valid;
    logic [SP_BANKS-1:0][BLOCK_SIZE-1:0] s2_issue_bank_dst_mask;
    logic [BLOCK_SIZE-1:0] s2_mask_after_issue;
    logic s2_complete_after_issue;

    // Stage 3: capture the final fixed-latency SRAM response and hold the
    // assembled vector until the output FIFO can accept it.
    logic s3_valid;
    logic s3_last;
    lane_req_t s3_req [BLOCK_SIZE];
    logic [BLOCK_SIZE-1:0] s3_pending_lane;
    logic [BLOCK_SIZE*ELEM_W-1:0] s3_data;
    logic [BLOCK_SIZE-1:0] s3_mask;
    logic [BLOCK_SIZE-1:0] s3_row_mask;
    logic [15:0] s3_n_index;
    logic [GROUP_W-1:0] s3_group_index;
    logic [BLOCK_SIZE*SPATIAL_W-1:0] s3_spatial_index;
    logic [SP_BANKS-1:0] s3_bank_pending;
    logic [SP_BANKS-1:0][BLOCK_SIZE-1:0] s3_bank_dst_mask;
    logic [BLOCK_SIZE-1:0] s3_pending_after_resp;
    logic [BLOCK_SIZE*ELEM_W-1:0] s3_data_after_resp;
    logic [SP_BANKS-1:0] s3_bank_pending_after_resp;
    logic [BLOCK_SIZE-1:0] s3_response_lane_mask;
    logic [BLOCK_SIZE*ELEM_W-1:0] s3_response_lane_data;
    logic s3_complete_after_resp;

    logic s1_to_s2;
    logic s2_to_s3;
    logic s3_ready;
`endif

    logic fifo_push;
    logic fifo_pop;
    logic gather_s1_ready;
    logic gather_busy;
    logic gather_output_valid;
    logic gather_output_last;
    logic [BLOCK_SIZE*ELEM_W-1:0] gather_output_data;
    logic [BLOCK_SIZE-1:0] gather_output_mask;
    logic [BLOCK_SIZE-1:0] gather_output_row_mask;
    logic [15:0] gather_output_n_index;
    logic [GROUP_W-1:0] gather_output_group_index;
    logic [BLOCK_SIZE*SPATIAL_W-1:0] gather_output_spatial_index;

    logic dbg_collect_all_done;
    logic [BLOCK_SIZE*SPATIAL_W-1:0] coord_gen_spatial_base;
    logic [BLOCK_SIZE-1:0] coord_gen_tap_active;
    logic [BLOCK_SIZE-1:0] coord_gen_boundary_valid;
    logic [BLOCK_SIZE-1:0] coord_gen_row_eligible;
    logic [BLOCK_SIZE-1:0] coord_gen_c_valid;
    logic [BLOCK_SIZE-1:0] coord_gen_is_padding;
    logic [BLOCK_SIZE*16-1:0] coord_gen_in_h;
    logic [BLOCK_SIZE*16-1:0] coord_gen_in_w;
    logic [BLOCK_SIZE-1:0] coord_req_valid;
    logic [BLOCK_SIZE-1:0] s1_req_valid_packed;
    logic [BLOCK_SIZE*SP_BANK_BITS-1:0] s1_req_bank_packed;
    logic [BLOCK_SIZE*SP_ROW_BITS-1:0] s1_req_row_packed;
    logic [BLOCK_SIZE-1:0] a1_req_valid;
    logic [BLOCK_SIZE*SP_BANK_BITS-1:0] a1_req_bank;
    logic [BLOCK_SIZE*SP_ROW_BITS-1:0] a1_req_row;

    assign start_accept = start && !busy && config_captured;
    assign start_reject = start && !busy && !config_captured;

    im2col_coord_stage0 #(
        .BLOCK_SIZE(BLOCK_SIZE),
        .KERNEL_PATTERN_BITS(KERNEL_PATTERN_BITS)
    ) u_coord_stage0 (
        .w_q(w_q), .mline_mode_q(mline_mode_q),
        .oh_idx(oh_idx), .ow_base(ow_base),
        .kh_idx(kh_idx), .kw_idx(kw_idx), .kernel_w_q(kernel_w_q),
        .rows_per_group_q(rows_per_group_q),
        .lane_out_h(coord_stage0_out_h), .lane_out_w(coord_stage0_out_w),
        .lane_row_eligible(coord_stage0_row_eligible),
        .tap_index(coord_stage0_tap_index)
    );

    im2col_coord_gen #(
        .BLOCK_SIZE(BLOCK_SIZE),
        .KERNEL_PATTERN_BITS(KERNEL_PATTERN_BITS),
        .SPATIAL_W(SPATIAL_W)
    ) u_coord_gen (
        .c_count_q(c_count_q),
        .h_q(h_q), .w_q(w_q), .out_h_q(out_h_q), .out_w_q(out_w_q),
        .stride_h_q(stride_h_q), .stride_w_q(stride_w_q),
        .dilation_h_q(dilation_h_q), .dilation_w_q(dilation_w_q),
        .pad_top_q(pad_top_q), .pad_left_q(pad_left_q),
        .kernel_pattern_q(kernel_pattern_q),
        .c_idx(stage0_c_idx),
        .stage0_kh_idx(stage0_kh_idx), .stage0_kw_idx(stage0_kw_idx),
        .stage0_out_h(stage0_out_h), .stage0_out_w(stage0_out_w),
        .stage0_row_eligible(stage0_row_eligible),
        .stage0_tap_index(stage0_tap_index),
        .lane_spatial_base(coord_gen_spatial_base),
        .lane_tap_active(coord_gen_tap_active),
        .lane_boundary_valid(coord_gen_boundary_valid),
        .lane_row_eligible(coord_gen_row_eligible),
        .lane_c_valid(coord_gen_c_valid),
        .lane_is_padding(coord_gen_is_padding),
        .lane_in_h(coord_gen_in_h), .lane_in_w(coord_gen_in_w)
    );

    function automatic [31:0] g0_chw_byte_addr(
        input logic [15:0] n,
        input logic [15:0] c,
        input logic [15:0] h,
        input logic [15:0] w
    );
        logic [31:0] tensor_offset;
        begin
            tensor_offset = n * batch_byte_stride +
                (c_base_q + c) * channel_byte_stride +
                h * {16'd0, w_q} + {16'd0, w};
            g0_chw_byte_addr = spad_base_q + tensor_offset;
        end
    endfunction

    // Finish the registered coordinate token. This is deliberately limited
    // to additions/Boolean combines and the address split consumed by A1.
    always_comb begin
        coord_req_valid = '0;
        lane_zero = '0;
        lane_row_mask = '0;
        lane_spatial_index = '0;
        for (int i = 0; i < BLOCK_SIZE; i++) begin
            logic row_mask_i;

            row_mask_i = coord_boundary_valid_q[i] && coord_row_eligible_q[i];
            lane_row_mask[i] = row_mask_i;
            if (row_mask_i)
                lane_spatial_index[i*SPATIAL_W +: SPATIAL_W] =
                    coord_spatial_base_q[i*SPATIAL_W +: SPATIAL_W] +
                    coord_out_w_q[i*16 +: 16];
            coord_req_valid[i] = coord_tap_active_q[i] && row_mask_i &&
                !coord_is_padding_q[i] && coord_c_valid_q[i];
            lane_zero[i] = coord_tap_active_q[i] && row_mask_i &&
                coord_is_padding_q[i] && coord_c_valid_q[i];
        end
    end

    // A1 remains combinational in Step 2.  It consumes only the registered
    // G0 geometry, so the cursor-to-address cone is split at g0_valid.
    always_comb begin
        a1_req_valid = '0;
        a1_req_bank = '0;
        a1_req_row = '0;
        for (int i = 0; i < BLOCK_SIZE; i++) begin
            logic [15:0] in_h_i;
            logic [15:0] in_w_i;
            logic [31:0] byte_addr;

            in_h_i = g0_in_h[i*16 +: 16];
            in_w_i = g0_in_w[i*16 +: 16];
            byte_addr = g0_chw_byte_addr(g0_n_idx, g0_c_idx, in_h_i, in_w_i);
            a1_req_valid[i] = g0_req_valid[i];
            a1_req_bank[i*SP_BANK_BITS +: SP_BANK_BITS] =
                byte_addr[SP_BANK_BITS-1:0];
            a1_req_row[i*SP_ROW_BITS +: SP_ROW_BITS] =
                byte_addr[SP_ADDR_BITS-1:SP_BANK_BITS];
        end
    end

    always_comb begin
        s1_req_valid_packed = '0;
        s1_req_bank_packed = '0;
        s1_req_row_packed = '0;
        for (int i = 0; i < BLOCK_SIZE; i++) begin
            s1_req_valid_packed[i] = s1_req[i].valid;
            s1_req_bank_packed[i*SP_BANK_BITS +: SP_BANK_BITS] = s1_req[i].bank;
            s1_req_row_packed[i*SP_ROW_BITS +: SP_ROW_BITS] = s1_req[i].row;
        end
    end

    im2col_cfg_cursor #(
        .BLOCK_SIZE(BLOCK_SIZE),
        .SP_BANKS(SP_BANKS),
        .SP_BANK_ENTRIES(SP_BANK_ENTRIES),
        .SP_ADDR_BITS(SP_ADDR_BITS),
        .KERNEL_PATTERN_BITS(KERNEL_PATTERN_BITS),
        .GROUP_W(GROUP_W),
        .MAX_SEG_C(MAX_SEG_C)
    ) u_cfg_cursor (
        .clk(clk),
        .rst_n(rst_n),
        .cfg_valid(cfg_valid),
        .cfg_spad_base(cfg_spad_base),
        .cfg_n(cfg_n),
        .cfg_c(cfg_c),
        .cfg_c_base(cfg_c_base),
        .cfg_c_count(cfg_c_count),
        .cfg_single_group_mode(cfg_single_group_mode),
        .cfg_group_n(cfg_group_n),
        .cfg_group_index(cfg_group_index),
        .cfg_group_oh_base(cfg_group_oh_base),
        .cfg_group_ow_base(cfg_group_ow_base),
        .cfg_h(cfg_h),
        .cfg_w(cfg_w),
        .cfg_out_h(cfg_out_h),
        .cfg_out_w(cfg_out_w),
        .cfg_mline_mode(cfg_mline_mode),
        .cfg_rows_per_group(cfg_rows_per_group),
        .cfg_kernel_h(cfg_kernel_h),
        .cfg_kernel_w(cfg_kernel_w),
        .cfg_stride_h(cfg_stride_h),
        .cfg_stride_w(cfg_stride_w),
        .cfg_dilation_h(cfg_dilation_h),
        .cfg_dilation_w(cfg_dilation_w),
        .cfg_pad_top(cfg_pad_top),
        .cfg_pad_left(cfg_pad_left),
        .cfg_kernel_pattern(cfg_kernel_pattern),
        .cfg_groups_per_n(cfg_groups_per_n),
        .start_accept(start_accept),
        .cursor_step(cursor_step),
        .spad_base_q(spad_base_q),
        .n_q(n_q),
        .c_q(c_q),
        .c_base_q(c_base_q),
        .c_count_q(c_count_q),
        .h_q(h_q),
        .w_q(w_q),
        .out_h_q(out_h_q),
        .out_w_q(out_w_q),
        .kernel_h_q(kernel_h_q),
        .kernel_w_q(kernel_w_q),
        .stride_h_q(stride_h_q),
        .stride_w_q(stride_w_q),
        .dilation_h_q(dilation_h_q),
        .dilation_w_q(dilation_w_q),
        .pad_top_q(pad_top_q),
        .pad_left_q(pad_left_q),
        .kernel_pattern_q(kernel_pattern_q),
        .single_group_mode_q(single_group_mode_q),
        .group_n_q(group_n_q),
        .group_config_index_q(group_config_index_q),
        .n_idx(n_idx),
        .c_idx(c_idx),
        .oh_idx(oh_idx),
        .ow_base(ow_base),
        .group_idx(group_idx),
        .kh_idx(kh_idx),
        .kw_idx(kw_idx),
        .mline_mode_q(mline_mode_q),
        .rows_per_group_q(rows_per_group_q),
        .channel_byte_stride(channel_byte_stride),
        .batch_byte_stride(batch_byte_stride),
        .config_captured(config_captured),
        .groups_per_n(groups_per_n),
        .cursor_last(cursor_last),
        .producer_active(producer_active)
    );


    // Rebuild the parameterized lane request type from the A1 outputs.
    always_comb begin
        for (int i = 0; i < BLOCK_SIZE; i++) begin
            lane_req[i] = '0;
            lane_req[i].valid = a1_req_valid[i];
            lane_req[i].bank = a1_req_bank[i*SP_BANK_BITS +: SP_BANK_BITS];
            lane_req[i].row = a1_req_row[i*SP_ROW_BITS +: SP_ROW_BITS];
            lane_req[i].dst_lane = i[LANE_W-1:0];
        end
    end

    im2col_descriptor_buffer #(
        .BLOCK_SIZE(BLOCK_SIZE), .SP_BANK_BITS(SP_BANK_BITS),
        .SP_ROW_BITS(SP_ROW_BITS), .GROUP_W(GROUP_W), .SPATIAL_W(SPATIAL_W),
        .DEPTH(DESCRIPTOR_DEPTH)
    ) u_descriptor_buffer (
        .clk(clk), .rst_n(rst_n),
        .in_valid(s1_valid), .in_ready(buffer_in_ready),
        .in_last(s1_last), .in_req_valid(s1_req_valid_packed),
        .in_req_bank(s1_req_bank_packed), .in_req_row(s1_req_row_packed),
        .in_zero(s1_zero), .in_row_mask(s1_row_mask),
        .in_n_index(s1_n_index), .in_group_index(s1_group_index),
        .in_spatial_index(s1_spatial_index),
        .out_valid(buffer_out_valid), .out_ready(gather_s1_ready),
        .out_last(buffer_out_last), .out_req_valid(buffer_out_req_valid),
        .out_req_bank(buffer_out_req_bank), .out_req_row(buffer_out_req_row),
        .out_zero(buffer_out_zero), .out_row_mask(buffer_out_row_mask),
        .out_n_index(buffer_out_n_index), .out_group_index(buffer_out_group_index),
        .out_spatial_index(buffer_out_spatial_index),
        .occupancy(descriptor_occupancy)
    );

    im2col_gather_engine #(
        .BLOCK_SIZE(BLOCK_SIZE), .ELEM_W(ELEM_W), .SP_BANKS(SP_BANKS),
        .SP_BANK_BITS(SP_BANK_BITS), .SP_ROW_BITS(SP_ROW_BITS),
        .SP_ADDR_BITS(SP_ADDR_BITS), .GROUP_W(GROUP_W), .SPATIAL_W(SPATIAL_W)
    ) u_gather_engine (
        .clk(clk), .rst_n(rst_n),
        .s1_valid(buffer_out_valid), .s1_last(buffer_out_last),
        .s1_req_valid(buffer_out_req_valid),
        .s1_req_bank(buffer_out_req_bank), .s1_req_row(buffer_out_req_row),
        .s1_zero(buffer_out_zero), .s1_row_mask(buffer_out_row_mask),
        .s1_n_index(buffer_out_n_index), .s1_group_index(buffer_out_group_index),
        .s1_spatial_index(buffer_out_spatial_index),
        .sram_resp_valid(sram_resp_valid), .sram_resp_data(sram_resp_data),
        .output_ready(fifo_push), .s1_ready(gather_s1_ready),
        .sram_req_valid(sram_req_valid), .sram_req_addr(sram_req_addr),
        .output_valid(gather_output_valid), .output_last(gather_output_last),
        .output_data(gather_output_data), .output_mask(gather_output_mask),
        .output_row_mask(gather_output_row_mask),
        .output_n_index(gather_output_n_index),
        .output_group_index(gather_output_group_index),
        .output_spatial_index(gather_output_spatial_index),
        .busy(gather_busy), .collect_done(dbg_collect_all_done)
    );

    assign s1_to_buffer = s1_valid && buffer_in_ready;
    assign s1_to_s2 = s1_to_buffer;
    assign s1_ready = !s1_valid || s1_to_buffer;
    assign g0_to_s1 = g0_valid && s1_ready;
    assign g0_ready = !g0_valid || s1_ready;
    assign coord_to_g0 = coord_valid && g0_ready;
    assign coord_ready = !coord_valid || g0_ready;
    // The cursor is the producer of the Stage-0 token. Keep the elastic
    // contract explicit: producer_active is input valid and cursor_step is
    // the Stage-0 input transfer event.
    assign stage0_in_valid = producer_active;
    assign stage0_ready = !stage0_valid || coord_ready;
    assign stage0_fire = stage0_in_valid && stage0_ready;
    assign stage0_to_coord = stage0_valid && coord_ready;
    // Compatibility alias: the cursor input fire is now the stage-0 fire.
    // No downstream logic uses g0_in_fire as a separate protocol event.
    assign g0_in_fire = stage0_fire;
    assign cursor_step = stage0_fire;

    im2col_output_fifo #(
        .BLOCK_SIZE(BLOCK_SIZE), .ELEM_W(ELEM_W), .FIFO_DEPTH(FIFO_DEPTH),
        .FIFO_PTR_W(FIFO_PTR_W), .GROUP_W(GROUP_W), .SPATIAL_W(SPATIAL_W)
    ) u_output_fifo (
        .clk(clk), .rst_n(rst_n), .in_valid(gather_output_valid),
        .in_data(gather_output_data), .in_mask(gather_output_mask),
        .in_row_mask(gather_output_row_mask), .in_n_index(gather_output_n_index),
        .in_group_index(gather_output_group_index),
        .in_spatial_index(gather_output_spatial_index), .feed_ready(feed_ready),
        .feed_valid(feed_valid), .feed_data(feed_data), .feed_mask(feed_mask),
        .feed_row_mask(feed_row_mask), .feed_n_index(feed_n_index),
        .feed_group_index(feed_group_index), .feed_spatial_index(feed_spatial_index),
        .push_fire(fifo_push), .pop_fire(fifo_pop)
    );

`ifndef SYNTHESIS
    // Compatibility mirrors for existing waveform/testbench hierarchy probes.
    assign s2_valid = u_gather_engine.s2_valid;
    assign s2_pending_lane = u_gather_engine.s2_pending_lane;
    assign s2_data_after_resp = u_gather_engine.s2_data_after_resp;
    assign s2_bank_pending = u_gather_engine.s2_bank_pending;
    assign s2_bank_dst_mask = u_gather_engine.s2_bank_dst_mask;
    assign s2_issue_lane = u_gather_engine.s2_issue_lane;
    assign s2_to_s3 = u_gather_engine.s2_to_s3;
    assign s3_valid = u_gather_engine.s3_valid;
    assign s3_pending_lane = u_gather_engine.s3_pending_lane;
    assign s3_data_after_resp = u_gather_engine.s3_data_after_resp;
    assign s3_bank_pending = u_gather_engine.s3_bank_pending;
    assign s3_bank_dst_mask = u_gather_engine.s3_bank_dst_mask;
    for (genvar g = 0; g < BLOCK_SIZE; g++) begin : g_compat_req
        assign s2_req[g] = u_gather_engine.s2_req[g];
        assign s3_req[g] = u_gather_engine.s3_req[g];
    end
`endif

    assign busy = producer_active || stage0_valid || coord_valid || g0_valid || s1_valid ||
                  (descriptor_occupancy != 0) || gather_busy;

    always_comb begin
        if (!busy)
            state = ST_IDLE;
        else if (done)
            state = ST_DONE;
        else if (gather_output_valid)
            state = ST_PUSH;
        else if (gather_busy)
            state = ST_COLLECT;
        else
            state = ST_ISSUE;
    end

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            done <= 1'b0;
            cfg_error <= 1'b0;
            stage0_valid <= 1'b0;
            stage0_n_idx <= '0;
            stage0_c_idx <= '0;
            stage0_group_idx <= '0;
            stage0_kh_idx <= '0;
            stage0_kw_idx <= '0;
            stage0_last <= 1'b0;
            stage0_out_h <= '0;
            stage0_out_w <= '0;
            stage0_row_eligible <= '0;
            stage0_tap_index <= '0;
            coord_valid <= 1'b0;
            coord_last <= 1'b0;
            coord_spatial_base_q <= '0;
            coord_out_w_q <= '0;
            coord_tap_active_q <= '0;
            coord_boundary_valid_q <= '0;
            coord_row_eligible_q <= '0;
            coord_c_valid_q <= '0;
            coord_is_padding_q <= '0;
            coord_in_h_q <= '0;
            coord_in_w_q <= '0;
            coord_n_index_q <= '0;
            coord_group_index_q <= '0;
            coord_n_idx_q <= '0;
            coord_c_idx_q <= '0;
            g0_valid <= 1'b0;
            g0_last <= 1'b0;
            g0_req_valid <= '0;
            g0_zero <= '0;
            g0_row_mask <= '0;
            g0_in_h <= '0;
            g0_in_w <= '0;
            g0_spatial_index <= '0;
            g0_n_index <= '0;
            g0_group_index <= '0;
            g0_n_idx <= '0;
            g0_c_idx <= '0;
            s1_valid <= 1'b0;
            s1_last <= 1'b0;
            s1_zero <= '0;
            s1_row_mask <= '0;
            s1_n_index <= '0;
            s1_group_index <= '0;
            s1_spatial_index <= '0;
            for (int i = 0; i < BLOCK_SIZE; i++)
                s1_req[i] <= '0;
        end else begin
            done <= 1'b0;
            cfg_error <= 1'b0;
            if (start_reject) begin
                cfg_error <= 1'b1;
                done <= 1'b1;
            end
            if (fifo_push && gather_output_last)
                done <= 1'b1;

            // Stage 0 captures one complete cursor token. It may accept the
            // next token while its previous token advances into Coord.
            if (stage0_fire) begin
                stage0_valid <= 1'b1;
                stage0_n_idx <= n_idx;
                stage0_c_idx <= c_idx;
                stage0_group_idx <= group_idx;
                stage0_kh_idx <= kh_idx;
                stage0_kw_idx <= kw_idx;
                stage0_last <= cursor_last;
                stage0_out_h <= coord_stage0_out_h;
                stage0_out_w <= coord_stage0_out_w;
                stage0_row_eligible <= coord_stage0_row_eligible;
                stage0_tap_index <= coord_stage0_tap_index;
            end else if (stage0_to_coord) begin
                stage0_valid <= 1'b0;
            end

            // Coord atomically captures all metadata generated from a held
            // Stage-0 token. Its payload is stable while downstream stalls.
            if (stage0_to_coord) begin
                coord_valid <= 1'b1;
                coord_last <= stage0_last;
                coord_spatial_base_q <= coord_gen_spatial_base;
                coord_out_w_q <= stage0_out_w;
                coord_tap_active_q <= coord_gen_tap_active;
                coord_boundary_valid_q <= coord_gen_boundary_valid;
                coord_row_eligible_q <= coord_gen_row_eligible;
                coord_c_valid_q <= coord_gen_c_valid;
                coord_is_padding_q <= coord_gen_is_padding;
                coord_in_h_q <= coord_gen_in_h;
                coord_in_w_q <= coord_gen_in_w;
                coord_n_index_q <= stage0_n_idx;
                coord_group_index_q <= stage0_group_idx;
                coord_n_idx_q <= stage0_n_idx;
                coord_c_idx_q <= stage0_c_idx;
            end else if (coord_to_g0) begin
                coord_valid <= 1'b0;
            end

            // G0 consumes only the held Coord token. All validity, padding,
            // spatial and coordinate fields remain atomic through A1/S1.
            if (coord_to_g0) begin
                g0_valid <= 1'b1;
                g0_last <= coord_last;
                g0_req_valid <= coord_req_valid;
                g0_zero <= lane_zero;
                g0_row_mask <= lane_row_mask;
                g0_in_h <= coord_in_h_q;
                g0_in_w <= coord_in_w_q;
                g0_spatial_index <= lane_spatial_index;
                g0_n_index <= coord_n_index_q;
                g0_group_index <= coord_group_index_q;
                g0_n_idx <= coord_n_idx_q;
                g0_c_idx <= coord_c_idx_q;
            end else if (g0_to_s1) begin
                g0_valid <= 1'b0;
            end

            // A1 is combinational in this step; s1 remains the existing
            // descriptor register and captures the complete A1 payload.
            if (g0_to_s1) begin
                s1_valid <= 1'b1;
                s1_last <= g0_last;
                s1_zero <= g0_zero;
                s1_row_mask <= g0_row_mask;
                s1_n_index <= g0_n_index;
                s1_group_index <= g0_group_index;
                s1_spatial_index <= g0_spatial_index;
                for (int i = 0; i < BLOCK_SIZE; i++)
                    s1_req[i] <= lane_req[i];
            end else if (s1_to_s2) begin
                s1_valid <= 1'b0;
            end
        end
    end

endmodule

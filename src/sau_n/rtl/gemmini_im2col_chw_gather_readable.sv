// CHW/W-inner gather-style Im2Col reference RTL.
//
// This is the same idea as docs/gemmini_im2col_gather_readable.sv, but the
// scratchpad/input layout assumption is changed from NHWC-like rows to CHW-like
// rows:
//
//   row address order:
//     N -> C -> packed H/W word
//
//   row data:
//     if W <= 16:
//       pack floor(16 / W) complete H rows into one scratchpad row.
//       Example W=5:
//         row data = h0.w0..w4, h1.w0..w4, h2.w0..w4, zero
//
//     if W > 16:
//       split one H row over multiple W words:
//         row data = input[n][c][h][w_base + 0 ... w_base + 15]
//
// In other words, W is the innermost dimension and one logical 16-byte row is
// physically striped across 16 int8 SRAM banks. The CHW lane index selects the
// int8 bank, while the packed H/W word index selects that bank's row.
//
// This is a readable design sketch, not drop-in Gemmini RTL.

module gemmini_im2col_chw_gather_readable #(
    parameter int BLOCK_SIZE      = 16,
    parameter int ELEM_W          = 8,
    parameter int SP_BANKS        = BLOCK_SIZE,
    parameter int SP_BANK_ENTRIES = 4096,
    parameter int FIFO_DEPTH      = 4,
    parameter int SP_BANK_BITS    = $clog2(SP_BANKS),
    parameter int SP_ROW_BITS     = $clog2(SP_BANK_ENTRIES),
    parameter int SP_ADDR_BITS    = SP_BANK_BITS + SP_ROW_BITS,
    parameter int FIFO_PTR_W      = $clog2(FIFO_DEPTH),
    parameter int LANE_W          = $clog2(BLOCK_SIZE)
) (
    input  logic                         clk,
    input  logic                         rst_n,

    input  logic                         cfg_valid,
    input  logic [SP_ADDR_BITS-1:0]      cfg_spad_base,
    input  logic [15:0]                  cfg_n,
    input  logic [15:0]                  cfg_c,
    input  logic [15:0]                  cfg_h,
    input  logic [15:0]                  cfg_w,
    input  logic [15:0]                  cfg_out_h,
    input  logic [15:0]                  cfg_out_w,
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

    // One bit per kernel tap. For a 3x3 dilation=2 horizontal tap group this
    // can represent a pattern like 1,0,1,0,1,...
    input  logic [BLOCK_SIZE-1:0]         cfg_kernel_pattern,

    input  logic                         start,
    output logic                         busy,
    output logic                         done,

    output logic [SP_BANKS-1:0]           sram_req_valid,
    output logic [SP_BANKS-1:0][SP_ROW_BITS-1:0] sram_req_addr,
    input  logic [SP_BANKS-1:0]           sram_resp_valid,
    input  logic [SP_BANKS-1:0][ELEM_W-1:0] sram_resp_data,

    output logic                         feed_valid,
    input  logic                         feed_ready,
    output logic [BLOCK_SIZE*ELEM_W-1:0] feed_data,
    output logic [BLOCK_SIZE-1:0]         feed_mask
);

    typedef enum logic [2:0] {
        ST_IDLE,
        ST_ISSUE,
        ST_COLLECT,
        ST_PUSH,
        ST_NEXT,
        ST_DONE
    } state_e;

    typedef struct packed {
        logic valid;
        logic [SP_BANK_BITS-1:0] bank;
        logic [SP_ROW_BITS-1:0] row;
        logic [LANE_W-1:0] lane_sel;
        logic [LANE_W-1:0] dst_lane;
    } lane_req_t;

    state_e state;

    logic [SP_ADDR_BITS-1:0] spad_base_q;
    logic [15:0] n_q, c_q, h_q, w_q, out_h_q, out_w_q;
    logic [3:0] kernel_h_q, kernel_w_q;
    logic [3:0] stride_h_q, stride_w_q;
    logic [3:0] dilation_h_q, dilation_w_q;
    logic [15:0] pad_top_q, pad_left_q;
    logic dw_mode_q;
    logic [BLOCK_SIZE-1:0] kernel_pattern_q;

    // Output tile and kernel/channel counters.
    logic [15:0] n_idx;
    logic [15:0] c_idx;
    logic [15:0] oh_idx;
    logic [15:0] ow_base;
    logic [3:0]  kh_idx;
    logic [3:0]  kw_idx;

    logic [31:0] w_words;
    logic [31:0] rows_per_word;
    logic [31:0] spatial_words_per_channel;
    logic [31:0] n_word_stride;

    lane_req_t lane_req [BLOCK_SIZE];
    lane_req_t lane_req_q [BLOCK_SIZE];
    logic [BLOCK_SIZE-1:0] lane_zero;
    logic [BLOCK_SIZE-1:0] lane_done;
    logic [BLOCK_SIZE*ELEM_W-1:0] interm_data;
    logic [BLOCK_SIZE-1:0] interm_valid;

    logic [BLOCK_SIZE*ELEM_W-1:0] fifo_data [FIFO_DEPTH];
    logic [BLOCK_SIZE-1:0] fifo_mask [FIFO_DEPTH];
    logic [FIFO_PTR_W:0] fifo_count;
    logic [FIFO_PTR_W-1:0] fifo_rptr;
    logic [FIFO_PTR_W-1:0] fifo_wptr;
    logic fifo_push;
    logic fifo_pop;

    // Debug-friendly mirrors of the procedural lane calculations. These are
    // intentionally module-level signals so DVE can show them as normal waves.
    logic [15:0] dbg_out_h [BLOCK_SIZE];
    logic [15:0] dbg_out_w [BLOCK_SIZE];
    logic [31:0] dbg_padded_h [BLOCK_SIZE];
    logic [31:0] dbg_padded_w [BLOCK_SIZE];
    logic signed [32:0] dbg_real_h [BLOCK_SIZE];
    logic signed [32:0] dbg_real_w [BLOCK_SIZE];
    logic [31:0] dbg_local_h [BLOCK_SIZE];
    logic [31:0] dbg_local_w [BLOCK_SIZE];
    logic [SP_ADDR_BITS-1:0] dbg_row_addr [BLOCK_SIZE];
    logic [SP_BANK_BITS-1:0] dbg_bank [BLOCK_SIZE];
    logic [SP_ROW_BITS-1:0] dbg_row [BLOCK_SIZE];
    logic [LANE_W-1:0] dbg_lane_sel [BLOCK_SIZE];
    logic [3:0] dbg_tap_index [BLOCK_SIZE];
    logic [BLOCK_SIZE-1:0] dbg_is_padding;
    logic [BLOCK_SIZE-1:0] dbg_lane_req_valid;
    logic [BLOCK_SIZE-1:0] dbg_lane_zero;
    logic dbg_collect_all_done;

    function automatic [SP_ROW_BITS-1:0] sp_row(input logic [SP_ADDR_BITS-1:0] a);
        sp_row = a[SP_ROW_BITS-1:0];
    endfunction

    function automatic [31:0] rows_per_word_lut(input logic [15:0] w);
        begin
            unique case (w)
                16'd0: rows_per_word_lut = 32'd1;
                16'd1: rows_per_word_lut = 32'd16;
                16'd2: rows_per_word_lut = 32'd8;
                16'd3: rows_per_word_lut = 32'd5;
                16'd4: rows_per_word_lut = 32'd4;
                16'd5: rows_per_word_lut = 32'd3;
                16'd6, 16'd7, 16'd8: rows_per_word_lut = 32'd2;
                default: rows_per_word_lut = 32'd1;
            endcase
        end
    endfunction

    function automatic [31:0] ceil_h_by_rows_per_word(
        input logic [15:0] h,
        input logic [31:0] rpword
    );
        logic [16:0] h_plus_4;
        begin
            h_plus_4 = {1'b0, h} + 17'd4;
            unique case (rpword)
                32'd16: ceil_h_by_rows_per_word = ({16'd0, h} + 32'd15) >> 4;
                32'd8:  ceil_h_by_rows_per_word = ({16'd0, h} + 32'd7) >> 3;
                32'd5:  ceil_h_by_rows_per_word = (h_plus_4 * 32'd52429) >> 18;
                32'd4:  ceil_h_by_rows_per_word = ({16'd0, h} + 32'd3) >> 2;
                32'd3:  ceil_h_by_rows_per_word = (({1'b0, h} + 17'd2) * 32'd43691) >> 17;
                32'd2:  ceil_h_by_rows_per_word = ({16'd0, h} + 32'd1) >> 1;
                default: ceil_h_by_rows_per_word = {16'd0, h};
            endcase
        end
    endfunction

    function automatic [31:0] h_div_rows_per_word(
        input logic [15:0] h,
        input logic [31:0] rpword
    );
        begin
            unique case (rpword)
                32'd16: h_div_rows_per_word = {16'd0, h} >> 4;
                32'd8:  h_div_rows_per_word = {16'd0, h} >> 3;
                32'd4:  h_div_rows_per_word = {16'd0, h} >> 2;
                32'd2:  h_div_rows_per_word = {16'd0, h} >> 1;
                32'd5:  h_div_rows_per_word = ({1'b0, h} * 32'd52429) >> 18;
                32'd3:  h_div_rows_per_word = ({1'b0, h} * 32'd43691) >> 17;
                default: h_div_rows_per_word = {16'd0, h};
            endcase
        end
    endfunction

    function automatic [31:0] h_mod_rows_per_word(
        input logic [15:0] h,
        input logic [31:0] rpword
    );
        logic [31:0] div_value;
        begin
            div_value = h_div_rows_per_word(h, rpword);
            unique case (rpword)
                32'd16: h_mod_rows_per_word = {28'd0, h[3:0]};
                32'd8:  h_mod_rows_per_word = {29'd0, h[2:0]};
                32'd5:  h_mod_rows_per_word = {16'd0, h} - div_value * 32'd5;
                32'd4:  h_mod_rows_per_word = {30'd0, h[1:0]};
                32'd3:  h_mod_rows_per_word = {16'd0, h} - div_value * 32'd3;
                32'd2:  h_mod_rows_per_word = {31'd0, h[0]};
                default: h_mod_rows_per_word = 32'd0;
            endcase
        end
    endfunction

    function automatic [31:0] lane_div_w(
        input int lane,
        input logic [15:0] w
    );
        begin
            unique case (w)
                16'd0: lane_div_w = 32'd0;
                16'd1: lane_div_w = lane;
                16'd2: lane_div_w = lane >> 1;
                16'd3: lane_div_w = (lane < 3) ? 32'd0 : (lane < 6) ? 32'd1 :
                                     (lane < 9) ? 32'd2 : (lane < 12) ? 32'd3 :
                                     (lane < 15) ? 32'd4 : 32'd5;
                16'd4: lane_div_w = lane >> 2;
                16'd5: lane_div_w = (lane < 5) ? 32'd0 : (lane < 10) ? 32'd1 :
                                     (lane < 15) ? 32'd2 : 32'd3;
                16'd6: lane_div_w = (lane < 6) ? 32'd0 : (lane < 12) ? 32'd1 : 32'd2;
                16'd7: lane_div_w = (lane < 7) ? 32'd0 : (lane < 14) ? 32'd1 : 32'd2;
                16'd8: lane_div_w = lane >> 3;
                default: lane_div_w = 32'd0;
            endcase
        end
    endfunction

    function automatic [31:0] lane_mod_w(
        input int lane,
        input logic [15:0] w
    );
        logic [31:0] div_value;
        begin
            div_value = lane_div_w(lane, w);
            unique case (w)
                16'd0: lane_mod_w = 32'd0;
                16'd1: lane_mod_w = 32'd0;
                16'd2: lane_mod_w = lane[0];
                16'd3: lane_mod_w = lane - div_value * 32'd3;
                16'd4: lane_mod_w = lane[1:0];
                16'd5: lane_mod_w = lane - div_value * 32'd5;
                16'd6: lane_mod_w = lane - div_value * 32'd6;
                16'd7: lane_mod_w = lane - div_value * 32'd7;
                16'd8: lane_mod_w = lane[2:0];
                default: lane_mod_w = lane;
            endcase
        end
    endfunction

    function automatic [SP_ADDR_BITS-1:0] chw_row_addr(
        input logic [15:0] n,
        input logic [15:0] c,
        input logic [15:0] h,
        input logic [15:0] w
    );
        logic [31:0] w_word;
        logic [31:0] h_word;
        logic [31:0] word_offset;
        begin
            if (w_q <= BLOCK_SIZE) begin
                h_word = h_div_rows_per_word(h, rows_per_word);
                word_offset =
                    n * n_word_stride +
                    c * spatial_words_per_channel +
                    h_word;
            end else begin
                w_word = {16'd0, w} >> 4;
                word_offset =
                    n * n_word_stride +
                    c * spatial_words_per_channel +
                    h * w_words +
                    w_word;
            end
            chw_row_addr = spad_base_q + word_offset[SP_ADDR_BITS-1:0];
        end
    endfunction

    function automatic [LANE_W-1:0] chw_lane_sel(
        input logic [15:0] h,
        input logic [15:0] w
    );
        logic [31:0] lane;
        begin
            if (w_q <= BLOCK_SIZE) begin
                lane = h_mod_rows_per_word(h, rows_per_word) * w_q + w;
            end else begin
                lane = {28'd0, w[3:0]};
            end
            chw_lane_sel = lane[LANE_W-1:0];
        end
    endfunction

    assign w_words = (w_q == 0) ? 32'd1 : (({16'd0, w_q} + 32'd15) >> 4);
    assign rows_per_word = rows_per_word_lut(w_q);
    assign spatial_words_per_channel =
        (w_q <= BLOCK_SIZE) ? ceil_h_by_rows_per_word(h_q, rows_per_word) :
                               ({16'd0, h_q} * w_words);
    assign n_word_stride = c_q * spatial_words_per_channel;

    assign feed_valid = (fifo_count != 0);
    assign feed_data  = fifo_data[fifo_rptr];
    assign feed_mask  = fifo_mask[fifo_rptr];
    assign fifo_pop   = feed_valid && feed_ready;
    assign fifo_push  = (state == ST_PUSH) && (fifo_count != FIFO_DEPTH);
    assign busy       = (state != ST_IDLE);

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            spad_base_q <= '0;
            n_q <= '0;
            c_q <= '0;
            h_q <= '0;
            w_q <= '0;
            out_h_q <= '0;
            out_w_q <= '0;
            kernel_h_q <= '0;
            kernel_w_q <= '0;
            stride_h_q <= '0;
            stride_w_q <= '0;
            dilation_h_q <= '0;
            dilation_w_q <= '0;
            pad_top_q <= '0;
            pad_left_q <= '0;
            dw_mode_q <= 1'b0;
            kernel_pattern_q <= '0;
        end else if (cfg_valid) begin
            spad_base_q <= cfg_spad_base;
            n_q <= cfg_n;
            c_q <= cfg_c;
            h_q <= cfg_h;
            w_q <= cfg_w;
            out_h_q <= cfg_out_h;
            out_w_q <= cfg_out_w;
            kernel_h_q <= cfg_kernel_h;
            kernel_w_q <= cfg_kernel_w;
            stride_h_q <= cfg_stride_h;
            stride_w_q <= cfg_stride_w;
            dilation_h_q <= cfg_dilation_h;
            dilation_w_q <= cfg_dilation_w;
            pad_top_q <= cfg_pad_top;
            pad_left_q <= cfg_pad_left;
            dw_mode_q <= cfg_dw_mode;
            kernel_pattern_q <= cfg_kernel_pattern;
        end
    end

    // Build lane requests for one feed vector.
    //
    // For CHW/W-inner layout:
    //
    // If W <= 16, several H rows can share one scratchpad row. For example
    // W=5 packs three H rows and leaves one zero lane. In this case feed lane i
    // maps to:
    //
    //   local_h = i / W
    //   local_w = i % W
    //   out_h   = oh_idx + local_h
    //   out_w   = local_w
    //
    // and the scratchpad source lane/int8 bank is:
    //
    //   lane_sel = (h % floor(16/W)) * W + w
    //
    // If W > 16, one H row is split across W words and:
    //
    //   out_h = oh_idx
    //   out_w = ow_base + i
    //   lane_sel = w % 16
    always_comb begin
        for (int i = 0; i < BLOCK_SIZE; i++) begin
            logic [15:0] out_h_i;
            logic [15:0] out_w_i;
            logic [15:0] in_h_i;
            logic [15:0] in_w_i;
            logic [31:0] padded_h_i;
            logic [31:0] padded_w_i;
            logic signed [32:0] real_h_i;
            logic signed [32:0] real_w_i;
            logic is_padding;
            logic [31:0] local_h;
            logic [31:0] local_w;
            logic [SP_ADDR_BITS-1:0] row_addr;
            logic [3:0] tap_index;

            if (w_q <= BLOCK_SIZE) begin
                local_h = lane_div_w(i, w_q);
                local_w = lane_mod_w(i, w_q);
                out_h_i = oh_idx + local_h[15:0];
                out_w_i = local_w[15:0];
            end else begin
                local_h = 32'd0;
                local_w = i;
                out_h_i = oh_idx;
                out_w_i = ow_base + i;
            end

            padded_h_i = oh_idx * stride_h_q + kh_idx * dilation_h_q;
            if (w_q <= BLOCK_SIZE) begin
                padded_h_i = out_h_i * stride_h_q + kh_idx * dilation_h_q;
            end
            padded_w_i = out_w_i * stride_w_q + kw_idx * dilation_w_q;
            real_h_i = $signed({1'b0, padded_h_i}) - $signed({1'b0, pad_top_q});
            real_w_i = $signed({1'b0, padded_w_i}) - $signed({1'b0, pad_left_q});
            is_padding =
                (real_h_i < 0) ||
                (real_w_i < 0) ||
                (real_h_i >= $signed({1'b0, h_q})) ||
                (real_w_i >= $signed({1'b0, w_q}));

            in_h_i = real_h_i[15:0];
            in_w_i = real_w_i[15:0];
            row_addr = chw_row_addr(n_idx, c_idx, in_h_i, in_w_i);
            tap_index = kh_idx * kernel_w_q + kw_idx;

            lane_req[i] = '0;
            lane_zero[i] = 1'b0;
            lane_req[i].valid =
                kernel_pattern_q[tap_index] &&
                (out_h_i < out_h_q) &&
                (out_w_i < out_w_q) &&
                !is_padding &&
                ((w_q > BLOCK_SIZE) || (local_h < rows_per_word)) &&
                (c_idx < c_q);
            lane_zero[i] =
                kernel_pattern_q[tap_index] &&
                (out_h_i < out_h_q) &&
                (out_w_i < out_w_q) &&
                is_padding &&
                ((w_q > BLOCK_SIZE) || (local_h < rows_per_word)) &&
                (c_idx < c_q);
            lane_req[i].bank = chw_lane_sel(in_h_i, in_w_i);
            lane_req[i].row = sp_row(row_addr);
            lane_req[i].lane_sel = chw_lane_sel(in_h_i, in_w_i);
            lane_req[i].dst_lane = i[LANE_W-1:0];

            dbg_out_h[i] = out_h_i;
            dbg_out_w[i] = out_w_i;
            dbg_padded_h[i] = padded_h_i;
            dbg_padded_w[i] = padded_w_i;
            dbg_real_h[i] = real_h_i;
            dbg_real_w[i] = real_w_i;
            dbg_local_h[i] = local_h;
            dbg_local_w[i] = local_w;
            dbg_row_addr[i] = row_addr;
            dbg_bank[i] = lane_req[i].bank;
            dbg_row[i] = lane_req[i].row;
            dbg_lane_sel[i] = lane_req[i].lane_sel;
            dbg_tap_index[i] = tap_index;
            dbg_is_padding[i] = is_padding;
            dbg_lane_req_valid[i] = lane_req[i].valid;
            dbg_lane_zero[i] = lane_zero[i];
        end
    end

    always_comb begin
        dbg_collect_all_done = 1'b1;
        for (int i = 0; i < BLOCK_SIZE; i++) begin
            if (lane_req_q[i].valid && !lane_done[i]) begin
                dbg_collect_all_done = 1'b0;
            end
        end
    end

    // One read per int8 bank per cycle. The 16-byte logical row is striped
    // across 16 banks, so lane_sel is also the physical bank id. If stride or
    // dilation maps several destination lanes to the same bank but different
    // rows, this scheduler takes multiple cycles to collect the full vector.
    always_comb begin
        sram_req_valid = '0;
        sram_req_addr = '0;

        for (int i = 0; i < BLOCK_SIZE; i++) begin
            if (lane_req_q[i].valid && !lane_done[i]) begin
                if (!sram_req_valid[lane_req_q[i].bank]) begin
                    sram_req_valid[lane_req_q[i].bank] = 1'b1;
                    sram_req_addr[lane_req_q[i].bank] = lane_req_q[i].row;
                end
            end
        end
    end

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            fifo_count <= '0;
            fifo_rptr <= '0;
            fifo_wptr <= '0;
        end else begin
            unique case ({fifo_push, fifo_pop})
                2'b10: begin
                    fifo_data[fifo_wptr] <= interm_data;
                    fifo_mask[fifo_wptr] <= interm_valid;
                    fifo_wptr <= fifo_wptr + 1'b1;
                    fifo_count <= fifo_count + 1'b1;
                end
                2'b01: begin
                    fifo_rptr <= fifo_rptr + 1'b1;
                    fifo_count <= fifo_count - 1'b1;
                end
                2'b11: begin
                    fifo_data[fifo_wptr] <= interm_data;
                    fifo_mask[fifo_wptr] <= interm_valid;
                    fifo_wptr <= fifo_wptr + 1'b1;
                    fifo_rptr <= fifo_rptr + 1'b1;
                end
                default: begin end
            endcase
        end
    end

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state <= ST_IDLE;
            done <= 1'b0;
            n_idx <= '0;
            c_idx <= '0;
            oh_idx <= '0;
            ow_base <= '0;
            kh_idx <= '0;
            kw_idx <= '0;
            lane_done <= '0;
            interm_data <= '0;
            interm_valid <= '0;
            for (int i = 0; i < BLOCK_SIZE; i++) begin
                lane_req_q[i] <= '0;
            end
        end else begin
            done <= 1'b0;

            unique case (state)
                ST_IDLE: begin
                    if (start) begin
                        n_idx <= '0;
                        c_idx <= '0;
                        oh_idx <= '0;
                        ow_base <= '0;
                        kh_idx <= '0;
                        kw_idx <= '0;
                        state <= ST_ISSUE;
                    end
                end

                ST_ISSUE: begin
                    lane_done <= '0;
                    interm_data <= '0;
                    interm_valid <= '0;
                    for (int i = 0; i < BLOCK_SIZE; i++) begin
                        lane_req_q[i] <= lane_req[i];
                        if (lane_zero[i]) begin
                            interm_valid[i] <= 1'b1;
                        end
                    end
                    state <= ST_COLLECT;
                end

                ST_COLLECT: begin : collect_block
                    logic all_done;

                    for (int b = 0; b < SP_BANKS; b++) begin
                        if (sram_resp_valid[b]) begin
                            for (int i = 0; i < BLOCK_SIZE; i++) begin
                                if (lane_req_q[i].valid &&
                                    !lane_done[i] &&
                                    lane_req_q[i].bank == b &&
                                    lane_req_q[i].row == sram_req_addr[b]) begin
                                    interm_data[lane_req_q[i].dst_lane*ELEM_W +: ELEM_W] <=
                                        sram_resp_data[b];
                                    interm_valid[lane_req_q[i].dst_lane] <= 1'b1;
                                    lane_done[i] <= 1'b1;
                                end
                            end
                        end
                    end

                    all_done = 1'b1;
                    for (int i = 0; i < BLOCK_SIZE; i++) begin
                        if (lane_req_q[i].valid && !lane_done[i]) begin
                            all_done = 1'b0;
                        end
                    end

                    if (all_done) begin
                        state <= ST_PUSH;
                    end
                end

                ST_PUSH: begin
                    if (fifo_count != FIFO_DEPTH) begin
                        state <= ST_NEXT;
                    end
                end

                ST_NEXT: begin : next_block
                    logic last_tile;
                    last_tile = 1'b0;

                    if (kw_idx + 1 < kernel_w_q) begin
                        kw_idx <= kw_idx + 1'b1;
                    end else begin
                        kw_idx <= '0;
                        if (kh_idx + 1 < kernel_h_q) begin
                            kh_idx <= kh_idx + 1'b1;
                        end else begin
                            kh_idx <= '0;
                            if (!dw_mode_q && c_idx + 1 < c_q) begin
                                c_idx <= c_idx + 1'b1;
                            end else begin
                                c_idx <= '0;
                                if (w_q > BLOCK_SIZE && ow_base + BLOCK_SIZE < out_w_q) begin
                                    ow_base <= ow_base + BLOCK_SIZE;
                                end else begin
                                    ow_base <= '0;
                                    if (w_q <= BLOCK_SIZE &&
                                        oh_idx + rows_per_word[15:0] < out_h_q) begin
                                        oh_idx <= oh_idx + rows_per_word[15:0];
                                    end else if (w_q > BLOCK_SIZE &&
                                                 oh_idx + 1 < out_h_q) begin
                                        oh_idx <= oh_idx + 1'b1;
                                    end else begin
                                        oh_idx <= '0;
                                        if (n_idx + 1 < n_q) begin
                                            n_idx <= n_idx + 1'b1;
                                        end else begin
                                            last_tile = 1'b1;
                                        end
                                    end
                                end
                            end
                        end
                    end

                    state <= last_tile ? ST_DONE : ST_ISSUE;
                end

                ST_DONE: begin
                    done <= 1'b1;
                    state <= ST_IDLE;
                end

                default: state <= ST_IDLE;
            endcase
        end
    end

endmodule

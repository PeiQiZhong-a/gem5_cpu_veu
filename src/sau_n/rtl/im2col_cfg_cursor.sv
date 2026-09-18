// Configuration snapshot and traversal cursor for Im2Col.
// The registers and update conditions mirror the original top-level logic.

module im2col_cfg_cursor #(
    parameter int BLOCK_SIZE = 16,
    parameter int SP_BANKS = BLOCK_SIZE,
    parameter int SP_BANK_ENTRIES = 4096,
    parameter int SP_ADDR_BITS = $clog2(SP_BANKS) + $clog2(SP_BANK_ENTRIES),
    parameter int KERNEL_PATTERN_BITS = 32,
    parameter int GROUP_W = 13,
    parameter int MAX_SEG_C = 16
) (
    input logic clk,
    input logic rst_n,
    input logic cfg_valid,
    input logic [SP_ADDR_BITS-1:0] cfg_spad_base,
    input logic [15:0] cfg_n,
    input logic [15:0] cfg_c,
    input logic [15:0] cfg_c_base,
    input logic [15:0] cfg_c_count,
    input logic cfg_single_group_mode,
    input logic [15:0] cfg_group_n,
    input logic [GROUP_W-1:0] cfg_group_index,
    input logic [15:0] cfg_group_oh_base,
    input logic [15:0] cfg_group_ow_base,
    input logic [15:0] cfg_h,
    input logic [15:0] cfg_w,
    input logic [15:0] cfg_out_h,
    input logic [15:0] cfg_out_w,
    input logic cfg_mline_mode,
    input logic [4:0] cfg_rows_per_group,
    input logic [3:0] cfg_kernel_h,
    input logic [3:0] cfg_kernel_w,
    input logic [3:0] cfg_stride_h,
    input logic [3:0] cfg_stride_w,
    input logic [3:0] cfg_dilation_h,
    input logic [3:0] cfg_dilation_w,
    input logic [15:0] cfg_pad_top,
    input logic [15:0] cfg_pad_left,
    input logic [KERNEL_PATTERN_BITS-1:0] cfg_kernel_pattern,
    input logic [GROUP_W-1:0] cfg_groups_per_n,
    input logic start_accept,
    input logic cursor_step,

    output logic [SP_ADDR_BITS-1:0] spad_base_q,
    output logic [15:0] n_q, c_q, c_base_q, c_count_q, h_q, w_q, out_h_q, out_w_q,
    output logic [3:0] kernel_h_q, kernel_w_q,
    output logic [3:0] stride_h_q, stride_w_q,
    output logic [3:0] dilation_h_q, dilation_w_q,
    output logic [15:0] pad_top_q, pad_left_q,
    output logic [KERNEL_PATTERN_BITS-1:0] kernel_pattern_q,
    output logic single_group_mode_q,
    output logic [15:0] group_n_q,
    output logic [GROUP_W-1:0] group_config_index_q,
    output logic [15:0] n_idx, c_idx, oh_idx, ow_base,
    output logic [GROUP_W-1:0] group_idx,
    output logic [3:0] kh_idx, kw_idx,
    output logic mline_mode_q,
    output logic [4:0] rows_per_group_q,
    output logic [31:0] channel_byte_stride,
    output logic [31:0] batch_byte_stride,
    output logic config_captured,
    output logic [GROUP_W-1:0] groups_per_n,
    output logic cursor_last,
    output logic producer_active
);

    logic [GROUP_W-1:0] groups_per_n_q;
    logic [15:0] single_group_oh_q;
    logic [15:0] single_group_ow_base_q;
    logic [31:0] channel_byte_stride_q;
    logic [31:0] batch_byte_stride_q;
    logic config_captured_q;

    assign channel_byte_stride = channel_byte_stride_q;
    assign batch_byte_stride = batch_byte_stride_q;
    assign groups_per_n = groups_per_n_q;
    assign config_captured = config_captured_q;

    assign cursor_last =
        (kw_idx + 1 >= kernel_w_q) &&
        (kh_idx + 1 >= kernel_h_q) &&
        (c_idx + 1 >= c_count_q) &&
        (single_group_mode_q ||
         ((group_idx + 1 >= groups_per_n_q) &&
          (n_idx + 1 >= n_q)));

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            spad_base_q <= '0;
            n_q <= '0;
            c_q <= '0;
            c_base_q <= '0;
            c_count_q <= '0;
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
            kernel_pattern_q <= '0;
            single_group_mode_q <= 1'b0;
            group_n_q <= '0;
            group_config_index_q <= '0;
            groups_per_n_q <= '0;
            mline_mode_q <= 1'b0;
            rows_per_group_q <= '0;
            single_group_oh_q <= '0;
            single_group_ow_base_q <= '0;
            channel_byte_stride_q <= '0;
            batch_byte_stride_q <= '0;
            config_captured_q <= 1'b0;
        end else if (cfg_valid) begin
            spad_base_q <= cfg_spad_base;
            n_q <= cfg_n;
            c_q <= cfg_c;
            c_base_q <= cfg_c_base;
            c_count_q <= cfg_c_count;
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
            kernel_pattern_q <= cfg_kernel_pattern;
            single_group_mode_q <= cfg_single_group_mode;
            group_n_q <= cfg_group_n;
            group_config_index_q <= cfg_group_index;
            groups_per_n_q <= cfg_groups_per_n;
            mline_mode_q <= cfg_mline_mode;
            rows_per_group_q <= cfg_rows_per_group;
            channel_byte_stride_q <= {16'd0, cfg_h} * {16'd0, cfg_w};
            batch_byte_stride_q <= {16'd0, cfg_c} *
                                  ({16'd0, cfg_h} * {16'd0, cfg_w});
            config_captured_q <= 1'b1;
            if (cfg_single_group_mode) begin
                single_group_oh_q <= cfg_group_oh_base;
                single_group_ow_base_q <= cfg_group_ow_base;
            end else begin
                single_group_oh_q <= '0;
                single_group_ow_base_q <= '0;
            end
        end
    end

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            producer_active <= 1'b0;
            n_idx <= '0;
            c_idx <= '0;
            oh_idx <= '0;
            ow_base <= '0;
            group_idx <= '0;
            kh_idx <= '0;
            kw_idx <= '0;
        end else begin
            if (start_accept) begin
                producer_active <= 1'b1;
                n_idx <= single_group_mode_q ? group_n_q : 16'd0;
                c_idx <= '0;
                if (single_group_mode_q) begin
                    oh_idx <= single_group_oh_q;
                    ow_base <= single_group_ow_base_q;
                end else begin
                    oh_idx <= '0;
                    ow_base <= '0;
                end
                group_idx <= single_group_mode_q ? group_config_index_q : '0;
                kh_idx <= '0;
                kw_idx <= '0;
            end

            if (cursor_step) begin
                if (cursor_last) begin
                    producer_active <= 1'b0;
                end else if (kw_idx + 1 < kernel_w_q) begin
                    kw_idx <= kw_idx + 1'b1;
                end else begin
                    kw_idx <= '0;
                    if (kh_idx + 1 < kernel_h_q) begin
                        kh_idx <= kh_idx + 1'b1;
                    end else begin
                        kh_idx <= '0;
                        if (c_idx + 1 < c_count_q) begin
                            c_idx <= c_idx + 1'b1;
                        end else begin
                            c_idx <= '0;
                            if (mline_mode_q) begin
                                if (group_idx + 1 < groups_per_n_q) begin
                                    oh_idx <= oh_idx +
                                              {11'd0, rows_per_group_q};
                                    group_idx <= group_idx + 1'b1;
                                end else begin
                                    oh_idx <= '0;
                                    n_idx <= n_idx + 1'b1;
                                    group_idx <= '0;
                                end
                                ow_base <= '0;
                            end else if (ow_base + BLOCK_SIZE < out_w_q) begin
                                ow_base <= ow_base + BLOCK_SIZE;
                                group_idx <= group_idx + 1'b1;
                            end else if (group_idx + 1 < groups_per_n_q) begin
                                ow_base <= '0;
                                oh_idx <= oh_idx + 1'b1;
                                group_idx <= group_idx + 1'b1;
                            end else begin
                                oh_idx <= '0;
                                ow_base <= '0;
                                n_idx <= n_idx + 1'b1;
                                group_idx <= '0;
                            end
                        end
                    end
                end
            end
        end
    end

endmodule

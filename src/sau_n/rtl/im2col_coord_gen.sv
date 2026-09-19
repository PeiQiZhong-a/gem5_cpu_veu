// Combinational coordinate stages for Im2Col.

module im2col_coord_stage0 #(
    parameter int BLOCK_SIZE = 16,
    parameter int KERNEL_PATTERN_BITS = 32
) (
    input logic [15:0] w_q,
    input logic mline_mode_q,
    input logic [15:0] oh_idx,
    input logic [15:0] ow_base,
    input logic [3:0] kh_idx,
    input logic [3:0] kw_idx,
    input logic [3:0] kernel_w_q,
    input logic [4:0] rows_per_group_q,
    output logic [BLOCK_SIZE*16-1:0] lane_out_h,
    output logic [BLOCK_SIZE*16-1:0] lane_out_w,
    output logic [BLOCK_SIZE-1:0] lane_row_eligible,
    output logic [$clog2(KERNEL_PATTERN_BITS)-1:0] tap_index
);
    function automatic [31:0] lane_div_w(input int lane, input logic [15:0] w);
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

    function automatic [31:0] lane_mod_w(input int lane, input logic [15:0] w);
        logic [31:0] div_value;
        begin
            div_value = lane_div_w(lane, w);
            unique case (w)
                16'd0, 16'd1: lane_mod_w = 32'd0;
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

    always_comb begin
        lane_out_h = '0;
        lane_out_w = '0;
        lane_row_eligible = '0;
        tap_index = {4'd0, kh_idx} * {4'd0, kernel_w_q} + {4'd0, kw_idx};
        for (int i = 0; i < BLOCK_SIZE; i++) begin
            logic [31:0] local_h;
            logic [31:0] local_w;
            logic [15:0] out_h_i;
            logic [15:0] out_w_i;

            if (mline_mode_q) begin
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
            lane_out_h[i*16 +: 16] = out_h_i;
            lane_out_w[i*16 +: 16] = out_w_i;
            lane_row_eligible[i] = !mline_mode_q ||
                (local_h < {27'd0, rows_per_group_q});
        end
    end
endmodule

module im2col_coord_gen #(
    parameter int BLOCK_SIZE = 16,
    parameter int KERNEL_PATTERN_BITS = 32,
    parameter int SPATIAL_W = 32
) (
    input logic [15:0] c_count_q, h_q, w_q, out_h_q, out_w_q,
    input logic [3:0] stride_h_q, stride_w_q,
    input logic [3:0] dilation_h_q, dilation_w_q,
    input logic [15:0] pad_top_q, pad_left_q,
    input logic [KERNEL_PATTERN_BITS-1:0] kernel_pattern_q,
    input logic [15:0] c_idx,
    input logic [3:0] stage0_kh_idx, stage0_kw_idx,
    input logic [BLOCK_SIZE*16-1:0] stage0_out_h,
    input logic [BLOCK_SIZE*16-1:0] stage0_out_w,
    input logic [BLOCK_SIZE-1:0] stage0_row_eligible,
    input logic [$clog2(KERNEL_PATTERN_BITS)-1:0] stage0_tap_index,

    // Intermediate fields intended for the elastic Coord register. Keeping
    // these fields together lets G0 perform only the final cheap combines.
    output logic [BLOCK_SIZE*SPATIAL_W-1:0] lane_spatial_base,
    output logic [BLOCK_SIZE-1:0] lane_tap_active,
    output logic [BLOCK_SIZE-1:0] lane_boundary_valid,
    output logic [BLOCK_SIZE-1:0] lane_row_eligible,
    output logic [BLOCK_SIZE-1:0] lane_c_valid,
    output logic [BLOCK_SIZE-1:0] lane_is_padding,
    // Geometry payload for the G0 register. These coordinates remain
    // deterministic for padding lanes; downstream validity controls whether
    // A1 may turn them into an SRAM request.
    output logic [BLOCK_SIZE*16-1:0] lane_in_h,
    output logic [BLOCK_SIZE*16-1:0] lane_in_w
);

    always_comb begin
        lane_spatial_base = '0;
        lane_tap_active = '0;
        lane_boundary_valid = '0;
        lane_row_eligible = '0;
        lane_c_valid = '0;
        lane_is_padding = '0;
        lane_in_h = '0;
        lane_in_w = '0;

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
            logic tap_active;
            logic boundary_valid;
            logic c_valid;

            out_h_i = stage0_out_h[i*16 +: 16];
            out_w_i = stage0_out_w[i*16 +: 16];
            padded_h_i = out_h_i * stride_h_q + stage0_kh_idx * dilation_h_q;
            padded_w_i = out_w_i * stride_w_q + stage0_kw_idx * dilation_w_q;
            real_h_i = $signed({1'b0, padded_h_i}) - $signed({1'b0, pad_top_q});
            real_w_i = $signed({1'b0, padded_w_i}) - $signed({1'b0, pad_left_q});
            is_padding = (real_h_i < 0) || (real_w_i < 0) ||
                (real_h_i >= $signed({1'b0, h_q})) ||
                (real_w_i >= $signed({1'b0, w_q}));
            tap_active = kernel_pattern_q[stage0_tap_index];
            boundary_valid = (out_h_i < out_h_q) && (out_w_i < out_w_q);
            c_valid = c_idx < c_count_q;

            in_h_i = real_h_i[15:0];
            in_w_i = real_w_i[15:0];
            lane_in_h[i*16 +: 16] = in_h_i;
            lane_in_w[i*16 +: 16] = in_w_i;

            lane_spatial_base[i*SPATIAL_W +: SPATIAL_W] =
                out_h_i * {16'd0, out_w_q};
            lane_tap_active[i] = tap_active;
            lane_boundary_valid[i] = boundary_valid;
            lane_row_eligible[i] = stage0_row_eligible[i];
            lane_c_valid[i] = c_valid;
            lane_is_padding[i] = is_padding;
        end
    end

endmodule

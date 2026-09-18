// Postprocess one raw accumulator column and write dense INT8 NCHW results.
//
// Each accepted result token represents one global OC and up to 16 explicitly
// tagged output spatial positions.  The module adds the current tile's signed
// INT16 bias with INT24 saturation, performs an arithmetic right shift, clamps
// to signed INT8, and schedules at most one write per Scratchpad bank per cycle.
module sau_nchw_writeback #(
    parameter int BLOCK_SIZE = 16,
    parameter int ACC_W = 24,
    parameter int BIAS_W = 16,
    parameter int ELEM_W = 8,
    parameter int SP_BANKS = BLOCK_SIZE,
    parameter int SP_BANK_ENTRIES = 4096,
    parameter int SP_BANK_BITS = $clog2(SP_BANKS),
    parameter int SP_ROW_BITS = $clog2(SP_BANK_ENTRIES),
    parameter int SP_ADDR_BITS = SP_BANK_BITS + SP_ROW_BITS,
    parameter int GROUP_W = 13,
    parameter int SPATIAL_W = 32,
    parameter int LANE_W = $clog2(BLOCK_SIZE)
) (
    input  logic clk,
    input  logic rst_n,

    input  logic cfg_valid,
    input  logic [SP_ADDR_BITS-1:0] cfg_output_base,
    input  logic [15:0] cfg_n,
    input  logic [15:0] cfg_oc,
    input  logic [15:0] cfg_tile_width,
    input  logic [15:0] cfg_out_h,
    input  logic [15:0] cfg_out_w,
    input  logic [4:0] cfg_cutbit,
    // Compatibility-only legacy status. Configuration legality is software-owned.
    output logic cfg_error,

    input  logic result_valid,
    output logic result_ready,
    input  logic [BLOCK_SIZE*ACC_W-1:0] result_acc_data,
    input  logic [LANE_W-1:0] result_oc_lane,
    input  logic [BLOCK_SIZE-1:0] result_row_mask,
    input  logic [BLOCK_SIZE*SPATIAL_W-1:0] result_spatial_index,
    input  logic [15:0] result_n_index,
    input  logic [GROUP_W-1:0] result_group_index,
    input  logic [15:0] result_oc_tile,
    input  logic result_last_column,
    input  logic result_last_command,

    input  logic [BLOCK_SIZE*BIAS_W-1:0] bias_data,
    input  logic [BLOCK_SIZE-1:0] bias_mask,
    input  logic [15:0] bias_oc_tile,

    output logic [SP_BANKS-1:0] spad_wr_valid,
    input  logic [SP_BANKS-1:0] spad_wr_ready,
    output logic [SP_BANKS-1:0][SP_ROW_BITS-1:0] spad_wr_addr,
    output logic [SP_BANKS-1:0][ELEM_W-1:0] spad_wr_data,

    output logic busy,
    output logic done,
    output logic protocol_error
);
    typedef enum logic {
        S_IDLE,
        S_WRITE
    } state_t;

    state_t state;
    logic config_loaded;
    logic [SP_ADDR_BITS-1:0] output_base_q;
    logic [15:0] n_q, oc_q;
    logic [15:0] tile_width_q;
    logic [4:0] cutbit_q;
    logic [31:0] output_spatial_count_q;

    logic [BLOCK_SIZE-1:0] pending_q;
    logic [SP_BANK_BITS-1:0] wr_bank_q [BLOCK_SIZE];
    logic [SP_ROW_BITS-1:0] wr_row_q [BLOCK_SIZE];
    logic [ELEM_W-1:0] wr_data_q [BLOCK_SIZE];
    logic [BLOCK_SIZE-1:0] bank_candidate_mask [SP_BANKS];
    logic [BLOCK_SIZE-1:0] bank_grant_onehot [SP_BANKS];
    logic [SP_BANKS-1:0] bank_grant_valid;
    logic [SP_BANKS-1:0] bank_accepted;
    logic [BLOCK_SIZE-1:0] accepted_group [4];
    logic [BLOCK_SIZE-1:0] accepted_row;
    logic [BLOCK_SIZE-1:0] pending_after_accept;
    logic last_command_q;

    logic [31:0] global_oc;
    logic result_protocol_valid;

    assign cfg_error = 1'b0;

    function automatic logic signed [ACC_W-1:0] sat_bias_add(
        input logic signed [ACC_W-1:0] acc,
        input logic signed [BIAS_W-1:0] bias
    );
        logic signed [ACC_W:0] sum_ext;
        logic signed [ACC_W:0] max_ext;
        logic signed [ACC_W:0] min_ext;
        begin
            sum_ext = $signed({acc[ACC_W-1], acc}) +
                      $signed({{(ACC_W + 1 - BIAS_W){bias[BIAS_W-1]}}, bias});
            max_ext = $signed({2'b00, {(ACC_W-1){1'b1}}});
            min_ext = $signed({2'b11, {(ACC_W-1){1'b0}}});
            if (sum_ext > max_ext)
                sat_bias_add = {1'b0, {(ACC_W-1){1'b1}}};
            else if (sum_ext < min_ext)
                sat_bias_add = {1'b1, {(ACC_W-1){1'b0}}};
            else
                sat_bias_add = sum_ext[ACC_W-1:0];
        end
    endfunction

    function automatic logic signed [ELEM_W-1:0] quantize_int8(
        input logic signed [ACC_W-1:0] value,
        input logic [4:0] shift
    );
        logic signed [ACC_W-1:0] shifted;
        begin
            shifted = $signed(value) >>> shift;
            if (shifted > 127)
                quantize_int8 = 8'sh7f;
            else if (shifted < -128)
                quantize_int8 = -8'sd128;
            else
                quantize_int8 = shifted[ELEM_W-1:0];
        end
    endfunction

    // Four independent 4-row arbiters feed a 4-way lowest-group selector.
    // This preserves lowest-row priority without a row-to-row feedback chain.
    function automatic logic [BLOCK_SIZE-1:0] lowest_row_onehot(
        input logic [BLOCK_SIZE-1:0] candidates
    );
        logic [3:0] local_winner [4];
        logic [3:0] group_nonempty;
        logic [1:0] selected_group;
        logic selected_valid;
        begin
            local_winner[0] = 4'b0;
            local_winner[1] = 4'b0;
            local_winner[2] = 4'b0;
            local_winner[3] = 4'b0;
            casez (candidates[3:0])
                4'b???1: local_winner[0] = 4'b0001;
                4'b??10: local_winner[0] = 4'b0010;
                4'b?100: local_winner[0] = 4'b0100;
                4'b1000: local_winner[0] = 4'b1000;
                default: local_winner[0] = 4'b0;
            endcase
            casez (candidates[7:4])
                4'b???1: local_winner[1] = 4'b0001;
                4'b??10: local_winner[1] = 4'b0010;
                4'b?100: local_winner[1] = 4'b0100;
                4'b1000: local_winner[1] = 4'b1000;
                default: local_winner[1] = 4'b0;
            endcase
            casez (candidates[11:8])
                4'b???1: local_winner[2] = 4'b0001;
                4'b??10: local_winner[2] = 4'b0010;
                4'b?100: local_winner[2] = 4'b0100;
                4'b1000: local_winner[2] = 4'b1000;
                default: local_winner[2] = 4'b0;
            endcase
            casez (candidates[15:12])
                4'b???1: local_winner[3] = 4'b0001;
                4'b??10: local_winner[3] = 4'b0010;
                4'b?100: local_winner[3] = 4'b0100;
                4'b1000: local_winner[3] = 4'b1000;
                default: local_winner[3] = 4'b0;
            endcase

            group_nonempty = {
                |candidates[15:12], |candidates[11:8],
                |candidates[7:4], |candidates[3:0]
            };
            selected_valid = 1'b1;
            casez (group_nonempty)
                4'b???1: selected_group = 2'd0;
                4'b??10: selected_group = 2'd1;
                4'b?100: selected_group = 2'd2;
                4'b1000: selected_group = 2'd3;
                default: begin
                    selected_group = 2'd0;
                    selected_valid = 1'b0;
                end
            endcase

            lowest_row_onehot = '0;
            if (selected_valid) begin
                case (selected_group)
                    2'd0: lowest_row_onehot[3:0] = local_winner[0];
                    2'd1: lowest_row_onehot[7:4] = local_winner[1];
                    2'd2: lowest_row_onehot[11:8] = local_winner[2];
                    default: lowest_row_onehot[15:12] = local_winner[3];
                endcase
            end
        end
    endfunction

    assign global_oc = {16'd0, result_oc_tile} * tile_width_q + result_oc_lane;

    always_comb begin
        result_protocol_valid = config_loaded && result_row_mask != '0 &&
                          result_n_index < n_q && global_oc < {16'd0, oc_q} &&
                          result_oc_tile == bias_oc_tile &&
                          bias_mask[result_oc_lane] &&
                          (!result_last_command || result_last_column);
        for (int row = 0; row < BLOCK_SIZE; row++) begin
            if (result_row_mask[row] &&
                result_spatial_index[row*SPATIAL_W +: SPATIAL_W] >=
                    output_spatial_count_q)
                result_protocol_valid = 1'b0;
        end
    end

    assign busy = state == S_WRITE;
    assign result_ready = state == S_IDLE && config_loaded;

    always_comb begin
        for (int bank = 0; bank < SP_BANKS; bank++) begin
            bank_candidate_mask[bank] = '0;
            for (int row = 0; row < BLOCK_SIZE; row++) begin
                bank_candidate_mask[bank][row] =
                    state == S_WRITE && pending_q[row] &&
                    (wr_bank_q[row] == bank);
            end
            bank_grant_onehot[bank] = lowest_row_onehot(bank_candidate_mask[bank]);
            bank_grant_valid[bank] = |bank_grant_onehot[bank];
            bank_accepted[bank] = bank_grant_valid[bank] && spad_wr_ready[bank];
        end

        spad_wr_valid = '0;
        spad_wr_addr = '0;
        spad_wr_data = '0;
        for (int bank = 0; bank < SP_BANKS; bank++) begin
            spad_wr_valid[bank] = bank_grant_valid[bank];
            for (int row = 0; row < BLOCK_SIZE; row++) begin
                if (bank_grant_onehot[bank][row]) begin
                    spad_wr_addr[bank] = wr_row_q[row];
                    spad_wr_data[bank] = wr_data_q[row];
                end
            end
        end

        accepted_group[0] = '0;
        accepted_group[1] = '0;
        accepted_group[2] = '0;
        accepted_group[3] = '0;
        for (int bank = 0; bank < 4; bank++)
            accepted_group[0] |= bank_accepted[bank] ? bank_grant_onehot[bank] : '0;
        for (int bank = 4; bank < 8; bank++)
            accepted_group[1] |= bank_accepted[bank] ? bank_grant_onehot[bank] : '0;
        for (int bank = 8; bank < 12; bank++)
            accepted_group[2] |= bank_accepted[bank] ? bank_grant_onehot[bank] : '0;
        for (int bank = 12; bank < 16; bank++)
            accepted_group[3] |= bank_accepted[bank] ? bank_grant_onehot[bank] : '0;
        accepted_row = accepted_group[0] | accepted_group[1] |
                       accepted_group[2] | accepted_group[3];
        pending_after_accept = pending_q & ~accepted_row;
    end

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state <= S_IDLE;
            config_loaded <= 1'b0;
            output_base_q <= '0;
            n_q <= '0;
            oc_q <= '0;
            tile_width_q <= BLOCK_SIZE;
            output_spatial_count_q <= '0;
            cutbit_q <= '0;
            protocol_error <= 1'b0;
            done <= 1'b0;
            pending_q <= '0;
            last_command_q <= 1'b0;
            for (int row = 0; row < BLOCK_SIZE; row++) begin
                wr_bank_q[row] <= '0;
                wr_row_q[row] <= '0;
                wr_data_q[row] <= '0;
            end
        end else begin
            protocol_error <= 1'b0;
            done <= 1'b0;

            if (cfg_valid && state == S_IDLE) begin
                output_base_q <= cfg_output_base;
                n_q <= cfg_n;
                oc_q <= cfg_oc;
                tile_width_q <= (cfg_tile_width != 0) ? cfg_tile_width : BLOCK_SIZE;
                output_spatial_count_q <= {16'd0, cfg_out_h} * {16'd0, cfg_out_w};
                cutbit_q <= cfg_cutbit;
                config_loaded <= 1'b1;
            end

            if (result_valid && result_ready) begin
                if (!result_protocol_valid) begin
                    protocol_error <= 1'b1;
                end else begin
                    logic signed [BIAS_W-1:0] selected_bias;
                    selected_bias = bias_data[result_oc_lane*BIAS_W +: BIAS_W];
                    pending_q <= result_row_mask;
                    last_command_q <= result_last_command;
                    for (int row = 0; row < BLOCK_SIZE; row++) begin
                        logic signed [ACC_W-1:0] acc_value;
                        logic signed [ACC_W-1:0] biased_value;
                        logic [63:0] byte_addr;
                        acc_value = result_acc_data[row*ACC_W +: ACC_W];
                        biased_value = sat_bias_add(acc_value, selected_bias);
                        byte_addr = {48'd0, output_base_q} +
                            (({48'd0, result_n_index} * {48'd0, oc_q} + global_oc) *
                             {32'd0, output_spatial_count_q}) +
                            result_spatial_index[row*SPATIAL_W +: SPATIAL_W];
                        wr_bank_q[row] <= byte_addr[SP_BANK_BITS-1:0];
                        wr_row_q[row] <= byte_addr[SP_ADDR_BITS-1:SP_BANK_BITS];
                        wr_data_q[row] <= quantize_int8(biased_value, cutbit_q);
                    end
                    state <= S_WRITE;
                end
            end

            if (state == S_WRITE) begin
                pending_q <= pending_after_accept;
                if (pending_after_accept == '0) begin
                    if (last_command_q)
                        done <= 1'b1;
                    state <= S_IDLE;
                end
            end
        end
    end

    logic _unused_group_index;
    assign _unused_group_index = ^result_group_index;
endmodule

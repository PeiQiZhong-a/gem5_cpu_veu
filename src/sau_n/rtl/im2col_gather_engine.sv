// s2/s3 SRAM gather engine for Im2Col.
//
// The state and response-owner updates mirror the original top-level logic.
// s2 and s3 remain one module because their owner lifetime and same-cycle
// handoff are coupled by the fixed one-cycle SRAM response contract.

module im2col_gather_engine #(
    parameter int BLOCK_SIZE = 16,
    parameter int ELEM_W = 8,
    parameter int SP_BANKS = BLOCK_SIZE,
    parameter int SP_BANK_BITS = $clog2(SP_BANKS),
    parameter int SP_ROW_BITS = $clog2(4096),
    parameter int SP_ADDR_BITS = SP_BANK_BITS + SP_ROW_BITS,
    parameter int GROUP_W = 13,
    parameter int SPATIAL_W = 32
) (
    input logic clk,
    input logic rst_n,
    input logic s1_valid,
    input logic s1_last,
    input logic [BLOCK_SIZE-1:0] s1_req_valid,
    input logic [BLOCK_SIZE*SP_BANK_BITS-1:0] s1_req_bank,
    input logic [BLOCK_SIZE*SP_ROW_BITS-1:0] s1_req_row,
    input logic [BLOCK_SIZE-1:0] s1_zero,
    input logic [BLOCK_SIZE-1:0] s1_row_mask,
    input logic [15:0] s1_n_index,
    input logic [GROUP_W-1:0] s1_group_index,
    input logic [BLOCK_SIZE*SPATIAL_W-1:0] s1_spatial_index,
    input logic [SP_BANKS-1:0] sram_resp_valid,
    input var logic [SP_BANKS-1:0][ELEM_W-1:0] sram_resp_data,
    input logic output_ready,

    output logic s1_ready,
    output logic [SP_BANKS-1:0] sram_req_valid,
    output logic [SP_BANKS-1:0][SP_ROW_BITS-1:0] sram_req_addr,
    output logic output_valid,
    output logic output_last,
    output logic [BLOCK_SIZE*ELEM_W-1:0] output_data,
    output logic [BLOCK_SIZE-1:0] output_mask,
    output logic [BLOCK_SIZE-1:0] output_row_mask,
    output logic [15:0] output_n_index,
    output logic [GROUP_W-1:0] output_group_index,
    output logic [BLOCK_SIZE*SPATIAL_W-1:0] output_spatial_index,
    output logic busy,
    output logic collect_done
);

    typedef struct packed {
        logic valid;
        logic [SP_BANK_BITS-1:0] bank;
        logic [SP_ROW_BITS-1:0] row;
        logic [SP_BANK_BITS-1:0] dst_lane;
    } lane_req_t;

    function automatic logic [BLOCK_SIZE-1:0] lowest_lane_onehot(
        input logic [BLOCK_SIZE-1:0] candidates
    );
        logic [BLOCK_SIZE-1:0] prefix_1;
        logic [BLOCK_SIZE-1:0] prefix_2;
        logic [BLOCK_SIZE-1:0] prefix_4;
        logic [BLOCK_SIZE-1:0] prefix_8;
        begin
            // The fixed 16-lane scheduler uses a four-level prefix tree rather
            // than the procedural first-match feedback chain.
            for (int i = 0; i < BLOCK_SIZE; i++) begin
                prefix_1[i] = candidates[i] |
                    ((i >= 1) ? candidates[i - 1] : 1'b0);
            end
            for (int i = 0; i < BLOCK_SIZE; i++) begin
                prefix_2[i] = prefix_1[i] |
                    ((i >= 2) ? prefix_1[i - 2] : 1'b0);
            end
            for (int i = 0; i < BLOCK_SIZE; i++) begin
                prefix_4[i] = prefix_2[i] |
                    ((i >= 4) ? prefix_2[i - 4] : 1'b0);
            end
            for (int i = 0; i < BLOCK_SIZE; i++) begin
                prefix_8[i] = prefix_4[i] |
                    ((i >= 8) ? prefix_4[i - 8] : 1'b0);
            end
            for (int i = 0; i < BLOCK_SIZE; i++) begin
                lowest_lane_onehot[i] = candidates[i] &
                    ((i >= 1) ? ~prefix_8[i - 1] : 1'b1);
            end
        end
    endfunction

    lane_req_t s1_req [BLOCK_SIZE];
    logic s2_valid, s2_last;
    lane_req_t s2_req [BLOCK_SIZE];
    logic [BLOCK_SIZE-1:0] s2_lane_done, s2_pending_lane;
    logic [BLOCK_SIZE-1:0] s2_unissued_lane;
    logic [BLOCK_SIZE*ELEM_W-1:0] s2_data;
    logic [BLOCK_SIZE-1:0] s2_mask, s2_row_mask;
    logic [15:0] s2_n_index;
    logic [GROUP_W-1:0] s2_group_index;
    logic [BLOCK_SIZE*SPATIAL_W-1:0] s2_spatial_index;
    logic [SP_BANKS-1:0] s2_bank_pending;
    logic [SP_BANKS-1:0][BLOCK_SIZE-1:0] s2_bank_dst_mask;
    logic [BLOCK_SIZE-1:0] s2_lane_done_after_resp, s2_pending_after_resp;
    logic [BLOCK_SIZE*ELEM_W-1:0] s2_data_after_resp;
    logic [SP_BANKS-1:0] s2_bank_pending_after_resp;
    logic [BLOCK_SIZE-1:0] s2_response_lane_mask;
    logic [BLOCK_SIZE*ELEM_W-1:0] s2_response_lane_data;
    logic [BLOCK_SIZE-1:0] s2_issue_lane;
    logic [SP_BANKS-1:0] s2_issue_bank_valid;
    logic [SP_BANKS-1:0][BLOCK_SIZE-1:0] s2_issue_bank_dst_mask;
    // Runtime-only mask update cone.  C/H remain global clears in the
    // sequential block; this array contains only the bank-local I/R update
    // priority (issue > response-clear > hold).
    logic [SP_BANKS-1:0][BLOCK_SIZE-1:0] s2_bank_dst_mask_runtime_next;
    logic [SP_BANKS-1:0] s2_bank_dst_mask_runtime_update;
    logic [BLOCK_SIZE-1:0] s2_lane_eligible;
    logic [BLOCK_SIZE-1:0] s2_unissued_after_issue;
    logic [BLOCK_SIZE-1:0] s2_pending_after_issue;
    logic [SP_BANKS-1:0][BLOCK_SIZE-1:0] s2_bank_lane_eligible;
    logic [SP_BANKS-1:0][BLOCK_SIZE-1:0] s2_bank_pick_lane;
    logic [SP_BANKS-1:0][BLOCK_SIZE-1:0][SP_ROW_BITS-1:0] s2_bank_pick_row;
    logic [SP_BANKS-1:0][BLOCK_SIZE-1:0][SP_ROW_BITS-1:0] s2_bank_pick_row_or1;
    logic [SP_BANKS-1:0][BLOCK_SIZE-1:0][SP_ROW_BITS-1:0] s2_bank_pick_row_or2;
    logic [SP_BANKS-1:0][BLOCK_SIZE-1:0][SP_ROW_BITS-1:0] s2_bank_pick_row_or4;
    logic [SP_BANKS-1:0][BLOCK_SIZE-1:0][SP_ROW_BITS-1:0] s2_bank_pick_row_or8;
    logic [SP_BANKS-1:0] s2_bank_candidate_valid;
    logic [SP_BANKS-1:0][SP_ROW_BITS-1:0] s2_bank_issue_row;
    logic [BLOCK_SIZE-1:0] s2_mask_after_issue;
    logic s2_complete_after_issue;
    logic s2_complete_after_issue_lane;
    logic [SP_BANKS-1:0] s2_bank_unissued_after_issue;

    logic s3_valid, s3_last;
    lane_req_t s3_req [BLOCK_SIZE];
    logic [BLOCK_SIZE-1:0] s3_pending_lane;
    logic [BLOCK_SIZE*ELEM_W-1:0] s3_data;
    logic [BLOCK_SIZE-1:0] s3_mask, s3_row_mask;
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
    logic s2_to_s3, s1_to_s2, s3_ready;

    always_comb begin
        for (int i = 0; i < BLOCK_SIZE; i++) begin
            s1_req[i] = '0;
            s1_req[i].valid = s1_req_valid[i];
            s1_req[i].bank = s1_req_bank[i*SP_BANK_BITS +: SP_BANK_BITS];
            s1_req[i].row = s1_req_row[i*SP_ROW_BITS +: SP_ROW_BITS];
            s1_req[i].dst_lane = i[SP_BANK_BITS-1:0];
        end
    end

    always_comb begin
        s2_response_lane_mask = '0;
        s2_response_lane_data = '0;
        for (int b = 0; b < SP_BANKS; b++) begin
            if (sram_resp_valid[b] && s2_bank_pending[b]) begin
                s2_response_lane_mask = s2_response_lane_mask | s2_bank_dst_mask[b];
                for (int i = 0; i < BLOCK_SIZE; i++)
                    s2_response_lane_data[i*ELEM_W +: ELEM_W] =
                        s2_response_lane_data[i*ELEM_W +: ELEM_W] |
                        (sram_resp_data[b] & {ELEM_W{s2_bank_dst_mask[b][i]}});
            end
        end
    end

    always_comb begin
        s2_lane_done_after_resp = s2_lane_done | s2_response_lane_mask;
        s2_pending_after_resp = s2_pending_lane & ~s2_response_lane_mask;
        s2_data_after_resp = s2_data;
        s2_bank_pending_after_resp = s2_bank_pending & ~sram_resp_valid;
        for (int i = 0; i < BLOCK_SIZE; i++)
            if (s2_response_lane_mask[i])
                s2_data_after_resp[i*ELEM_W +: ELEM_W] =
                    s2_response_lane_data[i*ELEM_W +: ELEM_W];
    end

    always_comb begin
        sram_req_valid = '0;
        sram_req_addr = '0;
        s2_issue_lane = '0;
        s2_lane_eligible = '0;
        s2_bank_lane_eligible = '0;
        s2_bank_pick_lane = '0;
        s2_bank_pick_row = '0;
        s2_bank_pick_row_or1 = '0;
        s2_bank_pick_row_or2 = '0;
        s2_bank_pick_row_or4 = '0;
        s2_bank_pick_row_or8 = '0;
        s2_bank_candidate_valid = '0;
        s2_bank_issue_row = '0;

        for (int i = 0; i < BLOCK_SIZE; i++) begin
            s2_lane_eligible[i] = s2_valid && s2_req[i].valid &&
                s2_unissued_lane[i];
            for (int b = 0; b < SP_BANKS; b++) begin
                if (s2_lane_eligible[i] && s2_req[i].bank == b)
                    s2_bank_lane_eligible[b][i] = 1'b1;
            end
        end
        for (int b = 0; b < SP_BANKS; b++) begin
            s2_bank_pick_lane[b] = lowest_lane_onehot(s2_bank_lane_eligible[b]);
            s2_bank_candidate_valid[b] = |s2_bank_pick_lane[b];
            for (int i = 0; i < BLOCK_SIZE; i++) begin
                s2_bank_pick_row[b][i] = s2_req[i].row &
                    {SP_ROW_BITS{s2_bank_pick_lane[b][i]}};
                s2_bank_pick_row_or1[b][i] = s2_bank_pick_row[b][i] |
                    ((i >= 1) ? s2_bank_pick_row[b][i - 1] : '0);
                s2_bank_pick_row_or2[b][i] = s2_bank_pick_row_or1[b][i] |
                    ((i >= 2) ? s2_bank_pick_row_or1[b][i - 2] : '0);
                s2_bank_pick_row_or4[b][i] = s2_bank_pick_row_or2[b][i] |
                    ((i >= 4) ? s2_bank_pick_row_or2[b][i - 4] : '0);
                s2_bank_pick_row_or8[b][i] = s2_bank_pick_row_or4[b][i] |
                    ((i >= 8) ? s2_bank_pick_row_or4[b][i - 8] : '0);
            end
            s2_bank_issue_row[b] = s2_bank_pick_row_or8[b][BLOCK_SIZE - 1];
        end
        for (int b = 0; b < SP_BANKS; b++) begin
            sram_req_valid[b] = s2_bank_candidate_valid[b];
            sram_req_addr[b] = s2_bank_issue_row[b];
            for (int i = 0; i < BLOCK_SIZE; i++) begin
                if (s2_bank_lane_eligible[b][i] &&
                    s2_req[i].row == s2_bank_issue_row[b])
                    s2_issue_lane[i] = 1'b1;
            end
        end

        s2_complete_after_issue_lane = s2_valid;
        s2_mask_after_issue = s2_mask;
        s2_unissued_after_issue = s2_unissued_lane & ~s2_issue_lane;
        s2_pending_after_issue = s2_pending_after_resp | s2_issue_lane;
        for (int i = 0; i < BLOCK_SIZE; i++) begin
            if (s2_req[i].valid && !s2_lane_done_after_resp[i] && !s2_issue_lane[i])
                s2_complete_after_issue_lane = 1'b0;
        end
    end

    always_comb begin
        s2_bank_unissued_after_issue = '0;
        for (int b = 0; b < SP_BANKS; b++) begin
            for (int i = 0; i < BLOCK_SIZE; i++) begin
                if (s2_bank_lane_eligible[b][i] &&
                    s2_req[i].row != s2_bank_issue_row[b])
                    s2_bank_unissued_after_issue[b] = 1'b1;
            end
        end
        s2_complete_after_issue = s2_valid &&
            (s2_unissued_after_issue == {BLOCK_SIZE{1'b0}}) &&
            (s2_pending_after_resp == {BLOCK_SIZE{1'b0}});
    end

`ifndef SYNTHESIS
    always_ff @(posedge clk) begin
        if (rst_n === 1'b1 &&
            s2_complete_after_issue !== s2_complete_after_issue_lane) begin
            $error("gather bank completion summary mismatch valid=%b old=%b new=%b issue=%h done=%h pending=%h unissued=%h",
                   s2_valid, s2_complete_after_issue_lane,
                   s2_complete_after_issue, s2_issue_lane,
                   s2_lane_done_after_resp, s2_pending_after_resp,
                   s2_bank_unissued_after_issue);
        end
    end
`endif

    always_comb begin
        s2_issue_bank_valid = s2_bank_candidate_valid;
        s2_issue_bank_dst_mask = '0;
        for (int b = 0; b < SP_BANKS; b++) begin
            for (int i = 0; i < BLOCK_SIZE; i++) begin
                if (s2_bank_lane_eligible[b][i] &&
                    s2_req[i].row == s2_bank_issue_row[b])
                    s2_issue_bank_dst_mask[b][i] = 1'b1;
            end
        end
    end

    // Keep the descriptor-capture/handoff clears out of this cone.  They are
    // intentionally applied to every bank by the sequential C/H branches
    // below.  Only issue and response-clear are bank-local here.
    always_comb begin
        s2_bank_dst_mask_runtime_next = s2_bank_dst_mask;
        s2_bank_dst_mask_runtime_update = '0;
        for (int b = 0; b < SP_BANKS; b++) begin
            if (s2_issue_bank_valid[b]) begin
                s2_bank_dst_mask_runtime_next[b] = s2_issue_bank_dst_mask[b];
                s2_bank_dst_mask_runtime_update[b] = 1'b1;
            end else if (sram_resp_valid[b] && s2_bank_pending[b]) begin
                s2_bank_dst_mask_runtime_next[b] = '0;
                s2_bank_dst_mask_runtime_update[b] = 1'b1;
            end
        end
    end

    always_comb begin
        s3_response_lane_mask = '0;
        s3_response_lane_data = '0;
        for (int b = 0; b < SP_BANKS; b++) begin
            if (sram_resp_valid[b] && s3_bank_pending[b]) begin
                s3_response_lane_mask = s3_response_lane_mask | s3_bank_dst_mask[b];
                for (int i = 0; i < BLOCK_SIZE; i++)
                    s3_response_lane_data[i*ELEM_W +: ELEM_W] =
                        s3_response_lane_data[i*ELEM_W +: ELEM_W] |
                        (sram_resp_data[b] & {ELEM_W{s3_bank_dst_mask[b][i]}});
            end
        end
    end

    always_comb begin
        s3_pending_after_resp = s3_pending_lane & ~s3_response_lane_mask;
        s3_data_after_resp = s3_data;
        s3_bank_pending_after_resp = s3_bank_pending & ~sram_resp_valid;
        for (int i = 0; i < BLOCK_SIZE; i++)
            if (s3_response_lane_mask[i])
                s3_data_after_resp[i*ELEM_W +: ELEM_W] =
                    s3_response_lane_data[i*ELEM_W +: ELEM_W];
        s3_complete_after_resp = s3_valid &&
            (s3_pending_after_resp == {BLOCK_SIZE{1'b0}});
    end

    assign output_valid = s3_complete_after_resp;
    assign output_last = s3_last;
    assign output_data = s3_data_after_resp;
    assign output_mask = s3_mask;
    assign output_row_mask = s3_row_mask;
    assign output_n_index = s3_n_index;
    assign output_group_index = s3_group_index;
    assign output_spatial_index = s3_spatial_index;
    assign s3_ready = !s3_valid || output_ready;
    assign s2_to_s3 = s2_complete_after_issue && s3_ready;
    assign s1_ready = !s2_valid || s2_to_s3;
    assign s1_to_s2 = s1_valid && s1_ready;
    assign busy = s1_valid || s2_valid || s3_valid;
    assign collect_done = s3_complete_after_resp;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            s2_valid <= 1'b0;
            s2_last <= 1'b0;
            s2_lane_done <= '0;
            s2_pending_lane <= '0;
            s2_unissued_lane <= '0;
            s2_data <= '0;
            s2_bank_pending <= '0;
            s2_bank_dst_mask <= '0;
            s2_mask <= '0;
            s2_row_mask <= '0;
            s2_n_index <= '0;
            s2_group_index <= '0;
            s2_spatial_index <= '0;
            s3_valid <= 1'b0;
            s3_last <= 1'b0;
            s3_pending_lane <= '0;
            s3_data <= '0;
            s3_bank_pending <= '0;
            s3_bank_dst_mask <= '0;
            s3_mask <= '0;
            s3_row_mask <= '0;
            s3_n_index <= '0;
            s3_group_index <= '0;
            s3_spatial_index <= '0;
            for (int i = 0; i < BLOCK_SIZE; i++) begin
                s2_req[i] <= '0;
                s3_req[i] <= '0;
            end
        end else begin
            if (s2_to_s3) begin
                s3_valid <= 1'b1;
                s3_last <= s2_last;
                s3_pending_lane <= s2_issue_lane;
                s3_data <= s2_data_after_resp;
                s3_bank_pending <= s2_issue_bank_valid;
                s3_bank_dst_mask <= s2_issue_bank_dst_mask;
                s3_mask <= s2_mask_after_issue;
                s3_row_mask <= s2_row_mask;
                s3_n_index <= s2_n_index;
                s3_group_index <= s2_group_index;
                s3_spatial_index <= s2_spatial_index;
                for (int i = 0; i < BLOCK_SIZE; i++) s3_req[i] <= s2_req[i];
            end else if (output_ready) begin
                s3_valid <= 1'b0;
                s3_bank_pending <= '0;
                s3_bank_dst_mask <= '0;
            end else if (s3_valid) begin
                s3_pending_lane <= s3_pending_after_resp;
                s3_data <= s3_data_after_resp;
                s3_bank_pending <= s3_bank_pending_after_resp;
            end

            if (s1_to_s2) begin
                s2_valid <= 1'b1;
                s2_last <= s1_last;
                s2_lane_done <= ~s1_req_valid;
                s2_pending_lane <= '0;
                s2_unissued_lane <= s1_req_valid;
                s2_data <= '0;
                s2_bank_pending <= '0;
                s2_bank_dst_mask <= '0;
                s2_mask <= s1_zero | s1_req_valid;
                s2_row_mask <= s1_row_mask;
                s2_n_index <= s1_n_index;
                s2_group_index <= s1_group_index;
                s2_spatial_index <= s1_spatial_index;
                for (int i = 0; i < BLOCK_SIZE; i++) s2_req[i] <= s1_req[i];
            end else if (s2_to_s3) begin
                s2_valid <= 1'b0;
                s2_bank_pending <= '0;
                s2_bank_dst_mask <= '0;
            end else if (s2_valid) begin
                s2_lane_done <= s2_lane_done_after_resp;
                s2_pending_lane <= s2_pending_after_issue;
                s2_unissued_lane <= s2_unissued_after_issue;
                s2_data <= s2_data_after_resp;
                s2_bank_pending <= s2_bank_pending_after_resp | s2_issue_bank_valid;
                for (int b = 0; b < SP_BANKS; b++) begin
                    if (s2_bank_dst_mask_runtime_update[b])
                        s2_bank_dst_mask[b] <= s2_bank_dst_mask_runtime_next[b];
                end
            end
        end
    end

endmodule

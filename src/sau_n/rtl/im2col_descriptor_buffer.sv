// Two-entry elastic buffer for complete Im2Col stage-1 descriptors.
//
// The input ready signal depends only on local occupancy.  This deliberately
// avoids feeding gather backpressure into the cursor's combinational advance
// decision.  A full buffer does not accept a same-cycle replacement when the
// head is popped; this conservative rule keeps the ready boundary registered
// by occupancy and permits a one-cycle recovery bubble after backpressure.

module im2col_descriptor_buffer #(
    parameter int BLOCK_SIZE = 16,
    parameter int SP_BANK_BITS = $clog2(BLOCK_SIZE),
    parameter int SP_ROW_BITS = $clog2(4096),
    parameter int GROUP_W = 13,
    parameter int SPATIAL_W = 32,
    parameter int DEPTH = 2,
    parameter int PTR_W = (DEPTH <= 1) ? 1 : $clog2(DEPTH),
    parameter int COUNT_W = $clog2(DEPTH + 1)
) (
    input logic clk,
    input logic rst_n,

    input logic in_valid,
    output logic in_ready,
    input logic in_last,
    input logic [BLOCK_SIZE-1:0] in_req_valid,
    input logic [BLOCK_SIZE*SP_BANK_BITS-1:0] in_req_bank,
    input logic [BLOCK_SIZE*SP_ROW_BITS-1:0] in_req_row,
    input logic [BLOCK_SIZE-1:0] in_zero,
    input logic [BLOCK_SIZE-1:0] in_row_mask,
    input logic [15:0] in_n_index,
    input logic [GROUP_W-1:0] in_group_index,
    input logic [BLOCK_SIZE*SPATIAL_W-1:0] in_spatial_index,

    output logic out_valid,
    input logic out_ready,
    output logic out_last,
    output logic [BLOCK_SIZE-1:0] out_req_valid,
    output logic [BLOCK_SIZE*SP_BANK_BITS-1:0] out_req_bank,
    output logic [BLOCK_SIZE*SP_ROW_BITS-1:0] out_req_row,
    output logic [BLOCK_SIZE-1:0] out_zero,
    output logic [BLOCK_SIZE-1:0] out_row_mask,
    output logic [15:0] out_n_index,
    output logic [GROUP_W-1:0] out_group_index,
    output logic [BLOCK_SIZE*SPATIAL_W-1:0] out_spatial_index,

    output logic [COUNT_W-1:0] occupancy
);

    logic [PTR_W-1:0] read_ptr;
    logic [PTR_W-1:0] write_ptr;
    logic in_fire;
    logic out_fire;

    logic fifo_last [DEPTH];
    logic [BLOCK_SIZE-1:0] fifo_req_valid [DEPTH];
    logic [BLOCK_SIZE*SP_BANK_BITS-1:0] fifo_req_bank [DEPTH];
    logic [BLOCK_SIZE*SP_ROW_BITS-1:0] fifo_req_row [DEPTH];
    logic [BLOCK_SIZE-1:0] fifo_zero [DEPTH];
    logic [BLOCK_SIZE-1:0] fifo_row_mask [DEPTH];
    logic [15:0] fifo_n_index [DEPTH];
    logic [GROUP_W-1:0] fifo_group_index [DEPTH];
    logic [BLOCK_SIZE*SPATIAL_W-1:0] fifo_spatial_index [DEPTH];

    function automatic [PTR_W-1:0] ptr_inc(input logic [PTR_W-1:0] ptr);
        begin
            if (ptr == DEPTH-1)
                ptr_inc = '0;
            else
                ptr_inc = ptr + 1'b1;
        end
    endfunction

    assign in_ready = (occupancy < DEPTH);
    assign out_valid = (occupancy != 0);
    assign in_fire = in_valid && in_ready;
    assign out_fire = out_valid && out_ready;

    assign out_last = fifo_last[read_ptr];
    assign out_req_valid = fifo_req_valid[read_ptr];
    assign out_req_bank = fifo_req_bank[read_ptr];
    assign out_req_row = fifo_req_row[read_ptr];
    assign out_zero = fifo_zero[read_ptr];
    assign out_row_mask = fifo_row_mask[read_ptr];
    assign out_n_index = fifo_n_index[read_ptr];
    assign out_group_index = fifo_group_index[read_ptr];
    assign out_spatial_index = fifo_spatial_index[read_ptr];

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            occupancy <= '0;
            read_ptr <= '0;
            write_ptr <= '0;
            for (int i = 0; i < DEPTH; i++) begin
                fifo_last[i] <= 1'b0;
                fifo_req_valid[i] <= '0;
                fifo_req_bank[i] <= '0;
                fifo_req_row[i] <= '0;
                fifo_zero[i] <= '0;
                fifo_row_mask[i] <= '0;
                fifo_n_index[i] <= '0;
                fifo_group_index[i] <= '0;
                fifo_spatial_index[i] <= '0;
            end
        end else begin
            unique case ({in_fire, out_fire})
                2'b10: begin
                    fifo_last[write_ptr] <= in_last;
                    fifo_req_valid[write_ptr] <= in_req_valid;
                    fifo_req_bank[write_ptr] <= in_req_bank;
                    fifo_req_row[write_ptr] <= in_req_row;
                    fifo_zero[write_ptr] <= in_zero;
                    fifo_row_mask[write_ptr] <= in_row_mask;
                    fifo_n_index[write_ptr] <= in_n_index;
                    fifo_group_index[write_ptr] <= in_group_index;
                    fifo_spatial_index[write_ptr] <= in_spatial_index;
                    write_ptr <= ptr_inc(write_ptr);
                    occupancy <= occupancy + 1'b1;
                end
                2'b01: begin
                    read_ptr <= ptr_inc(read_ptr);
                    occupancy <= occupancy - 1'b1;
                end
                2'b11: begin
                    fifo_last[write_ptr] <= in_last;
                    fifo_req_valid[write_ptr] <= in_req_valid;
                    fifo_req_bank[write_ptr] <= in_req_bank;
                    fifo_req_row[write_ptr] <= in_req_row;
                    fifo_zero[write_ptr] <= in_zero;
                    fifo_row_mask[write_ptr] <= in_row_mask;
                    fifo_n_index[write_ptr] <= in_n_index;
                    fifo_group_index[write_ptr] <= in_group_index;
                    fifo_spatial_index[write_ptr] <= in_spatial_index;
                    write_ptr <= ptr_inc(write_ptr);
                    read_ptr <= ptr_inc(read_ptr);
                end
                default: begin
                end
            endcase
        end
    end

`ifndef SYNTHESIS
    logic stall_prev;
    logic input_stall_prev;
    logic hold_last;
    logic [BLOCK_SIZE-1:0] hold_req_valid;
    logic [BLOCK_SIZE*SP_BANK_BITS-1:0] hold_req_bank;
    logic [BLOCK_SIZE*SP_ROW_BITS-1:0] hold_req_row;
    logic [BLOCK_SIZE-1:0] hold_zero;
    logic [BLOCK_SIZE-1:0] hold_row_mask;
    logic [15:0] hold_n_index;
    logic [GROUP_W-1:0] hold_group_index;
    logic [BLOCK_SIZE*SPATIAL_W-1:0] hold_spatial_index;
    logic hold_in_last;
    logic [BLOCK_SIZE-1:0] hold_in_req_valid;
    logic [BLOCK_SIZE*SP_BANK_BITS-1:0] hold_in_req_bank;
    logic [BLOCK_SIZE*SP_ROW_BITS-1:0] hold_in_req_row;
    logic [BLOCK_SIZE-1:0] hold_in_zero;
    logic [BLOCK_SIZE-1:0] hold_in_row_mask;
    logic [15:0] hold_in_n_index;
    logic [GROUP_W-1:0] hold_in_group_index;
    logic [BLOCK_SIZE*SPATIAL_W-1:0] hold_in_spatial_index;

    always_ff @(posedge clk) begin
        if (rst_n === 1'b1) begin
            assert (occupancy <= DEPTH)
                else $error("descriptor buffer occupancy overflow: %0d", occupancy);
            if (stall_prev) begin
                assert (out_last == hold_last && out_req_valid == hold_req_valid &&
                        out_req_bank == hold_req_bank && out_req_row == hold_req_row &&
                        out_zero == hold_zero && out_row_mask == hold_row_mask &&
                        out_n_index == hold_n_index && out_group_index == hold_group_index &&
                        out_spatial_index == hold_spatial_index)
                    else $error("descriptor buffer output changed while stalled");
            end
            if (input_stall_prev) begin
                assert (in_valid && in_last == hold_in_last &&
                        in_req_valid == hold_in_req_valid &&
                        in_req_bank == hold_in_req_bank &&
                        in_req_row == hold_in_req_row && in_zero == hold_in_zero &&
                        in_row_mask == hold_in_row_mask && in_n_index == hold_in_n_index &&
                        in_group_index == hold_in_group_index &&
                        in_spatial_index == hold_in_spatial_index)
                    else $error("descriptor buffer input changed while stalled");
            end
            stall_prev <= out_valid && !out_ready;
            input_stall_prev <= in_valid && !in_ready;
            if (out_valid && !out_ready) begin
                hold_last <= out_last;
                hold_req_valid <= out_req_valid;
                hold_req_bank <= out_req_bank;
                hold_req_row <= out_req_row;
                hold_zero <= out_zero;
                hold_row_mask <= out_row_mask;
                hold_n_index <= out_n_index;
                hold_group_index <= out_group_index;
                hold_spatial_index <= out_spatial_index;
            end
            if (in_valid && !in_ready) begin
                hold_in_last <= in_last;
                hold_in_req_valid <= in_req_valid;
                hold_in_req_bank <= in_req_bank;
                hold_in_req_row <= in_req_row;
                hold_in_zero <= in_zero;
                hold_in_row_mask <= in_row_mask;
                hold_in_n_index <= in_n_index;
                hold_in_group_index <= in_group_index;
                hold_in_spatial_index <= in_spatial_index;
            end
        end else begin
            stall_prev <= 1'b0;
            input_stall_prev <= 1'b0;
        end
    end
`endif

endmodule

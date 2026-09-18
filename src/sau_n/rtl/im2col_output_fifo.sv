// Output-vector FIFO for the Im2Col pipeline.
//
// This module preserves the original FIFO behavior: a completed s3 vector may
// be pushed when the FIFO is not full, or when the current head is popped in
// the same cycle. Storage is intentionally kept as registers for now; SRAM
// mapping is a separate optimization task.

module im2col_output_fifo #(
    parameter int BLOCK_SIZE = 16,
    parameter int ELEM_W = 8,
    parameter int FIFO_DEPTH = 4,
    parameter int FIFO_PTR_W = $clog2(FIFO_DEPTH),
    parameter int GROUP_W = 13,
    parameter int SPATIAL_W = 32
) (
    input  logic clk,
    input  logic rst_n,

    input  logic                         in_valid,
    input  logic [BLOCK_SIZE*ELEM_W-1:0] in_data,
    input  logic [BLOCK_SIZE-1:0]         in_mask,
    input  logic [BLOCK_SIZE-1:0]         in_row_mask,
    input  logic [15:0]                   in_n_index,
    input  logic [GROUP_W-1:0]            in_group_index,
    input  logic [BLOCK_SIZE*SPATIAL_W-1:0] in_spatial_index,

    input  logic                         feed_ready,
    output logic                         feed_valid,
    output logic [BLOCK_SIZE*ELEM_W-1:0] feed_data,
    output logic [BLOCK_SIZE-1:0]         feed_mask,
    output logic [BLOCK_SIZE-1:0]         feed_row_mask,
    output logic [15:0]                   feed_n_index,
    output logic [GROUP_W-1:0]            feed_group_index,
    output logic [BLOCK_SIZE*SPATIAL_W-1:0] feed_spatial_index,

    // These fire signals are consumed by the top-level pipeline control.
    output logic                         push_fire,
    output logic                         pop_fire
);

    logic [BLOCK_SIZE*ELEM_W-1:0] fifo_data [FIFO_DEPTH];
    logic [BLOCK_SIZE-1:0] fifo_mask [FIFO_DEPTH];
    logic [BLOCK_SIZE-1:0] fifo_row_mask [FIFO_DEPTH];
    logic [15:0] fifo_n_index [FIFO_DEPTH];
    logic [GROUP_W-1:0] fifo_group_index [FIFO_DEPTH];
    logic [BLOCK_SIZE*SPATIAL_W-1:0] fifo_spatial_index [FIFO_DEPTH];
    logic [FIFO_PTR_W:0] fifo_count;
    logic [FIFO_PTR_W-1:0] fifo_rptr;
    logic [FIFO_PTR_W-1:0] fifo_wptr;

    assign feed_valid = (fifo_count != 0);
    assign feed_data = fifo_data[fifo_rptr];
    assign feed_mask = fifo_mask[fifo_rptr];
    assign feed_row_mask = fifo_row_mask[fifo_rptr];
    assign feed_n_index = fifo_n_index[fifo_rptr];
    assign feed_group_index = fifo_group_index[fifo_rptr];
    assign feed_spatial_index = fifo_spatial_index[fifo_rptr];

    assign pop_fire = feed_valid && feed_ready;
    assign push_fire = in_valid &&
        ((fifo_count != FIFO_DEPTH) || pop_fire);

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            fifo_count <= '0;
            fifo_rptr <= '0;
            fifo_wptr <= '0;
        end else begin
            unique case ({push_fire, pop_fire})
                2'b10: begin
                    fifo_data[fifo_wptr] <= in_data;
                    fifo_mask[fifo_wptr] <= in_mask;
                    fifo_row_mask[fifo_wptr] <= in_row_mask;
                    fifo_n_index[fifo_wptr] <= in_n_index;
                    fifo_group_index[fifo_wptr] <= in_group_index;
                    fifo_spatial_index[fifo_wptr] <= in_spatial_index;
                    fifo_wptr <= fifo_wptr + 1'b1;
                    fifo_count <= fifo_count + 1'b1;
                end
                2'b01: begin
                    fifo_rptr <= fifo_rptr + 1'b1;
                    fifo_count <= fifo_count - 1'b1;
                end
                2'b11: begin
                    fifo_data[fifo_wptr] <= in_data;
                    fifo_mask[fifo_wptr] <= in_mask;
                    fifo_row_mask[fifo_wptr] <= in_row_mask;
                    fifo_n_index[fifo_wptr] <= in_n_index;
                    fifo_group_index[fifo_wptr] <= in_group_index;
                    fifo_spatial_index[fifo_wptr] <= in_spatial_index;
                    fifo_wptr <= fifo_wptr + 1'b1;
                    fifo_rptr <= fifo_rptr + 1'b1;
                end
                default: begin end
            endcase
        end
    end

endmodule

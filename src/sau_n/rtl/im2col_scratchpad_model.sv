`timescale 1ns/1ps

// Banked INT8 Scratchpad model used by the standalone Im2Col verification.
//
// Every bank has an independent read address. A request is accepted at a
// rising edge and the selected byte is returned with resp_valid one cycle
// later. The public memory array intentionally remains visible so the
// self-checking testbench can preload tensors and assert their physical
// bank/row layout.
module im2col_scratchpad_model #(
    parameter int SP_BANKS        = 16,
    parameter int SP_BANK_ENTRIES = 4096,
    parameter int ELEM_W          = 8,
    parameter int SP_ROW_BITS     = $clog2(SP_BANK_ENTRIES)
) (
    input  logic clk,
    input  logic rst_n,

    input  logic [SP_BANKS-1:0] req_valid,
    input  var logic [SP_BANKS-1:0][SP_ROW_BITS-1:0] req_addr,
    output logic [SP_BANKS-1:0] resp_valid,
    output logic [SP_BANKS-1:0][ELEM_W-1:0] resp_data,

    input  logic [SP_BANKS-1:0] wr_valid,
    input  var logic [SP_BANKS-1:0][SP_ROW_BITS-1:0] wr_addr,
    input  var logic [SP_BANKS-1:0][ELEM_W-1:0] wr_data,
    input  logic [SP_BANKS-1:0] wr_allow,
    output logic [SP_BANKS-1:0] wr_ready
);
    logic [ELEM_W-1:0] mem [SP_BANKS][SP_BANK_ENTRIES];

    assign wr_ready = wr_allow;

    // This is a verification memory model. The testbench intentionally
    // preloads the public mem array through a hierarchical reference, so use
    // a procedural clocked block instead of always_ff's single-driver rule.
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            resp_valid <= '0;
            resp_data <= '0;
        end else begin
            resp_valid <= req_valid;
            for (int bank = 0; bank < SP_BANKS; bank++) begin
                if (req_valid[bank])
                    resp_data[bank] <= mem[bank][req_addr[bank]];
                if (wr_valid[bank] && wr_ready[bank])
                    mem[bank][wr_addr[bank]] <= wr_data[bank];
            end
        end
    end
endmodule

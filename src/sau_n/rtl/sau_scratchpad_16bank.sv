`timescale 1ns/1ps

// Production scratchpad wrapper.
//
// The external contract intentionally matches im2col_scratchpad_model:
// sixteen independent banks, one logical 8-bit byte per bank and a fixed
// one-cycle synchronous read response.  In TSMC28 mode each logical bank is
// implemented by one 2048x16 SRAM macro.  Two adjacent logical byte rows are
// packed into one physical word:
//
//   logical_row[11:1] -> macro address A[10:0]
//   logical_row[0]==0 -> Q[7:0]
//   logical_row[0]==1 -> Q[15:8]
//
// The byte packing is hidden inside this module; no scheduler or DMA address
// format change is required.  The macro path uses a low-phase input latch,
// matching the hold-safe technique used by weight_reuse_sram_1rw.
module sau_scratchpad_16bank #(
    parameter int SP_BANKS        = 16,
    parameter int SP_BANK_ENTRIES = 4096,
    parameter int ELEM_W          = 8,
    parameter int SP_ROW_BITS     = $clog2(SP_BANK_ENTRIES)
) (
    input  logic clk,
    input  logic rst_n,

    input  logic [SP_BANKS-1:0] req_valid,
    input  logic [SP_BANKS-1:0][SP_ROW_BITS-1:0] req_addr,
    output logic [SP_BANKS-1:0] resp_valid,
    output logic [SP_BANKS-1:0][ELEM_W-1:0] resp_data,

    input  logic [SP_BANKS-1:0] wr_valid,
    input  logic [SP_BANKS-1:0][SP_ROW_BITS-1:0] wr_addr,
    input  logic [SP_BANKS-1:0][ELEM_W-1:0] wr_data,
    input  logic [SP_BANKS-1:0] wr_allow,
    output logic [SP_BANKS-1:0] wr_ready
);
    // There is no arbitration inside a bank: the surrounding feeder keeps
    // read and write phases mutually exclusive.  A write is accepted whenever
    // its caller allows it.
    assign wr_ready = wr_allow;

`ifdef SAU_USE_TSMC28_SCRATCHPAD_SRAM
    generate
        if (SP_BANKS != 16 || SP_BANK_ENTRIES != 4096 || ELEM_W != 8 ||
            SP_ROW_BITS != 12) begin : g_bad_macro_shape
`ifndef SYNTHESIS
            initial $fatal(1,
                "TSMC28 scratchpad macro requires 16 banks x 4096 logical rows x 8 bits");
`endif
        end
    endgenerate

    logic [SP_BANKS-1:0] macro_en_q;
    logic [SP_BANKS-1:0] macro_write_en_q;
    logic [SP_BANKS-1:0][10:0] macro_addr_q;
    logic [SP_BANKS-1:0][15:0] macro_write_data_q;
    logic [SP_BANKS-1:0][15:0] macro_bweb_q;
    logic [SP_BANKS-1:0] macro_sel_q;
    logic [SP_BANKS-1:0][15:0] macro_q;
    logic [SP_BANKS-1:0] response_en_q;
    logic [SP_BANKS-1:0] response_sel_q;

    genvar bank;
    generate
        for (bank = 0; bank < SP_BANKS; bank++) begin : g_bank
            logic bank_access;
            logic bank_write;
            logic [SP_ROW_BITS-1:0] logical_addr;
            logic [7:0] logical_write_data;

            assign bank_access = req_valid[bank] |
                                 (wr_valid[bank] && wr_allow[bank]);
            assign bank_write = wr_valid[bank] && wr_allow[bank];
            assign logical_addr = bank_write ? wr_addr[bank] : req_addr[bank];
            assign logical_write_data = wr_data[bank];

            // Capture the next macro transaction during the low clock phase.
            // This keeps A/CEB/WEB/D/BWEB stable around the active edge.
            always_latch begin
                if (!clk) begin
                    macro_en_q[bank]         <= bank_access;
                    macro_write_en_q[bank]   <= bank_write;
                    macro_addr_q[bank]       <= logical_addr[SP_ROW_BITS-1:1];
                    macro_write_data_q[bank] <= logical_addr[0] ?
                                                {logical_write_data, 8'h00} :
                                                {8'h00, logical_write_data};
                    macro_bweb_q[bank]      <= logical_addr[0] ?
                                                16'h00ff : 16'hff00;
                    macro_sel_q[bank]       <= logical_addr[0];
                end
            end

            TS1N28HPCPHVTB2048X16M4SWSO u_sram (
                .SLP   (1'b0),
                .SD    (1'b0),
                .CLK   (clk),
                .CEB   (~macro_en_q[bank]),
                .WEB   (~macro_write_en_q[bank]),
                .A     (macro_addr_q[bank]),
                .D     (macro_write_data_q[bank]),
                .BWEB  (macro_bweb_q[bank]),
                .Q     (macro_q[bank])
            );
        end
    endgenerate

    // The macro Q output is valid after the active edge.  Register only the
    // response-valid and byte-select metadata; the selected Q byte remains
    // combinational so this wrapper does not add a pipeline cycle.
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            response_en_q  <= '0;
            response_sel_q <= '0;
        end else begin
            response_en_q  <= macro_en_q & ~macro_write_en_q;
            response_sel_q <= macro_sel_q;
        end
    end

    assign resp_valid = response_en_q;
    generate
        for (bank = 0; bank < SP_BANKS; bank++) begin : g_response
            assign resp_data[bank] = response_sel_q[bank] ?
                                     macro_q[bank][15:8] : macro_q[bank][7:0];
        end
    endgenerate
`else
    // Portable synchronous model used for RTL simulation and non-macro DC
    // checks.  The storage array is deliberately not reset.
    logic [ELEM_W-1:0] mem [SP_BANKS][SP_BANK_ENTRIES];

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            resp_valid <= '0;
            resp_data  <= '0;
        end else begin
            resp_valid <= req_valid;
            for (int b = 0; b < SP_BANKS; b++) begin
                if (req_valid[b])
                    resp_data[b] <= mem[b][req_addr[b]];
                if (wr_valid[b] && wr_ready[b])
                    mem[b][wr_addr[b]] <= wr_data[b];
            end
        end
    end
`endif

`ifndef SYNTHESIS
    always_ff @(posedge clk) begin
        for (int b = 0; b < SP_BANKS; b++) begin
            assert (!(req_valid[b] && wr_valid[b] && wr_allow[b]))
                else $error("scratchpad bank %0d read/write collision", b);
            if (req_valid[b])
                assert (req_addr[b] < SP_BANK_ENTRIES)
                    else $error("scratchpad bank %0d read address out of range", b);
            if (wr_valid[b] && wr_ready[b])
                assert (wr_addr[b] < SP_BANK_ENTRIES)
                    else $error("scratchpad bank %0d write address out of range", b);
        end
    end
`endif
endmodule

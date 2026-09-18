// Synchronous single-port storage used by weight_reuse_buffer.
//
// The default implementation is a portable RTL model.  Defining
// SAU_USE_TSMC28_WEIGHT_SRAM replaces it with the generated TSMC 256x128
// macro.  Both implementations accept one access on a rising edge and expose
// read data after that edge without adding another output register.
module weight_reuse_sram_1rw #(
    parameter int DATA_W = 128,
    parameter int DEPTH  = 256,
    parameter int ADDR_W = $clog2(DEPTH)
) (
    input  logic              clk,
    input  logic              en,
    input  logic              write_en,
    input  logic [ADDR_W-1:0] addr,
    input  logic [DATA_W-1:0] write_data,
    output logic [DATA_W-1:0] read_data
);
`ifdef SAU_USE_TSMC28_WEIGHT_SRAM
    logic              macro_en_q;
    logic              macro_write_en_q;
    logic [ADDR_W-1:0] macro_addr_q;
    logic [DATA_W-1:0] macro_write_data_q;

    // The controller state and fetch index are launched on the rising edge.
    // A zero-delay RTL-to-macro connection would therefore change A/CEB/WEB
    // immediately after that same edge and trip the macro's physical hold
    // checks.  A low-phase latch captures the next access and closes before
    // the active rising edge, while also avoiding a same-edge TB drive race.
    always_latch begin
        if (!clk) begin
            macro_en_q         <= en;
            macro_write_en_q   <= write_en;
            macro_addr_q       <= addr;
            macro_write_data_q <= write_data;
        end
    end

    generate
        if (DATA_W == 128 && DEPTH == 256 && ADDR_W == 8) begin : g_tsmc28_256x128
            TS1N28HPCPHVTB256X128M4SWSO u_sram (
                .SLP   (1'b0),
                .SD    (1'b0),
                .CLK   (clk),
                .CEB   (~macro_en_q),
                .WEB   (~macro_write_en_q),
                .A     (macro_addr_q),
                .D     (macro_write_data_q),
                .BWEB  ({128{1'b0}}),
                .Q     (read_data)
            );
        end else begin : g_unsupported_macro_shape
            assign read_data = 'x;
`ifndef SYNTHESIS
            initial $fatal(1,
                "TSMC28 weight SRAM requires DATA_W=128 DEPTH=256 ADDR_W=8");
`endif
        end
    endgenerate
`else
    logic [DATA_W-1:0] mem [0:DEPTH-1];

    // Nonblocking assignments preserve the old array value on a write edge.
    // weight_reuse_buffer ignores read_data while filling, so the ASIC macro
    // is not required to expose a defined read-during-write value.
    always_ff @(posedge clk) begin
        if (en) begin
            if (write_en)
                mem[addr] <= write_data;
            read_data <= mem[addr];
        end
    end
`endif

`ifndef SYNTHESIS
    always @(posedge clk) begin
`ifdef SAU_USE_TSMC28_WEIGHT_SRAM
        if (macro_en_q)
            assert (macro_addr_q < DEPTH)
                else $error("weight SRAM address out of range: %0d", macro_addr_q);
`else
        if (en)
            assert (addr < DEPTH)
                else $error("weight SRAM address out of range: %0d", addr);
`endif
    end
`endif
endmodule

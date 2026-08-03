module sram_converter_32to128_tb;
    logic clk = 0;
    logic rst_n = 0;
    always #5 clk = ~clk;

    logic         m32_valid;
    logic [31:0]  m32_addr;
    logic [31:0]  m32_wdata;
    logic [3:0]   m32_wstrb;
    logic [31:0]  m32_rdata;
    logic         m32_ready;
    logic         s128_valid;
    logic [31:0]  s128_addr;
    logic [127:0] s128_wdata;
    logic [15:0]  s128_wstrb;
    logic [127:0] s128_rdata;
    logic         s128_ready;

    sram_converter_32to128 dut (
        .clk(clk), .rst_n(rst_n),
        .m32_valid(m32_valid), .m32_addr(m32_addr),
        .m32_wdata(m32_wdata), .m32_wstrb(m32_wstrb),
        .m32_rdata(m32_rdata), .m32_ready(m32_ready),
        .s128_valid(s128_valid), .s128_addr(s128_addr),
        .s128_wdata(s128_wdata), .s128_wstrb(s128_wstrb),
        .s128_rdata(s128_rdata), .s128_ready(s128_ready)
    );

    task automatic tick_edge;
        @(posedge clk);
        #1;
    endtask

    task automatic clear_inputs;
        m32_valid = 0;
        m32_addr = 0;
        m32_wdata = 0;
        m32_wstrb = 0;
        s128_rdata = 0;
        s128_ready = 0;
    endtask

    initial begin
        clear_inputs();
        repeat (2) tick_edge();
        rst_n = 1;

        // IDLE captures addr[3:2], then CONVERT registers the aligned beat.
        @(negedge clk);
        m32_valid = 1;
        m32_addr = 32'h2001_000c;
        tick_edge();
        assert(dut.current_state == 2'b01)
            else $fatal(1, "read request did not enter CONVERT");
        assert(s128_valid == 0)
            else $fatal(1, "IDLE capture edge asserted s128_valid");

        @(negedge clk);
        m32_valid = 0;
        tick_edge();
        assert(dut.current_state == 2'b10)
            else $fatal(1, "CONVERT did not enter WAITACK");
        assert(s128_valid == 1 && s128_addr == 32'h2001_0000)
            else $fatal(1, "128-bit aligned read request is incorrect");

        // WAITACK selects word 3 from the registered SRAM response.
        @(negedge clk);
        s128_ready = 1;
        s128_rdata = {32'h1122_3344, 96'h0};
        tick_edge();
        assert(m32_ready == 1 && m32_rdata == 32'h1122_3344)
            else $fatal(1, "read response word selection is incorrect");
        @(negedge clk);
        s128_ready = 0;
        tick_edge();
        assert(m32_ready == 0 && s128_valid == 0)
            else $fatal(1, "IDLE did not clear response strobes");

        // A word-1 write shifts both byte strobes and data by four bytes.
        @(negedge clk);
        m32_valid = 1;
        m32_addr = 32'h2002_0004;
        m32_wdata = 32'haabb_ccdd;
        m32_wstrb = 4'b0101;
        tick_edge();
        @(negedge clk);
        m32_valid = 0;
        tick_edge();
        assert(s128_addr == 32'h2002_0000)
            else $fatal(1, "write address alignment is incorrect");
        assert(s128_wstrb == 16'h0050)
            else $fatal(1, "write strobe placement is incorrect");
        assert(s128_wdata[63:32] == 32'haabb_ccdd)
            else $fatal(1, "write data placement is incorrect");

        $display("SRAM_CONVERTER_32TO128_RTL_PASS");
        $finish;
    end
endmodule

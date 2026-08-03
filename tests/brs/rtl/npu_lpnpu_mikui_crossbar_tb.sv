module npu_lpnpu_mikui_crossbar_tb;
    logic clk = 0;
    logic rst_n = 0;
    always #5 clk = ~clk;

    logic         dbus_req;
    logic [15:0]  dbus_wstrb;
    logic [31:0]  dbus_addr;
    logic [127:0] dbus_wdata;
    logic [127:0] dbus_rdata;
    logic         dbus_resp;

    logic         master_req [2];
    logic [15:0]  master_wstrb [2];
    logic [31:0]  master_addr [2];
    logic [127:0] master_wdata [2];
    logic         master_start [2];
    logic         master_done [2];
    logic [127:0] master_rdata [2];

    logic         slave_req [2];
    logic [15:0]  slave_wstrb [2];
    logic [31:0]  slave_addr [2];
    logic [127:0] slave_wdata [2];
    logic [127:0] slave_rdata [2];

    crossbar_mi dut (
        .clk(clk), .rst_n(rst_n),
        .dbus_req(dbus_req), .dbus_wstrb(dbus_wstrb),
        .dbus_addr(dbus_addr), .dbus_wdata(dbus_wdata),
        .dbus_rdata(dbus_rdata), .dbus_resp(dbus_resp),
        .master_req(master_req), .master_wstrb(master_wstrb),
        .master_addr(master_addr), .master_wdata(master_wdata),
        .master_crossbar_start(master_start),
        .master_crossbar_done(master_done),
        .master_rdata(master_rdata),
        .slave_req(slave_req), .slave_wstrb(slave_wstrb),
        .slave_addr(slave_addr), .slave_wdata(slave_wdata),
        .slave_rdata(slave_rdata)
    );

    task automatic clear_inputs;
        dbus_req = 0;
        dbus_wstrb = 0;
        dbus_addr = 0;
        dbus_wdata = 0;
        for (int i = 0; i < 2; i++) begin
            master_req[i] = 0;
            master_wstrb[i] = 0;
            master_addr[i] = 0;
            master_wdata[i] = 0;
            master_start[i] = 0;
            master_done[i] = 0;
            slave_rdata[i] = 0;
        end
    endtask

    task automatic tick_edge;
        @(posedge clk);
        #1;
    endtask

    initial begin
        clear_inputs();
        repeat (2) tick_edge();
        rst_n = 1;

        // IDLE prioritizes accelerator start and executes IDLE output logic
        // on the transition edge.
        @(negedge clk);
        dbus_req = 1;
        dbus_addr = 32'h2001_0000;
        master_start[0] = 1;
        master_req[0] = 1;
        master_addr[0] = 32'h2001_0000;
        tick_edge();
        assert(dut.state == 2'b01) else $fatal(1, "start did not enter ACTIVE");
        assert(slave_req[0] == 0) else $fatal(1, "IDLE edge forwarded master");

        // In ACTIVE, loop iteration 1 overwrites iteration 0 on one bank.
        @(negedge clk);
        clear_inputs();
        master_req[0] = 1;
        master_addr[0] = 32'h2001_0010;
        master_wstrb[0] = 16'h0001;
        master_wdata[0] = 128'ha5;
        master_req[1] = 1;
        master_addr[1] = 32'h2001_0020;
        master_wstrb[1] = 16'h0001;
        master_wdata[1] = 128'h5a;
        tick_edge();
        assert(slave_req[0] == 1) else $fatal(1, "ACTIVE did not drive bank 0");
        assert(slave_addr[0] == 32'h2001_0020)
            else $fatal(1, "VEU did not overwrite SAU address");
        assert(slave_wdata[0][7:0] == 8'h5a)
            else $fatal(1, "VEU did not overwrite SAU data");

        // done changes the next state but the old ACTIVE logic still runs.
        @(negedge clk);
        clear_inputs();
        master_done[0] = 1;
        master_req[0] = 1;
        master_addr[0] = 32'h2002_0000;
        tick_edge();
        assert(dut.state == 2'b00) else $fatal(1, "done did not enter IDLE");
        assert(slave_req[1] == 1)
            else $fatal(1, "done edge did not execute ACTIVE logic");
        @(negedge clk);
        clear_inputs();
        tick_edge();
        assert(slave_req[0] == 0 && slave_req[1] == 0)
            else $fatal(1, "IDLE did not clear slave requests");

        // RVACTIVE has no transition on DBUS deassertion.
        @(negedge clk);
        dbus_req = 1;
        dbus_addr = 32'h2001_0000;
        tick_edge();
        assert(dut.state == 2'b10) else $fatal(1, "DBUS did not enter RVACTIVE");
        @(negedge clk);
        dbus_req = 0;
        repeat (2) tick_edge();
        assert(dut.state == 2'b10)
            else $fatal(1, "RVACTIVE incorrectly left on DBUS idle");

        $display("NPU_LPNPU_MIKUI_CROSSBAR_RTL_PASS");
        $finish;
    end
endmodule

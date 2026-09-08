`timescale 1ns/1ps

module tb_gemmini_im2col_chw_gather_readable;
    localparam int BLOCK_SIZE      = 16;
    localparam int ELEM_W          = 8;
    localparam int SP_BANKS        = BLOCK_SIZE;
    localparam int SP_BANK_ENTRIES = 4096;
    localparam int FIFO_DEPTH      = 4;
    localparam int SP_BANK_BITS    = $clog2(SP_BANKS);
    localparam int SP_ROW_BITS     = $clog2(SP_BANK_ENTRIES);
    localparam int SP_ADDR_BITS    = SP_BANK_BITS + SP_ROW_BITS;

    logic clk;
    logic rst_n;

    logic cfg_valid;
    logic [SP_ADDR_BITS-1:0] cfg_spad_base;
    logic [15:0] cfg_n, cfg_c, cfg_h, cfg_w, cfg_out_h, cfg_out_w;
    logic [3:0] cfg_kernel_h, cfg_kernel_w;
    logic [3:0] cfg_stride_h, cfg_stride_w;
    logic [3:0] cfg_dilation_h, cfg_dilation_w;
    logic [15:0] cfg_pad_top, cfg_pad_left;
    logic cfg_dw_mode;
    logic [BLOCK_SIZE-1:0] cfg_kernel_pattern;

    logic start;
    logic busy;
    logic done;

    logic [SP_BANKS-1:0] sram_req_valid;
    logic [SP_BANKS-1:0][SP_ROW_BITS-1:0] sram_req_addr;
    logic [SP_BANKS-1:0] sram_resp_valid;
    logic [SP_BANKS-1:0][ELEM_W-1:0] sram_resp_data;

    logic feed_valid;
    logic feed_ready;
    logic [BLOCK_SIZE*ELEM_W-1:0] feed_data;
    logic [BLOCK_SIZE-1:0] feed_mask;

    logic [ELEM_W-1:0] spad_mem [SP_BANKS][SP_BANK_ENTRIES];

    int errors;
    int vectors_seen;
    int vectors_expected;

    int t_n, t_c, t_h, t_w;
    int t_out_h, t_out_w;
    int t_kh, t_kw;
    int t_stride_h, t_stride_w;
    int t_dilation_h, t_dilation_w;
    int t_pad_top, t_pad_left;
    bit t_dw_mode;
    bit [BLOCK_SIZE-1:0] t_kernel_pattern;

    int exp_n, exp_c, exp_oh, exp_ow, exp_kh, exp_kw;

    bit fixture_mode;
    bit fixture_running;
    bit fixture_seen_done;
    int fixture_ready_period;
    int fixture_ready_high_cycles;
    int fixture_spad_base;
    longint unsigned fixture_ready_cycle;
    longint unsigned fixture_trace_cycle;
    longint unsigned fixture_done_cycle;
    integer fixture_trace_fd;
    string fixture_name;
    string fixture_trace_path;
    string fixture_resolved_sha256;

    gemmini_im2col_chw_gather_readable #(
        .BLOCK_SIZE(BLOCK_SIZE),
        .ELEM_W(ELEM_W),
        .SP_BANKS(SP_BANKS),
        .SP_BANK_ENTRIES(SP_BANK_ENTRIES),
        .FIFO_DEPTH(FIFO_DEPTH)
    ) dut (
        .clk(clk),
        .rst_n(rst_n),
        .cfg_valid(cfg_valid),
        .cfg_spad_base(cfg_spad_base),
        .cfg_n(cfg_n),
        .cfg_c(cfg_c),
        .cfg_h(cfg_h),
        .cfg_w(cfg_w),
        .cfg_out_h(cfg_out_h),
        .cfg_out_w(cfg_out_w),
        .cfg_kernel_h(cfg_kernel_h),
        .cfg_kernel_w(cfg_kernel_w),
        .cfg_stride_h(cfg_stride_h),
        .cfg_stride_w(cfg_stride_w),
        .cfg_dilation_h(cfg_dilation_h),
        .cfg_dilation_w(cfg_dilation_w),
        .cfg_pad_top(cfg_pad_top),
        .cfg_pad_left(cfg_pad_left),
        .cfg_dw_mode(cfg_dw_mode),
        .cfg_kernel_pattern(cfg_kernel_pattern),
        .start(start),
        .busy(busy),
        .done(done),
        .sram_req_valid(sram_req_valid),
        .sram_req_addr(sram_req_addr),
        .sram_resp_valid(sram_resp_valid),
        .sram_resp_data(sram_resp_data),
        .feed_valid(feed_valid),
        .feed_ready(feed_ready),
        .feed_data(feed_data),
        .feed_mask(feed_mask)
    );

    initial clk = 1'b0;
    always #5 clk = ~clk;

    function automatic bit fixture_ready_at(input longint unsigned cycle);
        begin
            fixture_ready_at =
                (cycle % fixture_ready_period) < fixture_ready_high_cycles;
        end
    endfunction

    // The DUT consumes feed_ready at the posedge ending a trace cycle. Update
    // it through an NBA at that posedge so the new value belongs to the next
    // stable interval and matches the value sampled by the negedge observer.
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            feed_ready <= 1'b1;
            fixture_ready_cycle <= 0;
        end else if (fixture_mode && fixture_running) begin
            if (start) begin
                fixture_ready_cycle <= 0;
                feed_ready <= fixture_ready_at(0);
            end else begin
                fixture_ready_cycle <= fixture_ready_cycle + 1;
                feed_ready <= fixture_ready_at(fixture_ready_cycle + 1);
            end
        end else begin
            feed_ready <= 1'b1;
        end
    end

    initial begin
        if ($test$plusargs("DUMP_VPD")) begin
            $vcdplusfile("/tmp/tb_gemmini_im2col_chw_gather_readable.vpd");
            $vcdpluson(0, tb_gemmini_im2col_chw_gather_readable);
            $vcdplusmemon(0, tb_gemmini_im2col_chw_gather_readable);
        end
    end

    always_comb begin
        for (int b = 0; b < SP_BANKS; b++) begin
            sram_resp_valid[b] = sram_req_valid[b];
            sram_resp_data[b] = spad_mem[b][sram_req_addr[b]];
        end
    end

    function automatic int ceil_div(input int a, input int b);
        begin
            ceil_div = (a + b - 1) / b;
        end
    endfunction

    function automatic int rows_per_word(input int w);
        begin
            if (w <= 0) begin
                rows_per_word = 1;
            end else if (w <= BLOCK_SIZE) begin
                rows_per_word = BLOCK_SIZE / w;
            end else begin
                rows_per_word = 1;
            end
        end
    endfunction

    function automatic int spatial_words(input int h, input int w);
        int w_words;
        begin
            w_words = ceil_div(w, BLOCK_SIZE);
            if (w <= BLOCK_SIZE) begin
                spatial_words = ceil_div(h, rows_per_word(w));
            end else begin
                spatial_words = h * w_words;
            end
        end
    endfunction

    function automatic int chw_word_offset(input int n, input int c, input int h, input int w);
        int w_words;
        int rpword;
        begin
            w_words = ceil_div(t_w, BLOCK_SIZE);
            rpword = rows_per_word(t_w);
            if (t_w <= BLOCK_SIZE) begin
                chw_word_offset =
                    n * t_c * spatial_words(t_h, t_w) +
                    c * spatial_words(t_h, t_w) +
                    h / rpword;
            end else begin
                chw_word_offset =
                    n * t_c * spatial_words(t_h, t_w) +
                    c * spatial_words(t_h, t_w) +
                    h * w_words +
                    w / BLOCK_SIZE;
            end
        end
    endfunction

    function automatic int chw_lane(input int h, input int w);
        begin
            if (t_w <= BLOCK_SIZE) begin
                chw_lane = (h % rows_per_word(t_w)) * t_w + w;
            end else begin
                chw_lane = w % BLOCK_SIZE;
            end
        end
    endfunction

    function automatic logic [ELEM_W-1:0] act_value(
        input int n,
        input int c,
        input int h,
        input int w
    );
        begin
            act_value = n * 97 + c * 31 + h * 7 + w + 1;
        end
    endfunction

    task automatic clear_spad;
        begin
            for (int b = 0; b < SP_BANKS; b++) begin
                for (int r = 0; r < SP_BANK_ENTRIES; r++) begin
                    spad_mem[b][r] = '0;
                end
            end
        end
    endtask

    task automatic write_spad_word(input int word_offset, input int lane, input logic [ELEM_W-1:0] value);
        int linear_addr;
        int bank;
        int row;
        begin
            linear_addr = int'(cfg_spad_base) + word_offset;
            bank = lane;
            row = linear_addr & (SP_BANK_ENTRIES - 1);
            spad_mem[bank][row] = value;
        end
    endtask

    task automatic fill_chw_activation;
        int word;
        int lane;
        begin
            clear_spad();
            for (int n = 0; n < t_n; n++) begin
                for (int c = 0; c < t_c; c++) begin
                    for (int h = 0; h < t_h; h++) begin
                        for (int w = 0; w < t_w; w++) begin
                            word = chw_word_offset(n, c, h, w);
                            lane = chw_lane(h, w);
                            write_spad_word(word, lane, act_value(n, c, h, w));
                        end
                    end
                end
            end
        end
    endtask

    task automatic build_expected(
        input int n,
        input int c,
        input int oh,
        input int ow,
        input int kh,
        input int kw,
        output logic [BLOCK_SIZE*ELEM_W-1:0] exp_data,
        output logic [BLOCK_SIZE-1:0] exp_mask
    );
        int out_h_i;
        int out_w_i;
        int local_h;
        int local_w;
        int padded_h;
        int padded_w;
        int real_h;
        int real_w;
        int tap_index;
        bit valid_lane;
        bit is_padding;
        begin
            exp_data = '0;
            exp_mask = '0;
            tap_index = (kh * t_kw + kw) % BLOCK_SIZE;

            for (int i = 0; i < BLOCK_SIZE; i++) begin
                if (t_w <= BLOCK_SIZE) begin
                    local_h = i / t_w;
                    local_w = i % t_w;
                    out_h_i = oh + local_h;
                    out_w_i = local_w;
                    valid_lane = local_h < rows_per_word(t_w);
                end else begin
                    local_h = 0;
                    local_w = i;
                    out_h_i = oh;
                    out_w_i = ow + i;
                    valid_lane = 1'b1;
                end

                padded_h = out_h_i * t_stride_h + kh * t_dilation_h;
                padded_w = out_w_i * t_stride_w + kw * t_dilation_w;
                real_h = padded_h - t_pad_top;
                real_w = padded_w - t_pad_left;
                is_padding = real_h < 0 || real_w < 0 || real_h >= t_h || real_w >= t_w;

                if (t_kernel_pattern[tap_index] &&
                    valid_lane &&
                    out_h_i < t_out_h &&
                    out_w_i < t_out_w &&
                    c < t_c) begin
                    exp_mask[i] = 1'b1;
                    if (!is_padding) begin
                        exp_data[i*ELEM_W +: ELEM_W] = act_value(n, c, real_h, real_w);
                    end
                end
            end
        end
    endtask

    task automatic advance_expected;
        begin
            if (exp_kw + 1 < t_kw) begin
                exp_kw++;
            end else begin
                exp_kw = 0;
                if (exp_kh + 1 < t_kh) begin
                    exp_kh++;
                end else begin
                    exp_kh = 0;
                    if (!t_dw_mode && exp_c + 1 < t_c) begin
                        exp_c++;
                    end else begin
                        exp_c = 0;
                        if (t_w > BLOCK_SIZE && exp_ow + BLOCK_SIZE < t_out_w) begin
                            exp_ow += BLOCK_SIZE;
                        end else begin
                            exp_ow = 0;
                            if (t_w <= BLOCK_SIZE && exp_oh + rows_per_word(t_w) < t_out_h) begin
                                exp_oh += rows_per_word(t_w);
                            end else if (t_w > BLOCK_SIZE && exp_oh + 1 < t_out_h) begin
                                exp_oh++;
                            end else begin
                                exp_oh = 0;
                                if (exp_n + 1 < t_n) begin
                                    exp_n++;
                                end
                            end
                        end
                    end
                end
            end
        end
    endtask

    function automatic int calc_total_vectors;
        int c_iters;
        int h_groups;
        int w_groups;
        begin
            c_iters = t_dw_mode ? 1 : t_c;
            h_groups = (t_w <= BLOCK_SIZE) ? ceil_div(t_out_h, rows_per_word(t_w)) : t_out_h;
            w_groups = (t_w <= BLOCK_SIZE) ? 1 : ceil_div(t_out_w, BLOCK_SIZE);
            calc_total_vectors = t_n * c_iters * h_groups * w_groups * t_kh * t_kw;
        end
    endfunction

    task automatic check_feed(input string case_name);
        logic [BLOCK_SIZE*ELEM_W-1:0] exp_data;
        logic [BLOCK_SIZE-1:0] exp_mask;
        begin
            build_expected(exp_n, exp_c, exp_oh, exp_ow, exp_kh, exp_kw, exp_data, exp_mask);
            if (feed_mask !== exp_mask || feed_data !== exp_data) begin
                $display("[%0t] ERROR %s vector %0d", $time, case_name, vectors_seen);
                $display("  idx n=%0d c=%0d oh=%0d ow=%0d kh=%0d kw=%0d", exp_n, exp_c, exp_oh, exp_ow, exp_kh, exp_kw);
                $display("  got  mask=%h data=%h", feed_mask, feed_data);
                $display("  exp  mask=%h data=%h", exp_mask, exp_data);
                errors++;
            end
            vectors_seen++;
            advance_expected();
        end
    endtask

    task automatic require_fixture_plusargs;
        int fixture_dw_mode;
        int fixture_kernel_pattern;
        begin
            if (!$value$plusargs("FIXTURE_NAME=%s", fixture_name))
                $fatal(1, "fixture mode requires +FIXTURE_NAME");
            if (!$value$plusargs("TRACE_FILE=%s", fixture_trace_path))
                $fatal(1, "fixture mode requires +TRACE_FILE");
            if (!$value$plusargs(
                    "RESOLVED_CONFIG_SHA256=%s", fixture_resolved_sha256))
                $fatal(1, "fixture mode requires +RESOLVED_CONFIG_SHA256");
            if (!$value$plusargs("CFG_N=%d", t_n))
                $fatal(1, "fixture mode requires +CFG_N");
            if (!$value$plusargs("CFG_C=%d", t_c))
                $fatal(1, "fixture mode requires +CFG_C");
            if (!$value$plusargs("CFG_H=%d", t_h))
                $fatal(1, "fixture mode requires +CFG_H");
            if (!$value$plusargs("CFG_W=%d", t_w))
                $fatal(1, "fixture mode requires +CFG_W");
            if (!$value$plusargs("CFG_OUT_H=%d", t_out_h))
                $fatal(1, "fixture mode requires +CFG_OUT_H");
            if (!$value$plusargs("CFG_OUT_W=%d", t_out_w))
                $fatal(1, "fixture mode requires +CFG_OUT_W");
            if (!$value$plusargs("CFG_KERNEL_H=%d", t_kh))
                $fatal(1, "fixture mode requires +CFG_KERNEL_H");
            if (!$value$plusargs("CFG_KERNEL_W=%d", t_kw))
                $fatal(1, "fixture mode requires +CFG_KERNEL_W");
            if (!$value$plusargs("CFG_STRIDE_H=%d", t_stride_h))
                $fatal(1, "fixture mode requires +CFG_STRIDE_H");
            if (!$value$plusargs("CFG_STRIDE_W=%d", t_stride_w))
                $fatal(1, "fixture mode requires +CFG_STRIDE_W");
            if (!$value$plusargs("CFG_DILATION_H=%d", t_dilation_h))
                $fatal(1, "fixture mode requires +CFG_DILATION_H");
            if (!$value$plusargs("CFG_DILATION_W=%d", t_dilation_w))
                $fatal(1, "fixture mode requires +CFG_DILATION_W");
            if (!$value$plusargs("CFG_PAD_TOP=%d", t_pad_top))
                $fatal(1, "fixture mode requires +CFG_PAD_TOP");
            if (!$value$plusargs("CFG_PAD_LEFT=%d", t_pad_left))
                $fatal(1, "fixture mode requires +CFG_PAD_LEFT");
            if (!$value$plusargs("CFG_SPAD_BASE=%d", fixture_spad_base))
                $fatal(1, "fixture mode requires +CFG_SPAD_BASE");
            if (!$value$plusargs("CFG_DW_MODE=%d", fixture_dw_mode))
                $fatal(1, "fixture mode requires +CFG_DW_MODE");
            if (!$value$plusargs(
                    "CFG_KERNEL_PATTERN=%h", fixture_kernel_pattern))
                $fatal(1, "fixture mode requires +CFG_KERNEL_PATTERN");
            if (!$value$plusargs(
                    "READY_PERIOD=%d", fixture_ready_period))
                $fatal(1, "fixture mode requires +READY_PERIOD");
            if (!$value$plusargs(
                    "READY_HIGH_CYCLES=%d", fixture_ready_high_cycles))
                $fatal(1, "fixture mode requires +READY_HIGH_CYCLES");

            if (fixture_dw_mode != 0)
                $fatal(1, "fixture cfg_dw_mode must be zero");
            if (fixture_kernel_pattern != 16'hffff)
                $fatal(1, "fixture cfg_kernel_pattern must be 0xffff");
            if (fixture_ready_period < 1 || fixture_ready_high_cycles < 1 ||
                fixture_ready_high_cycles > fixture_ready_period)
                $fatal(1, "invalid fixture ready pattern");

            t_dw_mode = fixture_dw_mode[0];
            t_kernel_pattern = fixture_kernel_pattern[BLOCK_SIZE-1:0];
        end
    endtask

    task automatic write_fixture_trace_header;
        begin
            $fwrite(fixture_trace_fd,
                    "schema_version,resolved_config_sha256,cycle,state,busy,done,");
            $fwrite(fixture_trace_fd,
                    "fifo_count,fifo_rptr,fifo_wptr,req_valid");
            for (int b = 0; b < SP_BANKS; b++) begin
                $fwrite(fixture_trace_fd, ",req_addr_b%02d", b);
            end
            $fwrite(fixture_trace_fd, ",resp_valid");
            for (int b = 0; b < SP_BANKS; b++) begin
                $fwrite(fixture_trace_fd, ",resp_data_b%02d", b);
            end
            $fwrite(fixture_trace_fd,
                    ",feed_valid,feed_ready,feed_data,feed_mask\n");
        end
    endtask

    task automatic write_fixture_trace_cycle;
        logic [BLOCK_SIZE*ELEM_W-1:0] trace_feed_data;
        logic [BLOCK_SIZE-1:0] trace_feed_mask;
        begin
            trace_feed_data = feed_valid ? feed_data : '0;
            trace_feed_mask = feed_valid ? feed_mask : '0;
            $fwrite(fixture_trace_fd,
                    "1,%s,%0d,%0d,%0d,%0d,%0d,%0d,%0d,0x%04h",
                    fixture_resolved_sha256, fixture_trace_cycle, dut.state,
                    busy, done, dut.fifo_count, dut.fifo_rptr, dut.fifo_wptr,
                    sram_req_valid);
            for (int b = 0; b < SP_BANKS; b++) begin
                if (sram_req_valid[b])
                    $fwrite(fixture_trace_fd, ",0x%03h", sram_req_addr[b]);
                else
                    $fwrite(fixture_trace_fd, ",0x000");
            end
            $fwrite(fixture_trace_fd, ",0x%04h", sram_resp_valid);
            for (int b = 0; b < SP_BANKS; b++) begin
                if (sram_resp_valid[b])
                    $fwrite(fixture_trace_fd, ",0x%02h", sram_resp_data[b]);
                else
                    $fwrite(fixture_trace_fd, ",0x00");
            end
            $fwrite(fixture_trace_fd, ",%0d,%0d,0x%032h,0x%04h\n",
                    feed_valid, feed_ready, trace_feed_data, trace_feed_mask);
        end
    endtask

    task automatic initialize_expected_indices;
        begin
            exp_n = 0;
            exp_c = 0;
            exp_oh = 0;
            exp_ow = 0;
            exp_kh = 0;
            exp_kw = 0;
            vectors_seen = 0;
        end
    endtask

    task automatic run_fixture;
        bit fixture_drained;
        begin
            require_fixture_plusargs();
            vectors_expected = calc_total_vectors();

            cfg_spad_base = fixture_spad_base[SP_ADDR_BITS-1:0];
            fill_chw_activation();
            cfg_n = t_n[15:0];
            cfg_c = t_c[15:0];
            cfg_h = t_h[15:0];
            cfg_w = t_w[15:0];
            cfg_out_h = t_out_h[15:0];
            cfg_out_w = t_out_w[15:0];
            cfg_kernel_h = t_kh[3:0];
            cfg_kernel_w = t_kw[3:0];
            cfg_stride_h = t_stride_h[3:0];
            cfg_stride_w = t_stride_w[3:0];
            cfg_dilation_h = t_dilation_h[3:0];
            cfg_dilation_w = t_dilation_w[3:0];
            cfg_pad_top = t_pad_top[15:0];
            cfg_pad_left = t_pad_left[15:0];
            cfg_dw_mode = t_dw_mode;
            cfg_kernel_pattern = t_kernel_pattern;

            fixture_trace_fd = $fopen(fixture_trace_path, "w");
            if (fixture_trace_fd == 0)
                $fatal(1, "cannot open fixture trace %s", fixture_trace_path);
            write_fixture_trace_header();
            initialize_expected_indices();

            @(negedge clk);
            cfg_valid = 1'b1;
            @(negedge clk);
            cfg_valid = 1'b0;
            @(negedge clk);
            fixture_running = 1'b1;
            start = 1'b1;
            @(negedge clk);
            start = 1'b0;

            fixture_trace_cycle = 0;
            fixture_seen_done = 1'b0;
            fixture_drained = 1'b0;
            while (!fixture_drained) begin
                if (feed_valid && feed_ready)
                    check_feed(fixture_name);
                write_fixture_trace_cycle();
                if (done) begin
                    fixture_seen_done = 1'b1;
                    fixture_done_cycle = fixture_trace_cycle;
                end
                if (fixture_seen_done && dut.fifo_count == 0) begin
                    fixture_drained = 1'b1;
                end else begin
                    fixture_trace_cycle++;
                    @(negedge clk);
                end
            end

            $fclose(fixture_trace_fd);
            if (vectors_seen != vectors_expected) begin
                $display("ERROR %s saw %0d vectors, expected %0d",
                         fixture_name, vectors_seen, vectors_expected);
                errors++;
            end
            if (errors != 0) begin
                $display("FAIL fixture %s errors=%0d", fixture_name, errors);
                $fatal(1);
            end
            $display("rtl_done_cycle=%0d", fixture_done_cycle);
            $display("drained_cycle=%0d", fixture_trace_cycle);
            $display("post_done_drain_cycles=%0d",
                     fixture_trace_cycle - fixture_done_cycle);
            $display("PASS fixture %s vectors=%0d", fixture_name, vectors_seen);
            $finish;
        end
    endtask

    task automatic run_case(
        input string case_name,
        input int n,
        input int c,
        input int h,
        input int w,
        input int kh,
        input int kw,
        input int stride_h,
        input int stride_w,
        input int dilation_h,
        input int dilation_w,
        input int pad_top,
        input int pad_left,
        input bit dw_mode
    );
        int timeout;
        begin
            t_n = n;
            t_c = c;
            t_h = h;
            t_w = w;
            t_kh = kh;
            t_kw = kw;
            t_stride_h = stride_h;
            t_stride_w = stride_w;
            t_dilation_h = dilation_h;
            t_dilation_w = dilation_w;
            t_pad_top = pad_top;
            t_pad_left = pad_left;
            t_dw_mode = dw_mode;
            t_kernel_pattern = '1;
            t_out_h = ((h + 2 * pad_top - dilation_h * (kh - 1) - 1) / stride_h) + 1;
            t_out_w = ((w + 2 * pad_left - dilation_w * (kw - 1) - 1) / stride_w) + 1;
            vectors_expected = calc_total_vectors();

            cfg_spad_base = '0;
            fill_chw_activation();

            cfg_valid = 1'b1;
            cfg_n = n[15:0];
            cfg_c = c[15:0];
            cfg_h = h[15:0];
            cfg_w = w[15:0];
            cfg_out_h = t_out_h[15:0];
            cfg_out_w = t_out_w[15:0];
            cfg_kernel_h = kh[3:0];
            cfg_kernel_w = kw[3:0];
            cfg_stride_h = stride_h[3:0];
            cfg_stride_w = stride_w[3:0];
            cfg_dilation_h = dilation_h[3:0];
            cfg_dilation_w = dilation_w[3:0];
            cfg_pad_top = pad_top[15:0];
            cfg_pad_left = pad_left[15:0];
            cfg_dw_mode = dw_mode;
            cfg_kernel_pattern = t_kernel_pattern;
            @(posedge clk);
            cfg_valid = 1'b0;

            initialize_expected_indices();

            $display("RUN %-28s N=%0d C=%0d H=%0d W=%0d K=%0dx%0d stride=%0dx%0d pad=%0dx%0d out=%0dx%0d vectors=%0d",
                     case_name, n, c, h, w, kh, kw, stride_h, stride_w, pad_top, pad_left,
                     t_out_h, t_out_w, vectors_expected);

            start = 1'b1;
            @(posedge clk);
            start = 1'b0;

            timeout = 20000;
            while (!done && timeout > 0) begin
                @(posedge clk);
                if (feed_valid && feed_ready) begin
                    check_feed(case_name);
                end
                timeout--;
            end

            if (timeout == 0) begin
                $display("ERROR %s timed out", case_name);
                errors++;
            end
            if (vectors_seen != vectors_expected) begin
                $display("ERROR %s saw %0d vectors, expected %0d", case_name, vectors_seen, vectors_expected);
                errors++;
            end
            @(posedge clk);
        end
    endtask

    initial begin
        fixture_mode = $test$plusargs("FIXTURE_MODE");
        fixture_running = 1'b0;
        errors = 0;
        cfg_valid = 1'b0;
        start = 1'b0;
        rst_n = 1'b0;
        repeat (5) @(posedge clk);
        rst_n = 1'b1;
        repeat (2) @(posedge clk);

        if (fixture_mode) begin
            run_fixture();
        end

        run_case("w5_pack3_pad1_stride1", 1, 2, 4, 5, 3, 3, 1, 1, 1, 1, 1, 1, 1'b0);
        run_case("w7_pack2_pad0_stride2", 1, 3, 5, 7, 2, 2, 2, 2, 1, 1, 0, 0, 1'b0);
        run_case("w20_split_pad1_stride1", 1, 2, 3, 20, 3, 3, 1, 1, 1, 1, 1, 1, 1'b0);
        run_case("w9_pack1_pad2_stride2", 2, 1, 6, 9, 3, 3, 2, 2, 1, 1, 2, 2, 1'b0);

        if (errors == 0) begin
            $display("PASS tb_gemmini_im2col_chw_gather_readable");
        end else begin
            $display("FAIL tb_gemmini_im2col_chw_gather_readable errors=%0d", errors);
            $fatal(1);
        end
        $finish;
    end
endmodule

// Static Scratchpad scheduler and A/B stream join for the 16x16 PE array.
// Weight tiles are loaded into one local reuse buffer before Im2Col owns the
// shared Scratchpad. With one C segment, the resident tile stays active across
// spatial groups and batches. LOAD_B and RUN_A remain intentionally disjoint.
module sau_data_feeder #(
    parameter int BLOCK_SIZE      = 16,
    parameter int ELEM_W          = 8,
    parameter int SP_BANKS        = BLOCK_SIZE,
    parameter int SP_BANK_ENTRIES = 4096,
    parameter int MAX_C           = 64,
    parameter int MAX_SEG_C       = 16,
    parameter int MAX_KERNEL      = 5,
    parameter int MAX_K           = 250,
    parameter int K_W             = $clog2(MAX_K + 1),
    parameter int GROUP_W         = 13,
    parameter int SP_BANK_BITS    = $clog2(SP_BANKS),
    parameter int SP_ROW_BITS     = $clog2(SP_BANK_ENTRIES),
    parameter int SP_ADDR_BITS    = SP_BANK_BITS + SP_ROW_BITS
) (
    input  logic clk,
    input  logic rst_n,

    input  logic                         cfg_valid,
    input  logic                         cfg_dw_mode,
    input  logic                         cfg_mline_mode,
    input  logic [4:0]                   cfg_rows_per_group,
    input  logic [GROUP_W-1:0]           cfg_groups_per_n,
    input  logic [SP_ADDR_BITS-1:0]      cfg_activation_base,
    input  logic [SP_ADDR_BITS-1:0]      cfg_weight_base,
    input  logic [SP_ADDR_BITS-1:0]      cfg_bias_base,
    input  logic [15:0]                  cfg_n,
    input  logic [15:0]                  cfg_c,
    input  logic [15:0]                  cfg_h,
    input  logic [15:0]                  cfg_w,
    input  logic [15:0]                  cfg_out_h,
    input  logic [15:0]                  cfg_out_w,
    input  logic [3:0]                   cfg_kernel_h,
    input  logic [3:0]                   cfg_kernel_w,
    input  logic [3:0]                   cfg_stride_h,
    input  logic [3:0]                   cfg_stride_w,
    input  logic [3:0]                   cfg_dilation_h,
    input  logic [3:0]                   cfg_dilation_w,
    input  logic [15:0]                  cfg_pad_top,
    input  logic [15:0]                  cfg_pad_left,
    input  logic [31:0]                  cfg_kernel_pattern,
    input  logic [15:0]                  cfg_oc,

    input  logic start,
    input  logic tile_advance_ready,
    output logic busy,
    output logic done,
    output logic cfg_error,

    output logic [SP_BANKS-1:0] sram_req_valid,
    output logic [SP_BANKS-1:0][SP_ROW_BITS-1:0] sram_req_addr,
    input  logic [SP_BANKS-1:0] sram_resp_valid,
    input  var logic [SP_BANKS-1:0][ELEM_W-1:0] sram_resp_data,

    output logic                         pe_valid,
    input  logic                         pe_ready,
    output logic [BLOCK_SIZE*ELEM_W-1:0] pe_a_data,
    output logic [BLOCK_SIZE-1:0]        pe_a_mask,
    output logic [BLOCK_SIZE*ELEM_W-1:0] pe_b_data,
    output logic [BLOCK_SIZE-1:0]        pe_b_mask,
    output logic [K_W-1:0]               pe_k_index,
    output logic                         pe_last_k,
    output logic [15:0]                  pe_oc_tile,
    output logic [15:0]                  pe_c_tile,
    output logic                         pe_last_c_tile,
    output logic [K_W-1:0]               pe_k_count,
    output logic                         pe_last_group,
    output logic                         pe_last_oc_tile,
    output logic [BLOCK_SIZE-1:0]        pe_row_mask,
    output logic [15:0]                  pe_n_index,
    output logic [GROUP_W-1:0]           pe_group_index,
    output logic [BLOCK_SIZE*32-1:0]     pe_spatial_index,
    output logic [BLOCK_SIZE*16-1:0]     pe_bias_data,
    output logic [BLOCK_SIZE-1:0]        pe_bias_mask,
    output logic [15:0]                  pe_bias_oc_tile
);
    typedef enum logic [4:0] {
        S_IDLE, S_CFG_BASIC, S_CFG_PRODUCT0, S_CFG_PRODUCT1,
        S_CFG_PRODUCT2, S_CFG_COMMIT,
        S_FILL_CFG, S_FILL_START, S_LOAD_B, S_ACTIVATE,
        S_BIAS_CFG, S_BIAS_START, S_LOAD_BIAS,
        S_IM_CFG, S_IM_START, S_RUN_A, S_RELEASE, S_NEXT_TILE, S_DONE
    } state_t;
    state_t state;

    logic [SP_ADDR_BITS-1:0] activation_base_q, weight_base_q, bias_base_q;
    logic [15:0] n_q, c_q, h_q, w_q, out_h_q, out_w_q, pad_top_q, pad_left_q, oc_q;
    logic [3:0] kernel_h_q, kernel_w_q, stride_h_q, stride_w_q;
    logic [3:0] dilation_h_q, dilation_w_q;
    logic [31:0] kernel_pattern_q;
    logic dw_mode_q;
    logic mline_mode_q;
    logic [4:0] rows_per_group_q;
    logic [GROUP_W-1:0] cfg_groups_per_n_q;
    logic [15:0] tile_q, c_tile_q, c_count_q;
    logic [15:0] segment_c_base_q;
    logic [15:0] segment_width_q;
    logic [15:0] tile_width_q;
    logic [16:0] tile_oc_base_q;
    logic [15:0] group_n_q;
    logic [GROUP_W-1:0] group_index_q, groups_per_n_q;
    // Predecoded origin of the current single spatial group. Keeping this
    // alongside the sequential group index avoids a runtime group / row-width
    // divider in the Im2Col configuration snapshot.
    logic [15:0] group_oh_base_q, group_ow_base_q;
    logic [K_W-1:0] k_count_q;
    logic [K_W-1:0] pe_count_q;
    logic [K_W-1:0] expected_k_q;
    logic [15:0] first_c_count_q;
    logic [K_W-1:0] first_k_count_q;
    logic [15:0] configured_segment_width;
    logic [16:0] next_segment_c_base;
    logic [15:0] next_segment_c_count;
    logic [K_W-1:0] next_segment_k_count;
    logic [16:0] next_tile_oc_base;

    logic wl_cfg_valid, wl_start, wl_busy, wl_done, wl_error;
    logic [SP_BANKS-1:0] wl_req_valid, wl_resp_valid;
    logic [SP_BANKS-1:0][SP_ROW_BITS-1:0] wl_req_addr;
    logic [SP_BANKS-1:0][ELEM_W-1:0] wl_resp_data;
    logic wl_weight_valid, wl_weight_ready, wl_last_k, wl_last_tile;
    logic [BLOCK_SIZE*ELEM_W-1:0] wl_weight_data;
    logic [BLOCK_SIZE-1:0] wl_weight_mask;

    logic im_cfg_valid, im_start, im_busy, im_done, im_error;
    logic [SP_BANKS-1:0] im_req_valid, im_resp_valid;
    logic [SP_BANKS-1:0][SP_ROW_BITS-1:0] im_req_addr;
    logic [SP_BANKS-1:0][ELEM_W-1:0] im_resp_data;
    logic im_feed_valid, im_feed_ready;
    logic [BLOCK_SIZE*ELEM_W-1:0] im_feed_data;
    logic [BLOCK_SIZE-1:0] im_feed_mask;
    logic [BLOCK_SIZE-1:0] im_feed_row_mask;
    logic [15:0] im_feed_n_index;
    logic [GROUP_W-1:0] im_feed_group_index;
    logic [BLOCK_SIZE*32-1:0] im_feed_spatial_index;

    logic bl_cfg_valid, bl_start, bl_busy, bl_done, bl_error;
    logic [SP_BANKS-1:0] bl_req_valid, bl_resp_valid;
    logic [SP_BANKS-1:0][SP_ROW_BITS-1:0] bl_req_addr;
    logic [SP_BANKS-1:0][ELEM_W-1:0] bl_resp_data;
    logic bl_bias_valid, bl_bias_ready, bl_bias_last_tile;
    logic [BLOCK_SIZE*16-1:0] bl_bias_data;
    logic [BLOCK_SIZE-1:0] bl_bias_mask;
    logic [15:0] bl_bias_oc_tile;
    logic [BLOCK_SIZE*16-1:0] bias_data_q;
    logic [BLOCK_SIZE-1:0] bias_mask_q;
    logic [15:0] bias_tile_q;

    logic buf_fill_start, buf_activate, buf_release;
    logic buf_fill_ready, buf_read_valid, buf_read_ready;
    logic [BLOCK_SIZE*ELEM_W-1:0] buf_read_data;
    logic [BLOCK_SIZE-1:0] buf_read_mask;
    logic [K_W-1:0] buf_read_k;
    logic buf_read_last_k;
    logic [15:0] buf_read_tile;
    logic [15:0] buf_read_c_tile;
    logic buf_protocol_error;

    // Accepted request owner from the previous cycle, one bit per bank:
    // 0=Im2Col, 1=Weight Loader. resp_valid gates inactive owner entries.
    logic [SP_BANKS-1:0] response_owner_weight;
    logic [SP_BANKS-1:0] response_owner_bias;
    
    //判断输出通道lane是否是有效的
    function automatic logic [15:0] channel_limit_for_kernel(
        input logic [3:0] kernel_h,
        input logic [3:0] kernel_w
    );
        integer area;
        integer limit;
        begin
            area = kernel_h * kernel_w;
            limit = (area > 0) ? (MAX_K / area) : 1;
            if (limit < 1)
                limit = 1;
            if (limit > BLOCK_SIZE)
                limit = BLOCK_SIZE;
            channel_limit_for_kernel = limit;
        end
    endfunction

    function automatic logic [BLOCK_SIZE-1:0] expected_oc_mask(
        input logic [15:0] oc_count,
        input logic [16:0] tile_base,
        input logic [15:0] tile_width
    );
        logic [BLOCK_SIZE-1:0] value;
        begin
            value = '0;
            for (int lane = 0; lane < BLOCK_SIZE; lane++)
                value[lane] = (lane < tile_width) &&
                              (tile_base + lane < oc_count);
            return value;
        end
    endfunction
    
    /*
    1.告诉weight loader本次tile加载多少输入通道
    2.告诉im2col本次tile读取多少输入通道
    3.计算当前segment的K的数量
    4.用于判断是否是最后一个C segment
    */
    function automatic logic [15:0] segment_c_count_from_base(
        input logic [15:0] total_c,
        input logic [16:0] base,
        input logic [15:0] segment_width
    );
        begin
            if (base + segment_width <= total_c)
                segment_c_count_from_base = segment_width;
            else
                segment_c_count_from_base = total_c - base;
        end
    endfunction

    // MAX_K is bounded and supported kernel areas are 1, 9, and 25.
    // Explicit shift/add forms avoid a generic multiplier in tile transitions.
    function automatic logic [K_W-1:0] k_count_for_kernel(
        input logic [15:0] c_count,
        input logic [3:0] kernel_h
    );
        begin
            case (kernel_h)
                4'd1: k_count_for_kernel = c_count;
                4'd3: k_count_for_kernel = c_count + (c_count << 3);
                4'd5: k_count_for_kernel = c_count + (c_count << 3) +
                                             (c_count << 4);
                default: k_count_for_kernel = '0;
            endcase
        end
    endfunction

    assign configured_segment_width =
        channel_limit_for_kernel(kernel_h_q, kernel_w_q);
    assign next_segment_c_base =
        {1'b0, segment_c_base_q} + {1'b0, segment_width_q};
    assign next_segment_c_count =
        segment_c_count_from_base(c_q, next_segment_c_base, segment_width_q);
    assign next_segment_k_count =
        k_count_for_kernel(next_segment_c_count, kernel_h_q);
    assign next_tile_oc_base =
        tile_oc_base_q + {1'b0, tile_width_q};


    assign busy = (state != S_IDLE);
    assign wl_cfg_valid = (state == S_FILL_CFG);
    assign wl_start = (state == S_FILL_START);
    assign buf_fill_start = (state == S_FILL_CFG);
    assign buf_activate = (state == S_ACTIVATE);
    assign im_cfg_valid = (state == S_IM_CFG);
    assign im_start = (state == S_IM_START);
    assign bl_cfg_valid = (state == S_BIAS_CFG);
    assign bl_start = (state == S_BIAS_START);
    assign bl_bias_ready = (state == S_LOAD_BIAS);
    assign buf_release = (state == S_RELEASE);

    assign wl_weight_ready = (state == S_LOAD_B) && buf_fill_ready;
    assign im_feed_ready = (state == S_RUN_A) && buf_read_valid && pe_ready &&
                           (buf_read_k == expected_k_q) &&
                           (buf_read_tile == tile_q) &&
                           (buf_read_c_tile == c_tile_q);
    assign buf_read_ready = (state == S_RUN_A) && im_feed_valid && pe_ready &&
                            (buf_read_k == expected_k_q) &&
                            (buf_read_tile == tile_q) &&
                            (buf_read_c_tile == c_tile_q);

    assign pe_valid = (state == S_RUN_A) && im_feed_valid && buf_read_valid &&
                      (buf_read_k == expected_k_q) &&
                      (buf_read_tile == tile_q) &&
                      (buf_read_c_tile == c_tile_q);
    assign pe_a_data = im_feed_data;
    assign pe_a_mask = im_feed_mask;
    assign pe_b_data = buf_read_data;
    assign pe_b_mask = buf_read_mask;
    assign pe_k_index = buf_read_k;
    assign pe_last_k = buf_read_last_k;
    assign pe_oc_tile = buf_read_tile;
    assign pe_c_tile = buf_read_c_tile;
    assign pe_last_c_tile = dw_mode_q ||
                            (segment_c_base_q + c_count_q >= c_q);
    assign pe_k_count = k_count_q;
    assign pe_last_group = group_n_q + 1'b1 == n_q &&
                           group_index_q + 1'b1 == groups_per_n_q;
    assign pe_last_oc_tile =
        tile_oc_base_q + {1'b0, tile_width_q} >= {1'b0, oc_q};
    // DW selects a compact channel tile; normal Conv uses the same dynamic
    // width for C segments so every K vector fits the 256-word SRAM.
    // Normal Conv tiles split the input C dimension; DW tiles split the
    // compact weight/output channel dimension, so both loaders and Im2Col
    // advance by the selected OC tile width.
    assign pe_row_mask = im_feed_row_mask;
    assign pe_n_index = im_feed_n_index;
    assign pe_group_index = im_feed_group_index;
    assign pe_spatial_index = im_feed_spatial_index;
    assign pe_bias_data = bias_data_q;
    assign pe_bias_mask = bias_mask_q;
    assign pe_bias_oc_tile = bias_tile_q;

    always_comb begin
        sram_req_valid = '0;
        sram_req_addr = '0;
        if (state == S_LOAD_B) begin
            sram_req_valid = wl_req_valid;
            sram_req_addr = wl_req_addr;
        end else if (state == S_LOAD_BIAS) begin
            sram_req_valid = bl_req_valid;
            sram_req_addr = bl_req_addr;
        end else if (state == S_RUN_A) begin
            sram_req_valid = im_req_valid;
            sram_req_addr = im_req_addr;
        end

        wl_resp_valid = sram_resp_valid & response_owner_weight;
        bl_resp_valid = sram_resp_valid & response_owner_bias;
        im_resp_valid = sram_resp_valid &
                        ~response_owner_weight & ~response_owner_bias;
        wl_resp_data = sram_resp_data;
        bl_resp_data = sram_resp_data;
        im_resp_data = sram_resp_data;
    end

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            response_owner_weight <= '0;
            response_owner_bias <= '0;
        end else begin
            for (int bank = 0; bank < SP_BANKS; bank++) begin
                if (sram_req_valid[bank])
                    response_owner_weight[bank] <= (state == S_LOAD_B);
                if (sram_req_valid[bank])
                    response_owner_bias[bank] <= (state == S_LOAD_BIAS);
            end
        end
    end

    weight_loader #(
        .BLOCK_SIZE(BLOCK_SIZE), .ELEM_W(ELEM_W), .SP_BANKS(SP_BANKS),
        .SP_BANK_ENTRIES(SP_BANK_ENTRIES)
    ) u_weight_loader (
        .clk, .rst_n, .cfg_valid(wl_cfg_valid), .cfg_weight_base(weight_base_q),
        .cfg_dw_mode(dw_mode_q),
        .cfg_c(c_q), .cfg_c_base(segment_c_base_q),
        .cfg_c_count(c_count_q),
        .cfg_kernel_h(kernel_h_q), .cfg_kernel_w(kernel_w_q),
        .cfg_oc(oc_q), .cfg_tile_width(tile_width_q),
        .cfg_single_tile_mode(1'b1), .cfg_oc_tile(tile_q),
        .start(wl_start), .busy(wl_busy), .done(wl_done), .cfg_error(wl_error),
        .sram_req_valid(wl_req_valid), .sram_req_addr(wl_req_addr),
        .sram_resp_valid(wl_resp_valid), .sram_resp_data(wl_resp_data),
        .weight_valid(wl_weight_valid), .weight_ready(wl_weight_ready),
        .weight_data(wl_weight_data), .weight_mask(wl_weight_mask),
        .weight_last_k(wl_last_k), .weight_last_tile(wl_last_tile)
    );

    gemmini_im2col_chw_gather_readable #(
        .BLOCK_SIZE(BLOCK_SIZE), .ELEM_W(ELEM_W), .SP_BANKS(SP_BANKS),
        .SP_BANK_ENTRIES(SP_BANK_ENTRIES), .GROUP_W(GROUP_W)
    ) u_im2col (
        .clk, .rst_n, .cfg_valid(im_cfg_valid), .cfg_spad_base(activation_base_q),
        .cfg_n(n_q), .cfg_c(c_q), .cfg_c_base(segment_c_base_q),
        .cfg_c_count(c_count_q), .cfg_single_group_mode(1'b1),
        .cfg_group_n(group_n_q), .cfg_group_index(group_index_q),
        .cfg_group_oh_base(group_oh_base_q),
        .cfg_group_ow_base(group_ow_base_q),
        .cfg_h(h_q), .cfg_w(w_q),
        .cfg_out_h(out_h_q), .cfg_out_w(out_w_q),
        .cfg_mline_mode(mline_mode_q),
        .cfg_rows_per_group(rows_per_group_q),
        .cfg_kernel_h(kernel_h_q), .cfg_kernel_w(kernel_w_q),
        .cfg_stride_h(stride_h_q), .cfg_stride_w(stride_w_q),
        .cfg_dilation_h(dilation_h_q), .cfg_dilation_w(dilation_w_q),
        .cfg_pad_top(pad_top_q), .cfg_pad_left(pad_left_q),
        .cfg_dw_mode(1'b0), .cfg_kernel_pattern(kernel_pattern_q),
        .cfg_groups_per_n(groups_per_n_q),
        .start(im_start), .busy(im_busy), .done(im_done), .cfg_error(im_error),
        .sram_req_valid(im_req_valid), .sram_req_addr(im_req_addr),
        .sram_resp_valid(im_resp_valid), .sram_resp_data(im_resp_data),
        .feed_valid(im_feed_valid), .feed_ready(im_feed_ready),
        .feed_data(im_feed_data), .feed_mask(im_feed_mask),
        .feed_row_mask(im_feed_row_mask), .feed_n_index(im_feed_n_index),
        .feed_group_index(im_feed_group_index),
        .feed_spatial_index(im_feed_spatial_index)
    );

    bias_loader #(
        .BLOCK_SIZE(BLOCK_SIZE), .SP_BANKS(SP_BANKS),
        .SP_BANK_ENTRIES(SP_BANK_ENTRIES)
    ) u_bias_loader (
        .clk, .rst_n, .cfg_valid(bl_cfg_valid), .cfg_bias_base(bias_base_q),
        .cfg_oc(oc_q), .cfg_tile_width(tile_width_q),
        .cfg_oc_tile(tile_q), .start(bl_start),
        .busy(bl_busy), .done(bl_done), .cfg_error(bl_error),
        .sram_req_valid(bl_req_valid), .sram_req_addr(bl_req_addr),
        .sram_resp_valid(bl_resp_valid), .sram_resp_data(bl_resp_data),
        .bias_valid(bl_bias_valid), .bias_ready(bl_bias_ready),
        .bias_data(bl_bias_data), .bias_mask(bl_bias_mask),
        .bias_oc_tile(bl_bias_oc_tile), .bias_last_tile(bl_bias_last_tile)
    );

    weight_reuse_buffer #(
        .BLOCK_SIZE(BLOCK_SIZE), .ELEM_W(ELEM_W), .MAX_C(MAX_C),
        .MAX_KERNEL(MAX_KERNEL), .MAX_K(MAX_K), .MEM_DEPTH(256)
    ) u_b_buffer (
        .clk, .rst_n, .fill_start(buf_fill_start),
        .fill_k_count(k_count_q), .fill_tile_id(tile_q),
        .fill_c_tile_id(c_tile_q),
        .fill_valid(wl_weight_valid && state == S_LOAD_B),
        .fill_ready(buf_fill_ready), .fill_data(wl_weight_data),
        .fill_mask(wl_weight_mask), .fill_last_k(wl_last_k),
        .activate(buf_activate),
        .activate_tile_id(tile_q), .activate_c_tile_id(c_tile_q),
        .release_active(buf_release), .read_valid(buf_read_valid),
        .read_ready(buf_read_ready), .read_data(buf_read_data),
        .read_mask(buf_read_mask), .read_k_index(buf_read_k),
        .read_last_k(buf_read_last_k), .read_tile_id(buf_read_tile),
        .read_c_tile_id(buf_read_c_tile),
        .protocol_error(buf_protocol_error)
    );

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            activation_base_q <= '0; weight_base_q <= '0; bias_base_q <= '0;
            n_q <= '0; c_q <= '0; h_q <= '0; w_q <= '0;
            out_h_q <= '0; out_w_q <= '0; kernel_h_q <= '0; kernel_w_q <= '0;
            stride_h_q <= '0; stride_w_q <= '0; dilation_h_q <= '0;
            dilation_w_q <= '0; pad_top_q <= '0; pad_left_q <= '0;
            kernel_pattern_q <= '0; oc_q <= '0;
            dw_mode_q <= 1'b0;
            mline_mode_q <= 1'b0;
            rows_per_group_q <= '0;
            cfg_groups_per_n_q <= '0;
        end else if (cfg_valid && !busy) begin
            dw_mode_q <= cfg_dw_mode;
            mline_mode_q <= cfg_mline_mode;
            rows_per_group_q <= cfg_rows_per_group;
            cfg_groups_per_n_q <= cfg_groups_per_n;
            activation_base_q <= cfg_activation_base;
            weight_base_q <= cfg_weight_base;
            bias_base_q <= cfg_bias_base;
            n_q <= cfg_n; c_q <= cfg_c; h_q <= cfg_h; w_q <= cfg_w;
            out_h_q <= cfg_out_h; out_w_q <= cfg_out_w;
            kernel_h_q <= cfg_kernel_h; kernel_w_q <= cfg_kernel_w;
            stride_h_q <= cfg_stride_h; stride_w_q <= cfg_stride_w;
            dilation_h_q <= cfg_dilation_h; dilation_w_q <= cfg_dilation_w;
            pad_top_q <= cfg_pad_top; pad_left_q <= cfg_pad_left;
            kernel_pattern_q <= cfg_kernel_pattern; oc_q <= cfg_oc;
        end
    end

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state <= S_IDLE;
            done <= 1'b0;
            cfg_error <= 1'b0;
            tile_q <= '0;
            c_tile_q <= '0;
            c_count_q <= '0;
            group_n_q <= '0;
            group_index_q <= '0;
            group_oh_base_q <= '0;
            group_ow_base_q <= '0;
            groups_per_n_q <= '0;
            k_count_q <= '0;
            pe_count_q <= '0;
            expected_k_q <= '0;
            bias_data_q <= '0;
            bias_mask_q <= '0;
            bias_tile_q <= '0;
            segment_c_base_q <= '0;
            segment_width_q <= '0;
            tile_width_q <= '0;
            tile_oc_base_q <= '0;
            first_c_count_q <= '0;
            first_k_count_q <= '0;
        end else begin
            done <= 1'b0;
            cfg_error <= 1'b0;
            case (state)
                S_IDLE: if (start) begin
                    tile_q <= '0;
                    c_tile_q <= '0;
                    group_n_q <= '0;
                    group_index_q <= '0;
                    group_oh_base_q <= '0;
                    group_ow_base_q <= '0;
                    pe_count_q <= '0;
                    expected_k_q <= '0;
                    state <= S_CFG_BASIC;
                end
                S_CFG_BASIC: begin
                    segment_width_q <= configured_segment_width;
                    tile_width_q <= dw_mode_q ?
                                    configured_segment_width :
                                    BLOCK_SIZE;
                    segment_c_base_q <= '0;
                    tile_oc_base_q <= '0;
                    if (c_q <= configured_segment_width)
                        first_c_count_q <= c_q;
                    else
                        first_c_count_q <= configured_segment_width;
                    state <= S_CFG_PRODUCT0;
                end
                S_CFG_PRODUCT0: begin
                    state <= S_CFG_PRODUCT1;
                end
                S_CFG_PRODUCT1: begin
                    first_k_count_q <= k_count_for_kernel(first_c_count_q, kernel_h_q);
                    state <= S_CFG_PRODUCT2;
                end
                S_CFG_PRODUCT2: begin
                    state <= S_CFG_COMMIT;
                end
                S_CFG_COMMIT: begin
                    c_count_q <= first_c_count_q;
                    groups_per_n_q <= cfg_groups_per_n_q;
                    k_count_q <= first_k_count_q;
                    state <= S_FILL_CFG;
                end
                S_FILL_CFG: state <= S_FILL_START;
                S_FILL_START: state <= S_LOAD_B;
                S_LOAD_B: begin
                    if (wl_error || buf_protocol_error) begin
                        cfg_error <= 1'b1; done <= 1'b1; state <= S_IDLE;
                    end else if (wl_done) begin
                        if ((dw_mode_q ||
                             segment_c_base_q + c_count_q >= c_q) &&
                            group_n_q == 0 && group_index_q == 0)
                            state <= S_BIAS_CFG;
                        else
                            state <= S_ACTIVATE;
                    end
                end
                S_BIAS_CFG: state <= S_BIAS_START;
                S_BIAS_START: state <= S_LOAD_BIAS;
                S_LOAD_BIAS: begin
                    if (bl_error) begin
                        cfg_error <= 1'b1; done <= 1'b1; state <= S_IDLE;
                    end else if (bl_bias_valid && bl_bias_ready) begin
                        if (bl_bias_oc_tile != tile_q ||
                            bl_bias_mask != expected_oc_mask(oc_q, tile_oc_base_q,
                                                             tile_width_q)) begin
                            cfg_error <= 1'b1; done <= 1'b1; state <= S_IDLE;
                        end else begin
                            bias_data_q <= bl_bias_data;
                            bias_mask_q <= bl_bias_mask;
                            bias_tile_q <= bl_bias_oc_tile;
                            state <= S_ACTIVATE;
                        end
                    end
                end
                S_ACTIVATE: state <= S_IM_CFG;
                S_IM_CFG: state <= S_IM_START;
                S_IM_START: state <= S_RUN_A;
                S_RUN_A: begin
                    if (im_error || buf_protocol_error) begin
                        cfg_error <= 1'b1; done <= 1'b1; state <= S_IDLE;
                    end else if (pe_valid && pe_ready) begin
                        if (pe_count_q + 1'b1 == k_count_q) begin
                            if ((dw_mode_q || c_q <= segment_width_q) &&
                                ((group_index_q + 1 < groups_per_n_q) ||
                                 (group_n_q + 1 < n_q)))
                                state <= S_NEXT_TILE;
                            else
                                state <= S_RELEASE;
                        end else begin
                            pe_count_q <= pe_count_q + 1'b1;
                            if (expected_k_q + 1 == k_count_q)
                                expected_k_q <= '0;
                            else
                                expected_k_q <= expected_k_q + 1'b1;
                        end
                    end
                end
                S_RELEASE: state <= S_NEXT_TILE;
                S_NEXT_TILE: begin
                    if (!dw_mode_q &&
                        segment_c_base_q + c_count_q < c_q) begin
                        c_tile_q <= c_tile_q + 1'b1;
                        segment_c_base_q <= next_segment_c_base[15:0];
                        c_count_q <= next_segment_c_count;
                        k_count_q <= next_segment_k_count;
                        pe_count_q <= '0;
                        expected_k_q <= '0;
                        state <= S_FILL_CFG;
                    end else if (tile_advance_ready) begin
                        c_tile_q <= '0;
                        pe_count_q <= '0;
                        expected_k_q <= '0;
                        if (!dw_mode_q) begin
                            segment_c_base_q <= '0;
                            c_count_q <= first_c_count_q;
                            k_count_q <= first_k_count_q;
                        end
                        if (group_index_q + 1 < groups_per_n_q) begin
                            group_index_q <= group_index_q + 1'b1;
                            if (mline_mode_q) begin
                                group_oh_base_q <= group_oh_base_q +
                                    {11'd0, rows_per_group_q};
                                group_ow_base_q <= '0;
                            end else if (group_ow_base_q + BLOCK_SIZE < out_w_q) begin
                                group_ow_base_q <= group_ow_base_q + BLOCK_SIZE;
                            end else begin
                                group_oh_base_q <= group_oh_base_q + 1'b1;
                                group_ow_base_q <= '0;
                            end
                            if (dw_mode_q || c_q <= segment_width_q)
                                state <= S_IM_CFG;
                            else
                                state <= S_FILL_CFG;
                        end else if (group_n_q + 1 < n_q) begin
                            group_n_q <= group_n_q + 1'b1;
                            group_index_q <= '0;
                            group_oh_base_q <= '0;
                            group_ow_base_q <= '0;
                            if (dw_mode_q || c_q <= segment_width_q)
                                state <= S_IM_CFG;
                            else
                                state <= S_FILL_CFG;
                        end else if (next_tile_oc_base < {1'b0, oc_q}) begin
                            tile_q <= tile_q + 1'b1;
                            tile_oc_base_q <= next_tile_oc_base;
                            if (dw_mode_q) begin
                                segment_c_base_q <= next_segment_c_base[15:0];
                                c_count_q <= next_segment_c_count;
                                k_count_q <= next_segment_k_count;
                            end
                            group_n_q <= '0;
                            group_index_q <= '0;
                            group_oh_base_q <= '0;
                            group_ow_base_q <= '0;
                            state <= S_FILL_CFG;
                        end else begin
                            state <= S_DONE;
                        end
                    end
                end
                S_DONE: begin done <= 1'b1; state <= S_IDLE; end
                default: state <= S_IDLE;
            endcase
        end
    end
endmodule

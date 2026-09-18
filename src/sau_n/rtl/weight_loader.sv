// Dense [C][KH][KW][OC] to tiled [K][OC] Weight Loader.
//
// The source weights are stored as one continuous INT8 byte stream:
//
//   k                 = ((c * kernel_h) + kh) * kernel_w + kw
//   weight_byte_addr  = cfg_weight_base + k * OC + oc
//   bank              = weight_byte_addr % SP_BANKS
//   row               = weight_byte_addr / SP_BANKS
//
// OC tiles are the outer traversal dimension. Within one OC tile, kw, kh and
// c advance from fastest to slowest. The target Scratchpad has 16 independent
// INT8 banks and a fixed one-cycle read response.

module weight_loader #(
    parameter int BLOCK_SIZE      = 16,
    parameter int ELEM_W          = 8,
    parameter int SP_BANKS        = BLOCK_SIZE,
    parameter int SP_BANK_ENTRIES = 4096,
    parameter int FIFO_DEPTH      = 4,
    parameter int SP_BANK_BITS    = $clog2(SP_BANKS),
    parameter int SP_ROW_BITS     = $clog2(SP_BANK_ENTRIES),
    parameter int SP_ADDR_BITS    = SP_BANK_BITS + SP_ROW_BITS,
    parameter int FIFO_PTR_W      = $clog2(FIFO_DEPTH),
    parameter int LANE_W          = $clog2(BLOCK_SIZE)
) (
    input  logic clk,
    input  logic rst_n,

    input  logic                    cfg_valid,
    // DW uses one selected OC tile, with c_base=oc_tile*cfg_tile_width and
    // at most BLOCK_SIZE channels.
    input  logic                    cfg_dw_mode,
    input  logic [SP_ADDR_BITS-1:0] cfg_weight_base,
    input  logic [15:0]             cfg_c,
    input  logic [15:0]             cfg_c_base,
    input  logic [15:0]             cfg_c_count,
    input  logic [3:0]              cfg_kernel_h,
    input  logic [3:0]              cfg_kernel_w,
    input  logic [15:0]             cfg_oc,
    // Valid output lanes in the selected tile. Dense mode uses BLOCK_SIZE;
    // DW mode may use a kernel-aware narrower tile.
    input  logic [15:0]             cfg_tile_width,
    // Compatibility-preserving tile-select mode. When disabled, one command
    // emits every OC tile as before. When enabled, only cfg_oc_tile is emitted
    // while dense K-to-K addressing still uses the full cfg_oc stride.
    input  logic                    cfg_single_tile_mode,
    input  logic [15:0]             cfg_oc_tile,

    input  logic start,
    output logic busy,
    output logic done,
    output logic cfg_error,

    output logic [SP_BANKS-1:0] sram_req_valid,
    output logic [SP_BANKS-1:0][SP_ROW_BITS-1:0] sram_req_addr,
    input  logic [SP_BANKS-1:0] sram_resp_valid,
    input  var logic [SP_BANKS-1:0][ELEM_W-1:0] sram_resp_data,

    output logic                         weight_valid,
    input  logic                         weight_ready,
    output logic [BLOCK_SIZE*ELEM_W-1:0] weight_data,
    output logic [BLOCK_SIZE-1:0]        weight_mask,
    output logic                         weight_last_k,
    output logic                         weight_last_tile
);

    typedef struct packed {
        logic valid;
        logic [SP_BANK_BITS-1:0] bank;
        logic [SP_ROW_BITS-1:0] row;
        logic [LANE_W-1:0] dst_lane;
    } lane_req_t;

    typedef enum logic [1:0] {
        ADDR_PREP_IDLE,
        ADDR_PREP_OFFSET,
        ADDR_PREP_BASE,
        ADDR_PREP_VECTOR
    } addr_prep_state_t;

    logic [SP_ADDR_BITS-1:0] weight_base_q;
    localparam int SP_CAPACITY_BYTES = SP_BANKS * SP_BANK_ENTRIES;
    localparam int CAPACITY_ADDR_W = (SP_CAPACITY_BYTES > 0) ?
                                      $clog2(SP_CAPACITY_BYTES + 1) : 1;
    // The runtime address also carries the 17-bit OC lane index and the
    // 16-bit configuration counters.  Use the larger of those fields,
    // capacity width, and row address width for non-default memories.
    localparam int ADDR_MATH_W =
        (CAPACITY_ADDR_W > SP_ADDR_BITS) ?
        ((CAPACITY_ADDR_W > 17) ? CAPACITY_ADDR_W : 17) :
        ((SP_ADDR_BITS > 17) ? SP_ADDR_BITS : 17);
    logic [15:0] c_q, c_base_q, c_count_q;
    logic [15:0] last_c_q;
    logic [3:0] kernel_h_q;
    logic [3:0] kernel_w_q;
    logic [15:0] oc_q;
    logic [4:0] kernel_area_q;
    logic single_tile_mode_q;
    logic dw_mode_q;
    logic [15:0] oc_tile_q;
    logic [15:0] tile_width_q;
    logic config_captured_q;

    addr_prep_state_t addr_prep_state;
    logic [ADDR_MATH_W-1:0] channel_start_k_q;
    logic [ADDR_MATH_W-1:0] segment_offset_q;
    logic [ADDR_MATH_W-1:0] segment_base_addr_q;
    logic [ADDR_MATH_W-1:0] tile_base_addr_q;
    logic [ADDR_MATH_W-1:0] vector_base_addr_q;
    logic [ADDR_MATH_W-1:0] vector_stride_q;

    logic producer_active;
    logic [15:0] oc_base_idx;
    logic [15:0] c_idx;
    logic [3:0] kh_idx;
    logic [3:0] kw_idx;
    logic cursor_last_k;
    logic cursor_last_tile;

    lane_req_t generated_req [BLOCK_SIZE];
    logic [BLOCK_SIZE-1:0] generated_mask;

    // Stage 1: generated dense addresses and boundary metadata.
    logic s1_valid;
    lane_req_t s1_req [BLOCK_SIZE];
    logic [BLOCK_SIZE-1:0] s1_mask;
    logic s1_last_k;
    logic s1_last_tile;

    // Stage 2: descriptor presented to the banked Scratchpad.
    logic s2_valid;
    lane_req_t s2_req [BLOCK_SIZE];
    logic [BLOCK_SIZE-1:0] s2_mask;
    logic s2_last_k;
    logic s2_last_tile;

    // Stage 3: one-cycle read response assembly.
    logic s3_valid;
    lane_req_t s3_req [BLOCK_SIZE];
    logic [BLOCK_SIZE-1:0] s3_pending;
    logic [BLOCK_SIZE*ELEM_W-1:0] s3_data;
    logic [BLOCK_SIZE-1:0] s3_mask;
    logic s3_last_k;
    logic s3_last_tile;
    logic [SP_BANKS-1:0] s3_bank_pending;
    logic [SP_BANKS-1:0][LANE_W-1:0] s3_bank_dst_lane;
    logic [BLOCK_SIZE-1:0] s3_pending_after_resp;
    logic [BLOCK_SIZE*ELEM_W-1:0] s3_data_after_resp;
    logic [SP_BANKS-1:0] s3_bank_pending_after_resp;
    logic s3_complete_after_resp;

    logic s1_to_s2;
    logic s2_to_s3;
    logic s3_ready;

    logic [BLOCK_SIZE*ELEM_W-1:0] fifo_data [FIFO_DEPTH];
    logic [BLOCK_SIZE-1:0] fifo_mask [FIFO_DEPTH];
    logic fifo_last_k [FIFO_DEPTH];
    logic fifo_last_tile [FIFO_DEPTH];
    logic [FIFO_PTR_W:0] fifo_count;
    logic [FIFO_PTR_W-1:0] fifo_rptr;
    logic [FIFO_PTR_W-1:0] fifo_wptr;
    logic fifo_push;
    logic fifo_pop;

    assign cursor_last_k =
        (kw_idx + 1 >= kernel_w_q) &&
        (kh_idx + 1 >= kernel_h_q) &&
        (c_idx == last_c_q);
    assign cursor_last_tile = (oc_base_idx + tile_width_q >= oc_q);

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            weight_base_q <= '0;
            c_q <= '0;
            c_base_q <= '0;
            c_count_q <= '0;
            last_c_q <= '0;
            kernel_h_q <= '0;
            kernel_w_q <= '0;
            oc_q <= '0;
            kernel_area_q <= '0;
            config_captured_q <= 1'b0;
            single_tile_mode_q <= 1'b0;
            dw_mode_q <= 1'b0;
            vector_stride_q <= '0;
            oc_tile_q <= '0;
            tile_width_q <= BLOCK_SIZE;
        end else if (cfg_valid && !busy) begin
            weight_base_q <= cfg_weight_base;
            c_q <= cfg_c;
            c_base_q <= cfg_c_base;
            c_count_q <= cfg_c_count;
            last_c_q <= cfg_c_count - 16'd1;
            kernel_h_q <= cfg_kernel_h;
            kernel_w_q <= cfg_kernel_w;
            oc_q <= cfg_oc;
            kernel_area_q <= {4'd0, cfg_kernel_h} * {4'd0, cfg_kernel_w};
            config_captured_q <= 1'b1;
            single_tile_mode_q <= cfg_single_tile_mode;
            dw_mode_q <= cfg_dw_mode;
            vector_stride_q <= cfg_dw_mode ? {{(ADDR_MATH_W-1){1'b0}}, 1'b1} :
                               {{(ADDR_MATH_W-16){1'b0}}, cfg_oc};
            oc_tile_q <= cfg_oc_tile;
            tile_width_q <= (cfg_tile_width != 0) ? cfg_tile_width : BLOCK_SIZE;
        end
    end

    // DW reads one compact weight into its matching column; other valid columns
    // are zero-filled. The output column mask stays constant for the whole tile.
    always_comb begin
        for (int lane = 0; lane < BLOCK_SIZE; lane++) begin
            logic [ADDR_MATH_W-1:0] byte_addr;
            logic [16:0] oc_index;

            oc_index = {1'b0, oc_base_idx} + lane;
            byte_addr =
                vector_base_addr_q +
                (dw_mode_q ? {ADDR_MATH_W{1'b0}} :
                 {{(ADDR_MATH_W-LANE_W){1'b0}}, lane[LANE_W-1:0]});
            generated_req[lane] = '0;
            generated_mask[lane] = (lane < tile_width_q) &&
                                   (oc_index < {1'b0, oc_q});
            generated_req[lane].valid = generated_mask[lane] &&
                                        (!dw_mode_q || c_idx == 16'(lane));
            generated_req[lane].bank = byte_addr[SP_BANK_BITS-1:0];
            generated_req[lane].row = byte_addr[SP_ADDR_BITS-1:SP_BANK_BITS];
            generated_req[lane].dst_lane = lane[LANE_W-1:0];
        end
    end

    // A stage-2 descriptor issues only when stage 3 can own its response.
    always_comb begin
        sram_req_valid = '0;
        sram_req_addr = '0;
        if (s2_to_s3) begin
            for (int lane = 0; lane < BLOCK_SIZE; lane++) begin
                if (s2_req[lane].valid) begin
                    sram_req_valid[s2_req[lane].bank] = 1'b1;
                    sram_req_addr[s2_req[lane].bank] = s2_req[lane].row;
                end
            end
        end
    end

    always_comb begin
        s3_pending_after_resp = s3_pending;
        s3_data_after_resp = s3_data;
        s3_bank_pending_after_resp = s3_bank_pending;
        for (int bank = 0; bank < SP_BANKS; bank++) begin
            if (sram_resp_valid[bank] && s3_bank_pending[bank]) begin
                s3_data_after_resp[s3_bank_dst_lane[bank]*ELEM_W +: ELEM_W] =
                    sram_resp_data[bank];
                s3_pending_after_resp[s3_bank_dst_lane[bank]] = 1'b0;
                s3_bank_pending_after_resp[bank] = 1'b0;
            end
        end
        s3_complete_after_resp = s3_valid &&
            (s3_bank_pending_after_resp == {SP_BANKS{1'b0}});
    end

    assign weight_valid = (fifo_count != 0);
    assign weight_data = fifo_data[fifo_rptr];
    assign weight_mask = fifo_mask[fifo_rptr];
    assign weight_last_k = fifo_last_k[fifo_rptr];
    assign weight_last_tile = fifo_last_tile[fifo_rptr];
    assign fifo_pop = weight_valid && weight_ready;
    assign fifo_push = s3_complete_after_resp &&
        ((fifo_count != FIFO_DEPTH) || fifo_pop);
    assign s3_ready = !s3_valid || fifo_push;
    assign s2_to_s3 = s2_valid && s3_ready;
    assign s1_to_s2 = s1_valid && (!s2_valid || s2_to_s3);
    assign busy = (addr_prep_state != ADDR_PREP_IDLE) ||
                  producer_active || s1_valid || s2_valid || s3_valid ||
                  (fifo_count != 0);

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            fifo_count <= '0;
            fifo_rptr <= '0;
            fifo_wptr <= '0;
        end else begin
            unique case ({fifo_push, fifo_pop})
                2'b10: begin
                    fifo_data[fifo_wptr] <= s3_data_after_resp;
                    fifo_mask[fifo_wptr] <= s3_mask;
                    fifo_last_k[fifo_wptr] <= s3_last_k;
                    fifo_last_tile[fifo_wptr] <= s3_last_tile;
                    fifo_wptr <= fifo_wptr + 1'b1;
                    fifo_count <= fifo_count + 1'b1;
                end
                2'b01: begin
                    fifo_rptr <= fifo_rptr + 1'b1;
                    fifo_count <= fifo_count - 1'b1;
                end
                2'b11: begin
                    fifo_data[fifo_wptr] <= s3_data_after_resp;
                    fifo_mask[fifo_wptr] <= s3_mask;
                    fifo_last_k[fifo_wptr] <= s3_last_k;
                    fifo_last_tile[fifo_wptr] <= s3_last_tile;
                    fifo_wptr <= fifo_wptr + 1'b1;
                    fifo_rptr <= fifo_rptr + 1'b1;
                end
                default: begin end
            endcase
        end
    end

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            done <= 1'b0;
            cfg_error <= 1'b0;
            addr_prep_state <= ADDR_PREP_IDLE;
            channel_start_k_q <= '0;
            segment_offset_q <= '0;
            segment_base_addr_q <= '0;
            tile_base_addr_q <= '0;
            vector_base_addr_q <= '0;
            producer_active <= 1'b0;
            oc_base_idx <= '0;
            c_idx <= '0;
            kh_idx <= '0;
            kw_idx <= '0;
            s1_valid <= 1'b0;
            s1_mask <= '0;
            s1_last_k <= 1'b0;
            s1_last_tile <= 1'b0;
            s2_valid <= 1'b0;
            s2_mask <= '0;
            s2_last_k <= 1'b0;
            s2_last_tile <= 1'b0;
            s3_valid <= 1'b0;
            s3_pending <= '0;
            s3_data <= '0;
            s3_mask <= '0;
            s3_last_k <= 1'b0;
            s3_last_tile <= 1'b0;
            s3_bank_pending <= '0;
            s3_bank_dst_lane <= '0;
            for (int lane = 0; lane < BLOCK_SIZE; lane++) begin
                s1_req[lane] <= '0;
                s2_req[lane] <= '0;
                s3_req[lane] <= '0;
            end
        end else begin
            done <= 1'b0;
            cfg_error <= 1'b0;

            if (start && !busy) begin
                if (!config_captured_q) begin
                    cfg_error <= 1'b1;
                    done <= 1'b1;
                end else begin
                    channel_start_k_q <=
                        {{(ADDR_MATH_W-16){1'b0}}, c_base_q} *
                        {{(ADDR_MATH_W-5){1'b0}}, kernel_area_q};
                    addr_prep_state <= ADDR_PREP_OFFSET;
                    if (single_tile_mode_q)
                        oc_base_idx <= oc_tile_q * tile_width_q;
                    else
                        oc_base_idx <= '0;
                    c_idx <= '0;
                    kh_idx <= '0;
                    kw_idx <= '0;
                end
            end

            unique case (addr_prep_state)
                ADDR_PREP_OFFSET: begin
                    segment_offset_q <= dw_mode_q ? channel_start_k_q :
                        channel_start_k_q * {{(ADDR_MATH_W-16){1'b0}}, oc_q};
                    addr_prep_state <= ADDR_PREP_BASE;
                end
                ADDR_PREP_BASE: begin
                    segment_base_addr_q <=
                        {{(ADDR_MATH_W-SP_ADDR_BITS){1'b0}}, weight_base_q} +
                        segment_offset_q;
                    addr_prep_state <= ADDR_PREP_VECTOR;
                end
                ADDR_PREP_VECTOR: begin
                    tile_base_addr_q <=
                        segment_base_addr_q +
                        (dw_mode_q ? {ADDR_MATH_W{1'b0}} :
                         {{(ADDR_MATH_W-16){1'b0}}, oc_base_idx});
                    vector_base_addr_q <=
                        segment_base_addr_q +
                        (dw_mode_q ? {ADDR_MATH_W{1'b0}} :
                         {{(ADDR_MATH_W-16){1'b0}}, oc_base_idx});
                    producer_active <= 1'b1;
                    addr_prep_state <= ADDR_PREP_IDLE;
                end
                default: begin end
            endcase

            // Completion is defined at the external stream handshake.
            if (fifo_pop && fifo_last_k[fifo_rptr] &&
                (fifo_last_tile[fifo_rptr] || single_tile_mode_q)) begin
                done <= 1'b1;
            end

            if (s2_to_s3) begin
                s3_valid <= 1'b1;
                s3_data <= '0;
                s3_mask <= s2_mask;
                s3_last_k <= s2_last_k;
                s3_last_tile <= s2_last_tile;
                s3_bank_pending <= '0;
                for (int lane = 0; lane < BLOCK_SIZE; lane++) begin
                    s3_req[lane] <= s2_req[lane];
                    s3_pending[lane] <= s2_req[lane].valid;
                end
                for (int lane = 0; lane < BLOCK_SIZE; lane++) begin
                    if (s2_req[lane].valid) begin
                        s3_bank_pending[s2_req[lane].bank] <= 1'b1;
                        s3_bank_dst_lane[s2_req[lane].bank] <= s2_req[lane].dst_lane;
                    end
                end
            end else if (fifo_push) begin
                s3_valid <= 1'b0;
                s3_bank_pending <= '0;
            end else if (s3_valid) begin
                s3_pending <= s3_pending_after_resp;
                s3_data <= s3_data_after_resp;
                s3_bank_pending <= s3_bank_pending_after_resp;
            end

            if (s1_to_s2) begin
                s2_valid <= 1'b1;
                s2_mask <= s1_mask;
                s2_last_k <= s1_last_k;
                s2_last_tile <= s1_last_tile;
                for (int lane = 0; lane < BLOCK_SIZE; lane++)
                    s2_req[lane] <= s1_req[lane];
            end else if (s2_to_s3) begin
                s2_valid <= 1'b0;
            end

            if (producer_active && (!s1_valid || s1_to_s2)) begin
                s1_valid <= 1'b1;
                s1_mask <= generated_mask;
                s1_last_k <= cursor_last_k;
                s1_last_tile <= cursor_last_tile;
                for (int lane = 0; lane < BLOCK_SIZE; lane++)
                    s1_req[lane] <= generated_req[lane];

                if (!cursor_last_k) begin
                    vector_base_addr_q <= vector_base_addr_q + vector_stride_q;
                end else if (!(cursor_last_tile || single_tile_mode_q)) begin
                    tile_base_addr_q <= tile_base_addr_q + BLOCK_SIZE;
                    vector_base_addr_q <= tile_base_addr_q + BLOCK_SIZE;
                end

                if (cursor_last_k &&
                    (cursor_last_tile || single_tile_mode_q)) begin
                    producer_active <= 1'b0;
                end else if (kw_idx + 1 < kernel_w_q) begin
                    kw_idx <= kw_idx + 1'b1;
                end else begin
                    kw_idx <= '0;
                    if (kh_idx + 1 < kernel_h_q) begin
                        kh_idx <= kh_idx + 1'b1;
                    end else begin
                        kh_idx <= '0;
                        if (c_idx != last_c_q) begin
                            c_idx <= c_idx + 1'b1;
                        end else begin
                            c_idx <= '0;
                            oc_base_idx <= oc_base_idx +
                                           (dw_mode_q ? tile_width_q : BLOCK_SIZE);
                        end
                    end
                end
            end else if (s1_to_s2) begin
                s1_valid <= 1'b0;
            end
        end
    end

endmodule

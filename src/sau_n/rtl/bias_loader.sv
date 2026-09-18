// Load one OC tile of dense signed INT16 little-endian bias values.
//
// Bias element address for global output channel oc:
//   low  = cfg_bias_base + oc*2
//   high = cfg_bias_base + oc*2 + 1
//
// A tile is a contiguous element range. Each request round covers at most one
// element per Scratchpad bank, so no dynamic request arbitration is required.
module bias_loader #(
    parameter int BLOCK_SIZE = 16,
    parameter int BIAS_W = 16,
    parameter int ELEM_W = 8,
    parameter int SP_BANKS = BLOCK_SIZE,
    parameter int SP_BANK_ENTRIES = 4096,
    parameter int SP_BANK_BITS = $clog2(SP_BANKS),
    parameter int SP_ROW_BITS = $clog2(SP_BANK_ENTRIES),
    parameter int SP_ADDR_BITS = SP_BANK_BITS + SP_ROW_BITS,
    parameter int LANE_W = $clog2(BLOCK_SIZE),
    parameter int REQ_COUNT = 2 * BLOCK_SIZE,
    parameter int REQ_W = $clog2(REQ_COUNT)
) (
    input  logic clk,
    input  logic rst_n,

    input  logic cfg_valid,
    input  logic [SP_ADDR_BITS-1:0] cfg_bias_base,
    input  logic [15:0] cfg_oc,
    input  logic [15:0] cfg_tile_width,
    input  logic [15:0] cfg_oc_tile,

    input  logic start,
    output logic busy,
    output logic done,
    output logic cfg_error,

    output logic [SP_BANKS-1:0] sram_req_valid,
    output logic [SP_BANKS-1:0][SP_ROW_BITS-1:0] sram_req_addr,
    input  logic [SP_BANKS-1:0] sram_resp_valid,
    input  var logic [SP_BANKS-1:0][ELEM_W-1:0] sram_resp_data,

    output logic bias_valid,
    input  logic bias_ready,
    output logic [BLOCK_SIZE*BIAS_W-1:0] bias_data,
    output logic [BLOCK_SIZE-1:0] bias_mask,
    output logic [15:0] bias_oc_tile,
    output logic bias_last_tile
);
    localparam int ELEMS_PER_BIAS = BIAS_W / ELEM_W;
    localparam int TILE_ELEMS = BLOCK_SIZE * ELEMS_PER_BIAS;
    localparam int ROUND_COUNT =
        (TILE_ELEMS + SP_BANKS - 1) / SP_BANKS;
    localparam int ROUND_W =
        (ROUND_COUNT <= 1) ? 1 : $clog2(ROUND_COUNT);
    localparam int VALID_LANE_W =
        (BLOCK_SIZE <= 1) ? 1 : $clog2(BLOCK_SIZE + 1);
    localparam int VALID_ELEM_W =
        (TILE_ELEMS <= 1) ? 1 : $clog2(TILE_ELEMS + 1);
    localparam int OC_CALC_W = 16 +
        ((BLOCK_SIZE <= 1) ? 0 : $clog2(BLOCK_SIZE));
    localparam int REQUEST_INDEX_W =
        (VALID_ELEM_W > SP_BANK_BITS + ROUND_W) ?
        VALID_ELEM_W : SP_BANK_BITS + ROUND_W;

    typedef enum logic [1:0] {
        S_IDLE,
        S_ISSUE,
        S_DRAIN,
        S_OUTPUT
    } state_t;

    state_t state;
    logic [SP_ADDR_BITS-1:0] bias_base_q;
    logic [15:0] oc_q;
    logic [15:0] oc_tile_q;
    logic [15:0] tile_width_q;
    logic config_captured_q;
    logic [BLOCK_SIZE-1:0] configured_mask_q;

    logic [SP_ADDR_BITS-1:0] tile_elem_base_q;
    logic [SP_BANK_BITS-1:0] tile_start_bank_q;
    logic [VALID_ELEM_W-1:0] valid_elem_count_q;
    logic [ROUND_W-1:0] last_request_round_q;
    logic [ROUND_W-1:0] request_round_q;

    logic issued_valid_q;
    logic [ROUND_W-1:0] issued_round_q;
    logic [SP_BANKS-1:0] issued_bank_mask_q;

    logic [ELEM_W-1:0] data_elem_q [TILE_ELEMS];
    logic [BLOCK_SIZE-1:0] mask_q;
    logic [15:0] output_tile_q;
    logic output_last_tile_q;

    logic [OC_CALC_W-1:0] cfg_input_oc_extended;
    logic [OC_CALC_W-1:0] cfg_input_tile_start_oc;
    logic [BLOCK_SIZE-1:0] cfg_input_mask;
    logic [OC_CALC_W-1:0] oc_extended;
    logic [OC_CALC_W-1:0] tile_start_oc;
    logic [63:0] tile_elem_base;
    logic [OC_CALC_W-1:0] remaining_lanes;
    logic [VALID_LANE_W-1:0] start_valid_lane_count;
    logic [VALID_ELEM_W-1:0] start_valid_elem_count;
    logic [ROUND_W-1:0] start_last_request_round;
    logic start_last_tile;
    logic issued_response_complete;

`ifndef SYNTHESIS
    initial begin
        if (BIAS_W % ELEM_W != 0)
            $error("bias_loader requires BIAS_W to be divisible by ELEM_W");
        if (SP_BANKS != (1 << SP_BANK_BITS))
            $error("bias_loader requires a power-of-two SP_BANKS");
    end
`endif

    assign cfg_input_oc_extended = cfg_oc;
    assign cfg_input_tile_start_oc = cfg_oc_tile *
                                     ((cfg_tile_width != 0) ? cfg_tile_width : BLOCK_SIZE);

    always_comb begin
        cfg_input_mask = '0;
        for (int lane = 0; lane < BLOCK_SIZE; lane++) begin
            if ((lane < ((cfg_tile_width != 0) ? cfg_tile_width : BLOCK_SIZE)) &&
                cfg_input_tile_start_oc + lane < cfg_input_oc_extended)
                cfg_input_mask[lane] = 1'b1;
        end
    end

    assign oc_extended = oc_q;
    assign tile_start_oc = oc_tile_q * tile_width_q;
    assign tile_elem_base = {48'd0, bias_base_q} +
                            tile_start_oc * ELEMS_PER_BIAS;
    assign remaining_lanes = (tile_start_oc < oc_extended) ?
                             oc_extended - tile_start_oc : '0;

    always_comb begin
        if (remaining_lanes >= tile_width_q)
            start_valid_lane_count = tile_width_q[VALID_LANE_W-1:0];
        else
            start_valid_lane_count = remaining_lanes[VALID_LANE_W-1:0];

        start_valid_elem_count =
            start_valid_lane_count * ELEMS_PER_BIAS;
        if (start_valid_elem_count != 0)
            start_last_request_round =
                (start_valid_elem_count - 1'b1) >> SP_BANK_BITS;
        else
            start_last_request_round = '0;

        start_last_tile = tile_start_oc + tile_width_q >= oc_extended;
    end

    assign busy = state != S_IDLE;
    assign bias_valid = state == S_OUTPUT;
    assign bias_mask = mask_q;
    assign bias_oc_tile = output_tile_q;
    assign bias_last_tile = output_last_tile_q;
    assign issued_response_complete = issued_valid_q &&
        ((sram_resp_valid & issued_bank_mask_q) == issued_bank_mask_q);

    always_comb begin
        bias_data = '0;
        for (int elem = 0; elem < TILE_ELEMS; elem++) begin
            bias_data[elem*ELEM_W +: ELEM_W] = data_elem_q[elem];
        end
    end

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            bias_base_q <= '0;
            oc_q <= '0;
            oc_tile_q <= '0;
            tile_width_q <= BLOCK_SIZE;
            config_captured_q <= 1'b0;
            configured_mask_q <= '0;
        end else if (cfg_valid && !busy) begin
            bias_base_q <= cfg_bias_base;
            oc_q <= cfg_oc;
            oc_tile_q <= cfg_oc_tile;
            tile_width_q <= (cfg_tile_width != 0) ? cfg_tile_width : BLOCK_SIZE;
            config_captured_q <= 1'b1;
            configured_mask_q <= cfg_input_mask;
        end
    end

    // Each bank independently maps to one logical element in the current
    // round. SP_BANK_BITS-bit subtraction implements the modulo rotation.
    always_comb begin
        sram_req_valid = '0;
        sram_req_addr = '0;
        if (state == S_ISSUE) begin
            for (int bank = 0; bank < SP_BANKS; bank++) begin
                logic [SP_BANK_BITS-1:0] bank_delta;
                logic [REQUEST_INDEX_W-1:0] elem_index;
                logic [SP_ADDR_BITS:0] elem_addr;

                bank_delta = bank[SP_BANK_BITS-1:0] - tile_start_bank_q;
                elem_index = request_round_q * SP_BANKS + bank_delta;
                elem_addr = {1'b0, tile_elem_base_q} + elem_index;
                if (elem_index < valid_elem_count_q) begin
                    sram_req_valid[bank] = 1'b1;
                    sram_req_addr[bank] =
                        elem_addr[SP_ADDR_BITS-1:SP_BANK_BITS];
                end
            end
        end
    end

    // Every output element has a fixed destination. Only its source bank is
    // rotated by the tile base, avoiding a bank/request response search.
    generate
        for (genvar elem = 0; elem < TILE_ELEMS; elem++) begin : g_data_elem
            localparam int ELEM_ROUND = elem / SP_BANKS;
            localparam int ELEM_SLOT = elem % SP_BANKS;
            localparam logic [ROUND_W-1:0] ELEM_ROUND_VALUE = ELEM_ROUND;
            localparam logic [SP_BANK_BITS-1:0] ELEM_SLOT_VALUE = ELEM_SLOT;
            logic [SP_BANK_BITS-1:0] response_bank;

            assign response_bank = tile_start_bank_q + ELEM_SLOT_VALUE;

            // A start before configuration capture never exposes stale bias_data.
            always_ff @(posedge clk or negedge rst_n) begin
                if (!rst_n) begin
                    data_elem_q[elem] <= '0;
                end else if (start && state == S_IDLE) begin
                    data_elem_q[elem] <= '0;
                end else if (issued_valid_q &&
                             issued_round_q == ELEM_ROUND_VALUE &&
                             issued_bank_mask_q[response_bank] &&
                             sram_resp_valid[response_bank]) begin
                    data_elem_q[elem] <= sram_resp_data[response_bank];
                end
            end
        end
    endgenerate

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state <= S_IDLE;
            done <= 1'b0;
            cfg_error <= 1'b0;
            tile_elem_base_q <= '0;
            tile_start_bank_q <= '0;
            valid_elem_count_q <= '0;
            last_request_round_q <= '0;
            request_round_q <= '0;
            issued_valid_q <= 1'b0;
            issued_round_q <= '0;
            issued_bank_mask_q <= '0;
            mask_q <= '0;
            output_tile_q <= '0;
            output_last_tile_q <= 1'b0;
        end else begin
            done <= 1'b0;
            cfg_error <= 1'b0;

            case (state)
                S_IDLE: begin
                    issued_valid_q <= 1'b0;
                    issued_bank_mask_q <= '0;
                    if (start) begin
                        // Command metadata is hidden unless state advances to
                        // ISSUE, so its load enable need not include the
                        // configuration-capture result.
                        tile_elem_base_q <=
                            tile_elem_base[SP_ADDR_BITS-1:0];
                        tile_start_bank_q <=
                            tile_elem_base[SP_BANK_BITS-1:0];
                        valid_elem_count_q <= start_valid_elem_count;
                        last_request_round_q <= start_last_request_round;
                        request_round_q <= '0;
                        mask_q <= configured_mask_q;
                        output_tile_q <= oc_tile_q;
                        output_last_tile_q <= start_last_tile;
                        if (!config_captured_q) begin
                            cfg_error <= 1'b1;
                            done <= 1'b1;
                        end else begin
                            state <= S_ISSUE;
                        end
                    end
                end

                S_ISSUE: begin
                    issued_valid_q <= |sram_req_valid;
                    issued_round_q <= request_round_q;
                    issued_bank_mask_q <= sram_req_valid;
                    if (request_round_q == last_request_round_q) begin
                        state <= S_DRAIN;
                    end else begin
                        request_round_q <= request_round_q + 1'b1;
                    end
                end

                S_DRAIN: begin
                    if (issued_response_complete) begin
                        issued_valid_q <= 1'b0;
                        issued_bank_mask_q <= '0;
                        state <= S_OUTPUT;
                    end
                end

                S_OUTPUT: begin
                    if (bias_ready) begin
                        done <= 1'b1;
                        state <= S_IDLE;
                    end
                end

                default: state <= S_IDLE;
            endcase
        end
    end
endmodule

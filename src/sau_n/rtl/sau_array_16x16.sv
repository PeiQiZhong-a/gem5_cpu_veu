// Four-row grouped, registered-B output-stationary SAU array.
//
// B advances one registered row group per cycle; A and masks are delayed to
// match. Bubbles advance with the data, preserving per-PE K order and II=1. A
// non-final segment leaves the accumulators in HOLD; the next command must be a
// matching retain command for the immediately following channel tile.  A final
// segment emits raw signed INT24 accumulators one valid OC column at a time.
module sau_array_16x16 #(
    parameter int BLOCK_SIZE = 16,
    parameter int ELEM_W = 8,
    parameter int ACC_W = 24,
    parameter int MAX_K = 250,
    parameter int K_W = $clog2(MAX_K + 1),
    parameter int N_W = 16,
    parameter int GROUP_W = 13,
    parameter int SPATIAL_W = 32,
    parameter int OC_TILE_W = 16,   //@16位多了
    parameter int C_TILE_W = 16,   //@16位多了
    parameter int LANE_W = $clog2(BLOCK_SIZE)
) (
    input  logic clk,
    input  logic rst_n,

    input  logic cmd_valid,
    output logic cmd_ready,
    input  logic cmd_clear_acc,
    input  logic cmd_finalize,
    input  logic [K_W-1:0] cmd_k_count,
    input  logic [C_TILE_W-1:0] cmd_c_tile,
    input  logic cmd_last_c_tile,
    input  logic [BLOCK_SIZE-1:0] cmd_row_mask,
    // Valid output columns are a nonzero, contiguous prefix starting at lane 0.
    input  logic [BLOCK_SIZE-1:0] cmd_col_mask,
    input  logic [N_W-1:0] cmd_n_index,
    input  logic [GROUP_W-1:0] cmd_group_index,
    input  logic [BLOCK_SIZE*SPATIAL_W-1:0] cmd_spatial_index,
    input  logic [OC_TILE_W-1:0] cmd_oc_tile,
    input  logic cmd_last_group,
    input  logic cmd_last_oc_tile,

    input  logic input_valid,
    output logic input_ready,
    input  logic [BLOCK_SIZE*ELEM_W-1:0] input_a_data,
    input  logic [BLOCK_SIZE-1:0] input_a_mask,
    input  logic [BLOCK_SIZE*ELEM_W-1:0] input_b_data,
    input  logic [BLOCK_SIZE-1:0] input_b_mask,
    input  logic [K_W-1:0] input_k_index,
    input  logic input_last_k,

    output logic result_valid,
    input  logic result_ready,
    output logic [BLOCK_SIZE*ACC_W-1:0] result_acc_data,
    output logic [LANE_W-1:0] result_oc_lane,
    output logic [BLOCK_SIZE-1:0] result_row_mask,
    output logic [BLOCK_SIZE*SPATIAL_W-1:0] result_spatial_index,
    output logic [N_W-1:0] result_n_index,
    output logic [GROUP_W-1:0] result_group_index,
    output logic [OC_TILE_W-1:0] result_oc_tile,
    output logic result_last_column,
    output logic result_last_command,

    output logic busy,
    output logic protocol_error
);
    typedef enum logic [2:0] {
        S_IDLE,
        S_STREAM,
        S_FLUSH,
        S_HOLD,
        S_VALIDATE_RETAIN,
        S_OUTPUT_COL
    } state_t;

    state_t state;

    logic [K_W-1:0] active_k_count;
    logic [K_W-1:0] accepted_k_count;
    logic active_finalize;
    logic [C_TILE_W-1:0] active_c_tile;
    logic [BLOCK_SIZE-1:0] active_row_mask;
    logic [BLOCK_SIZE-1:0] active_col_mask;
    logic [N_W-1:0] active_n_index;
    logic [GROUP_W-1:0] active_group_index;
    logic [BLOCK_SIZE*SPATIAL_W-1:0] active_spatial_index;
    logic [OC_TILE_W-1:0] active_oc_tile;
    logic active_last_group;
    logic active_last_oc_tile;
    logic identity_valid;
    logic [LANE_W-1:0] output_col;

    logic cmd_basic_protocol_valid;
    logic cmd_idle_protocol_valid;
    logic cmd_fire;
    logic input_fire;
    logic input_beat_ok;
    logic input_is_last_expected;
    logic pe_clear_acc;
    logic pe_input_valid;
    localparam int ROWS_PER_GROUP = 4;
    localparam int ROW_GROUPS = (BLOCK_SIZE + ROWS_PER_GROUP - 1) / ROWS_PER_GROUP;
    localparam int DRAIN_W = $clog2(ROW_GROUPS + 2);
    logic [DRAIN_W-1:0] drain_left;
    logic [ROW_GROUPS-1:0] group_valid;
    logic [BLOCK_SIZE*ELEM_W-1:0] group_b [ROW_GROUPS];
    logic [BLOCK_SIZE-1:0] group_b_mask [ROW_GROUPS];
    logic pipeline_active;
    assign pipeline_active = state == S_STREAM || state == S_FLUSH;

    logic [K_W-1:0] pending_k_count;
    logic pending_finalize;
    logic [C_TILE_W-1:0] pending_c_tile;
    logic [BLOCK_SIZE-1:0] pending_spatial_match;
    logic [7:0] pending_meta_match;

    logic signed [ACC_W-1:0] pe_acc [BLOCK_SIZE][BLOCK_SIZE];
    logic pe_product_pending [BLOCK_SIZE][BLOCK_SIZE];

    assign cmd_basic_protocol_valid = cmd_k_count != 0 && cmd_k_count <= MAX_K &&
                             cmd_row_mask != '0 && cmd_col_mask != '0 &&
                             cmd_finalize == cmd_last_c_tile;
    assign cmd_idle_protocol_valid = cmd_basic_protocol_valid &&
        cmd_clear_acc && cmd_c_tile == 0;

    assign cmd_ready = (state == S_IDLE) || (state == S_HOLD);
    assign cmd_fire = cmd_valid && cmd_ready;
    assign input_ready = (state == S_STREAM);
    assign input_fire = input_valid && input_ready;
    // The incremented value is also written back to accepted_k_count.  Keeping
    // the same expression here lets synthesis share the required 9-bit adder.
    assign input_is_last_expected =
        accepted_k_count + 1'b1 == active_k_count;
    assign input_beat_ok = input_k_index == accepted_k_count &&
                           input_last_k == input_is_last_expected &&
                           input_b_mask == active_col_mask;
    assign pe_clear_acc = cmd_fire && state == S_IDLE && cmd_idle_protocol_valid;
    assign pe_input_valid = input_fire && input_beat_ok;
    assign busy = state != S_IDLE;

    assign result_valid = (state == S_OUTPUT_COL);
    assign result_oc_lane = output_col;
    assign result_row_mask = active_row_mask;
    assign result_spatial_index = active_spatial_index;
    assign result_n_index = active_n_index;
    assign result_group_index = active_group_index;
    assign result_oc_tile = active_oc_tile;
    // Column masks are nonzero contiguous prefixes.  During output the active
    // mask shifts with each accepted column, so bit 1 directly says whether
    // another column remains after the current bit-0 column.
    assign result_last_column = (state == S_OUTPUT_COL) &&
                                !active_col_mask[1];
    assign result_last_command = result_last_column &&
                                 active_last_group && active_last_oc_tile;

    always_comb begin
        result_acc_data = '0;
        for (int row = 0; row < BLOCK_SIZE; row++)
            result_acc_data[row*ACC_W +: ACC_W] = pe_acc[row][output_col];
    end

    // Only the first register bank loads external B. Different temporal stages
    // cannot be merged as identical replicated registers by synthesis.
    generate
        for (genvar g = 0; g < ROW_GROUPS; g++) begin : g_b_group
            wire valid_in;
            wire [BLOCK_SIZE*ELEM_W-1:0] data_in;
            wire [BLOCK_SIZE-1:0] mask_in;
            if (g == 0) begin : g_first
                assign valid_in = pe_input_valid;
                assign data_in = input_b_data;
                assign mask_in = input_b_mask;
            end else begin : g_next
                assign valid_in = group_valid[g-1];
                assign data_in = group_b[g-1];
                assign mask_in = group_b_mask[g-1];
            end
            always_ff @(posedge clk or negedge rst_n) begin
                if (!rst_n) group_valid[g] <= 1'b0;
                else group_valid[g] <= pipeline_active && valid_in;
            end
            // Data is irrelevant without valid; no reset on payload registers.
            always_ff @(posedge clk) begin
                if (pipeline_active && valid_in) begin
                    group_b[g] <= data_in;
                    group_b_mask[g] <= mask_in;
                end
            end
        end
    endgenerate

    generate
        for (genvar row = 0; row < BLOCK_SIZE; row++) begin : g_row
            localparam int GROUP = row / ROWS_PER_GROUP;
            logic [ELEM_W-1:0] a_delay [GROUP+1];
            logic a_mask_delay [GROUP+1];
            for (genvar d = 0; d <= GROUP; d++) begin : g_a_delay
                if (d == 0) begin : g_first
                    always_ff @(posedge clk) begin
                        if (pe_input_valid) begin
                            a_delay[d] <= input_a_data[row*ELEM_W +: ELEM_W];
                            a_mask_delay[d] <= input_a_mask[row];
                        end
                    end
                end else begin : g_next
                    always_ff @(posedge clk) begin
                        if (pipeline_active && group_valid[d-1]) begin
                            a_delay[d] <= a_delay[d-1];
                            a_mask_delay[d] <= a_mask_delay[d-1];
                        end
                    end
                end
            end
            for (genvar col = 0; col < BLOCK_SIZE; col++) begin : g_col
                sau_pe #(
                    .ELEM_W(ELEM_W),
                    .ACC_W(ACC_W)
                ) u_pe (
                    .clk(clk),
                    .rst_n(rst_n),
                    .clear_acc(pe_clear_acc),
                    .input_valid(pipeline_active && group_valid[GROUP]),
                    .input_mac_enable(a_mask_delay[GROUP] && group_b_mask[GROUP][col]),
                    .input_a_data(a_delay[GROUP]),
                    .input_b_data(group_b[GROUP][col*ELEM_W +: ELEM_W]),
                    .product_pending(pe_product_pending[row][col]),
                    .accumulator(pe_acc[row][col])
                );
            end
        end
    endgenerate

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state <= S_IDLE;
            drain_left <= '0;
            active_k_count <= '0;
            accepted_k_count <= '0;
            active_finalize <= 1'b0;
            active_c_tile <= '0;
            active_row_mask <= '0;
            active_col_mask <= '0;
            active_n_index <= '0;
            active_group_index <= '0;
            active_spatial_index <= '0;
            active_oc_tile <= '0;
            active_last_group <= 1'b0;
            active_last_oc_tile <= 1'b0;
            identity_valid <= 1'b0;
            output_col <= '0;
            pending_k_count <= '0;
            pending_finalize <= 1'b0;
            pending_c_tile <= '0;
            pending_spatial_match <= '0;
            pending_meta_match <= '0;
            protocol_error <= 1'b0;
        end else begin
            protocol_error <= 1'b0;

            unique case (state)
                S_IDLE: begin
                    if (cmd_valid) begin
                        if (!cmd_idle_protocol_valid) begin
                            protocol_error <= 1'b1;
                        end else begin
                            active_k_count <= cmd_k_count;
                            accepted_k_count <= '0;
                            active_finalize <= cmd_finalize;
                            active_c_tile <= cmd_c_tile;
                            active_row_mask <= cmd_row_mask;
                            active_col_mask <= cmd_col_mask;
                            active_n_index <= cmd_n_index;
                            active_group_index <= cmd_group_index;
                            active_spatial_index <= cmd_spatial_index;
                            active_oc_tile <= cmd_oc_tile;
                            active_last_group <= cmd_last_group;
                            active_last_oc_tile <= cmd_last_oc_tile;
                            identity_valid <= 1'b1;
                            state <= S_STREAM;
                        end
                    end
                end

                S_STREAM: begin
                    if (input_valid) begin
                        if (!input_beat_ok) begin
                            protocol_error <= 1'b1;
                            identity_valid <= 1'b0;
                            accepted_k_count <= '0;
                            state <= S_IDLE;
                        end else begin
                            accepted_k_count <= accepted_k_count + 1'b1;
                            // input_beat_ok already proved that input_last_k
                            // matches the internally expected final beat.
                            if (input_last_k) begin
                                drain_left <= DRAIN_W'(ROW_GROUPS + 1);
                                state <= S_FLUSH;
                            end
                        end
                    end
                end

                S_FLUSH: begin
                    // Last group captures the product after ROW_GROUPS cycles;
                    // its accumulator commits one cycle later.
                    if (drain_left > 1) begin
                        drain_left <= drain_left - 1'b1;
                    end else if (active_finalize) begin
                        output_col <= '0;
                        state <= S_OUTPUT_COL;
                    end else begin
                        state <= S_HOLD;
                    end
                end

                S_HOLD: begin
                    if (cmd_valid) begin
                        if (!cmd_basic_protocol_valid || cmd_clear_acc) begin
                            protocol_error <= 1'b1;
                        end else begin
                            // A retain command has a wide identity.  Compare
                            // each spatial lane in parallel and register all
                            // match bits so the wide reduction is not on a
                            // one-cycle control path.
                            pending_k_count <= cmd_k_count;
                            pending_finalize <= cmd_finalize;
                            pending_c_tile <= cmd_c_tile;
                            for (int lane = 0; lane < BLOCK_SIZE; lane++)
                                pending_spatial_match[lane] <=
                                    cmd_spatial_index[lane*SPATIAL_W +: SPATIAL_W] ==
                                    active_spatial_index[lane*SPATIAL_W +: SPATIAL_W];
                            pending_meta_match[0] <= identity_valid;
                            pending_meta_match[1] <= cmd_n_index == active_n_index;
                            pending_meta_match[2] <= cmd_group_index == active_group_index;
                            pending_meta_match[3] <= cmd_row_mask == active_row_mask;
                            pending_meta_match[4] <= cmd_oc_tile == active_oc_tile;
                            pending_meta_match[5] <= cmd_col_mask == active_col_mask;
                            pending_meta_match[6] <= cmd_last_group == active_last_group &&
                                                     cmd_last_oc_tile == active_last_oc_tile;
                            pending_meta_match[7] <= cmd_c_tile == active_c_tile + 1'b1;
                            state <= S_VALIDATE_RETAIN;
                        end
                    end
                end

                S_VALIDATE_RETAIN: begin
                    if (&pending_spatial_match && &pending_meta_match) begin
                        active_k_count <= pending_k_count;
                        accepted_k_count <= '0;
                        active_finalize <= pending_finalize;
                        active_c_tile <= pending_c_tile;
                        state <= S_STREAM;
                    end else begin
                        protocol_error <= 1'b1;
                        state <= S_HOLD;
                    end
                end

                S_OUTPUT_COL: begin
                    if (result_ready) begin
                        if (result_last_column) begin
                            state <= S_IDLE;
                            identity_valid <= 1'b0;
                        end else begin
                            active_col_mask <= active_col_mask >> 1;
                            output_col <= output_col + 1'b1;
                        end
                    end
                end
            endcase
        end
    end
endmodule

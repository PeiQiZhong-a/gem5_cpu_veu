// Single local weight-reuse buffer for one 16-lane OC/channel tile.
//
// A fill command writes K vectors sequentially. The buffer becomes ready only
// after exactly fill_k_count vectors have been accepted and the final vector
// carries fill_last_k. Once activated, its read stream repeats K0..Klast until
// released so one resident tile can serve multiple spatial groups.
module weight_reuse_buffer #(
    parameter int BLOCK_SIZE = 16,
    parameter int ELEM_W     = 8,
    parameter int MAX_C      = 16,
    parameter int MAX_KERNEL = 5,
    parameter int MAX_K      = 250,
    parameter int MEM_DEPTH  = 256,
    parameter int K_W        = $clog2(MAX_K + 1)
) (
    input  logic clk,
    input  logic rst_n,

    input  logic             fill_start,
    input  logic [K_W-1:0]   fill_k_count,
    input  logic [15:0]      fill_tile_id,
    input  logic [15:0]      fill_c_tile_id,
    input  logic             fill_valid,
    output logic             fill_ready,
    input  logic [BLOCK_SIZE*ELEM_W-1:0] fill_data,
    input  logic [BLOCK_SIZE-1:0]        fill_mask,
    input  logic             fill_last_k,

    input  logic             activate,
    input  logic [15:0]      activate_tile_id,
    input  logic [15:0]      activate_c_tile_id,
    input  logic             release_active,

    output logic             read_valid,
    input  logic             read_ready,
    output logic [BLOCK_SIZE*ELEM_W-1:0] read_data,
    output logic [BLOCK_SIZE-1:0]        read_mask,
    output logic [K_W-1:0]               read_k_index,
    output logic                         read_last_k,
    output logic [15:0]                  read_tile_id,
    output logic [15:0]                  read_c_tile_id,

    output logic       protocol_error
);
    // One logical 1RW memory bank.  Each word is the complete B vector for one
    // K index (16 INT8 lanes = 128 bits with the default parameters).
    logic filling_q;
    logic ready_q;
    logic active_q;
    logic [K_W-1:0] k_count_q;
    logic [K_W-1:0] write_count_q;
    logic [BLOCK_SIZE-1:0] buffer_mask_q;
    logic [15:0] tile_id_q;
    logic [15:0] c_tile_id_q;

    logic [K_W-1:0] fetch_index;
    logic out_valid;
    logic [BLOCK_SIZE*ELEM_W-1:0] out_data;
    logic [BLOCK_SIZE-1:0] out_mask;
    logic [K_W-1:0] out_k_index;
    logic out_last_k;
    logic [15:0] out_tile_id;
    logic [15:0] out_c_tile_id;
    logic mem_write_en;
    logic mem_read_en;
    logic mem_access_en;
    logic [K_W-1:0] mem_addr;
    logic [K_W-1:0] write_count_d;

    assign fill_ready = filling_q && (write_count_q < k_count_q);
    assign read_valid = out_valid;
    assign read_data = out_data;
    assign read_mask = out_mask;
    assign read_k_index = out_k_index;
    assign read_last_k = out_last_k;
    assign read_tile_id = out_tile_id;
    assign read_c_tile_id = out_c_tile_id;
    assign mem_write_en = rst_n && fill_valid && fill_ready;
    assign mem_read_en = rst_n && active_q && !release_active &&
                         (!out_valid || read_ready);
    assign mem_access_en = mem_write_en || mem_read_en;
    assign mem_addr = mem_write_en ? write_count_q : fetch_index;

    // Keep the write counter on a data path instead of a feedback clock-enable
    // path.  The counter is reset for every fill and advances for every
    // accepted vector, including the final vector.
    always_comb begin
        write_count_d = write_count_q;
        if (fill_start && !filling_q && !ready_q && !active_q &&
            fill_k_count != 0 && fill_k_count <= MAX_K)
            write_count_d = '0;
        else if (mem_write_en)
            write_count_d = write_count_q + 1'b1;
    end

    // LOAD_B and RUN_A are mutually exclusive, so the shared 1RW port performs
    // at most one access per cycle.  The wrapper preserves the existing
    // rising-edge request and one-cycle synchronous read contract.
    weight_reuse_sram_1rw #(
        .DATA_W (BLOCK_SIZE * ELEM_W),
        .DEPTH  (MEM_DEPTH),
        .ADDR_W (K_W)
    ) u_data_mem (
        .clk        (clk),
        .en         (mem_access_en),
        .write_en   (mem_write_en),
        .addr       (mem_addr),
        .write_data (fill_data),
        .read_data  (out_data)
    );

    // The elastic payload is meaningful only while out_valid is asserted, so
    // these registers do not need reset.  Keeping them out of the async-reset
    // control block also keeps the wrapper read-data path free of reset logic.
    always_ff @(posedge clk) begin
        if (mem_read_en) begin
            out_mask <= buffer_mask_q;
            out_k_index <= fetch_index;
            out_last_k <= (fetch_index + 1 == k_count_q);
            out_tile_id <= tile_id_q;
            out_c_tile_id <= c_tile_id_q;
        end
    end

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            filling_q <= 1'b0;
            ready_q <= 1'b0;
            active_q <= 1'b0;
            k_count_q <= '0;
            write_count_q <= '0;
            buffer_mask_q <= '0;
            tile_id_q <= '0;
            c_tile_id_q <= '0;
            protocol_error <= 1'b0;
            out_valid <= 1'b0;
            fetch_index <= '0;
        end else begin
            protocol_error <= 1'b0;
            write_count_q <= write_count_d;

            if (fill_start) begin
                if (filling_q || ready_q || active_q ||
                    fill_k_count == 0 || fill_k_count > MAX_K) begin
                    protocol_error <= 1'b1;
                end else begin
                    filling_q <= 1'b1;
                    ready_q <= 1'b0;
                    k_count_q <= fill_k_count;
                    buffer_mask_q <= '0;
                    tile_id_q <= fill_tile_id;
                    c_tile_id_q <= fill_c_tile_id;
                end
            end

            if (fill_valid && !fill_ready)
                protocol_error <= 1'b1;

            if (mem_write_en) begin
                if (write_count_q == 0)
                    buffer_mask_q <= fill_mask;
                else if (fill_mask != buffer_mask_q)
                    protocol_error <= 1'b1;

                if (write_count_q + 1 == k_count_q) begin
                    filling_q <= 1'b0;
                    if (fill_last_k) begin
                        ready_q <= 1'b1;
                    end else begin
                        ready_q <= 1'b0;
                        protocol_error <= 1'b1;
                    end
                end else begin
                    if (fill_last_k) begin
                        filling_q <= 1'b0;
                        ready_q <= 1'b0;
                        protocol_error <= 1'b1;
                    end
                end
            end

            if (activate) begin
                if (active_q || !ready_q || tile_id_q != activate_tile_id ||
                    c_tile_id_q != activate_c_tile_id) begin
                    protocol_error <= 1'b1;
                end else begin
                    active_q <= 1'b1;
                    ready_q <= 1'b0;
                    fetch_index <= '0;
                    out_valid <= 1'b0;
                end
            end

            // Metadata advances on the same enabled edge as the synchronous
            // memory read above.
            if (mem_read_en) begin
                out_valid <= 1'b1;
                if (fetch_index + 1 == k_count_q)
                    fetch_index <= '0;
                else
                    fetch_index <= fetch_index + 1'b1;
            end

            if (release_active) begin
                if (!active_q) begin
                    protocol_error <= 1'b1;
                end else begin
                    active_q <= 1'b0;
                    out_valid <= 1'b0;
                    fetch_index <= '0;
                end
            end
        end
    end
endmodule

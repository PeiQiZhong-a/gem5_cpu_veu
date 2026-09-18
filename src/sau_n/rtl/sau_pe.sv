// One output-stationary processing element for the broadcast SAU array.
//
// Stage P captures one signed INT8 product for every accepted array input.
// Stage A commits the preceding product into a signed INT24 accumulator.  The
// accumulator saturates after every enabled MAC, which is part of the numeric
// contract and is intentionally different from saturating only at segment end.
module sau_pe #(
    parameter int ELEM_W = 8,
    parameter int ACC_W  = 24
) (
    input  logic clk,
    input  logic rst_n,

    input  logic clear_acc,
    input  logic input_valid,
    input  logic input_mac_enable,
    input  logic signed [ELEM_W-1:0] input_a_data,
    input  logic signed [ELEM_W-1:0] input_b_data,

    output logic product_pending,
    output logic signed [ACC_W-1:0] accumulator
);
    localparam int PRODUCT_W = 2 * ELEM_W;

    logic signed [PRODUCT_W-1:0] product_q;
    logic product_valid_q;
    logic product_enable_q;

    function automatic logic signed [ACC_W-1:0] sat_acc_add(
        input logic signed [ACC_W-1:0] acc,
        input logic signed [PRODUCT_W-1:0] product
    );
        logic signed [ACC_W-1:0] product_ext;
        logic signed [ACC_W-1:0] sum_normal;
        logic positive_overflow;
        logic negative_overflow;
        begin
            product_ext = $signed({{(ACC_W-PRODUCT_W){product[PRODUCT_W-1]}},
                                    product});
            sum_normal = acc + product_ext;

            // Signed overflow is fully determined by the operand/result signs.
            // This preserves per-MAC saturation without wide limit comparators.
            positive_overflow = !acc[ACC_W-1] && !product_ext[ACC_W-1] &&
                                sum_normal[ACC_W-1];
            negative_overflow = acc[ACC_W-1] && product_ext[ACC_W-1] &&
                                !sum_normal[ACC_W-1];

            if (positive_overflow)
                sat_acc_add = {1'b0, {(ACC_W-1){1'b1}}};
            else if (negative_overflow)
                sat_acc_add = {1'b1, {(ACC_W-1){1'b0}}};
            else
                sat_acc_add = sum_normal;
        end
    endfunction

    assign product_pending = product_valid_q;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            product_q <= '0;
            product_valid_q <= 1'b0;
            product_enable_q <= 1'b0;
            accumulator <= '0;
        end else begin
            // clear_acc has priority over a product already waiting in Stage P.
            // A new input may still be captured and will commit on the next cycle.
            if (clear_acc)
                accumulator <= '0;
            else if (product_valid_q && product_enable_q)
                accumulator <= sat_acc_add(accumulator, product_q);

            product_valid_q <= input_valid;
            if (input_valid) begin
                product_q <= $signed(input_a_data) * $signed(input_b_data);
                product_enable_q <= input_mac_enable;
            end else begin
                product_enable_q <= 1'b0;
            end
        end
    end
endmodule

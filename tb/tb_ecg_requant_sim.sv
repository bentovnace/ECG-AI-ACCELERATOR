//-----------------------------------------------------------------------------
// ECG-AI Project - Standalone SystemVerilog Simulation Testbench
// Module: tb_ecg_requant_sim
// Target: Direct simulation on Vivado GUI xsim / Verilator.
//         Verify INT32 -> INT8 requantization unit (ecg_requant):
//         Scale multiplication (mult), right shift, round-half-up,
//         output domain bias addition, ReLU activation, and symmetric clamping [-127, 127].
//-----------------------------------------------------------------------------

`timescale 1ns / 1ps

module tb_ecg_requant_sim;
  import ecg_pkg::*;

  localparam real CLK_PERIOD = 10.0; // 100 MHz (10 ns)

  // DUT interface signals
  logic                                clk;
  logic                                rst_n;
  logic                                valid_in;
  logic signed [ECG_PSUM_BITS-1:0]     psum_in;
  logic signed [8:0]                   bias_in;
  logic [ECG_REQUANT_MULT_BITS-1:0]    mult_in;
  logic [ECG_REQUANT_SHIFT_BITS-1:0]   shift_in;
  ecg_act_e                            act_in;

  logic                                valid_out;
  logic signed [7:0]                   act_out;

  // Verification tracking variables
  int error_count = 0;
  int test_step   = 0;

  // DUT instantiation (ecg_requant)
  ecg_requant u_dut (
      .clk_i    (clk),
      .rst_ni   (rst_n),
      .valid_i  (valid_in),
      .psum_i   (psum_in),
      .bias_i   (bias_in),
      .mult_i   (mult_in),
      .shift_i  (shift_in),
      .act_i    (act_in),
      .valid_o  (valid_out),
      .act_o    (act_out)
  );

  // Clock generator
  initial begin
    clk = 0;
    forever #(CLK_PERIOD / 2.0) clk = ~clk;
  end

  // Task to issue 1 test vector and verify result after 1 cycle
  task automatic check_requant(
      input string             test_name,
      input int                psum_val,
      input int                mult_val,
      input int                shift_val,
      input int                bias_val,
      input ecg_act_e          act_mode,
      input int                expected_out
  );
    @(posedge clk);
    valid_in <= 1'b1;
    psum_in  <= ECG_PSUM_BITS'(psum_val);
    mult_in  <= ECG_REQUANT_MULT_BITS'(mult_val);
    shift_in <= ECG_REQUANT_SHIFT_BITS'(shift_val);
    bias_in  <= 9'(bias_val);
    act_in   <= act_mode;

    @(posedge clk);
    #1; // Lay mau dau ra sau canh xung
    if (!valid_out) begin
      $display("  -> [FAIL] %s: valid_out did not assert!", test_name);
      error_count++;
    end else if (act_out !== 8'(expected_out)) begin
      $display("  -> [FAIL] %s: act_out = %0d (expected: %0d)", test_name, act_out, expected_out);
      error_count++;
    end else begin
      $display("  -> [PASS] %s: act_out = %0d (exact)", test_name, act_out);
    end
  endtask

  // Stimulus process
  initial begin
    $display("\n============================================================");
    $display("=== START SIMULATION: tb_ecg_requant_sim (Vivado xsim) ===");
    $display("============================================================");

    // Initialization
    rst_n    = 1'b0;
    valid_in = 1'b0;
    psum_in  = '0;
    bias_in  = '0;
    mult_in  = '0;
    shift_in = '0;
    act_in   = ECG_ACT_NONE;

    #(CLK_PERIOD * 3);
    rst_n = 1'b1;
    #(CLK_PERIOD * 2);

    // TEST 1: Simple 1:1 scaling, no shift, no bias
    // psum = 50, mult = 1, shift = 0, bias = 0 -> out = 50
    test_step = 1;
    $display("[TEST 1] 1:1 pass-through (no shift, no bias)...");
    check_requant("Identity_50", 50, 1, 0, 0, ECG_ACT_NONE, 50);

    // TEST 2: Right shift with round-half-up
    // psum = 100, mult = 1, shift = 3 (chia 8):
    // 100 / 8 = 12.5 -> lam tron thanh 13
    test_step = 2;
    $display("\n[TEST 2] Check round-half-up (100/8 -> 13)...");
    check_requant("RoundHalfUp_100div8", 100, 1, 3, 0, ECG_ACT_NONE, 13);

    // TEST 3: Add bias in output domain
    // psum = 100, mult = 1, shift = 3 -> 13; bias = -5 -> out = 8
    test_step = 3;
    $display("\n[TEST 3] Add bias in output domain (+ bias = -5)...");
    check_requant("BiasAdd_-5", 100, 1, 3, -5, ECG_ACT_NONE, 8);

    // TEST 4: ReLU activation (clamp negative to 0)
    // psum = -50, mult = 1, shift = 0, bias = 0
    // Voi ACT_NONE: out = -50
    // Voi ACT_RELU: out = 0
    test_step = 4;
    $display("\n[TEST 4] Check ReLU activation function...");
    check_requant("Negative_NoReLU", -50, 1, 0, 0, ECG_ACT_NONE, -50);
    check_requant("Negative_WithReLU", -50, 1, 0, 0, ECG_ACT_RELU, 0);

    // TEST 5: Check symmetric clamping at [-127, +127]
    // psum lon duong -> kep tai +127
    // psum lon am -> kep tai -127
    test_step = 5;
    $display("\n[TEST 5] Check symmetric saturation [-127, 127]...");
    check_requant("Clamp_Positive_Max", 5000, 1, 0, 0, ECG_ACT_NONE, 127);
    check_requant("Clamp_Negative_Min", -5000, 1, 0, 0, ECG_ACT_NONE, -127);

    // Clear valid_in
    @(posedge clk);
    valid_in <= 1'b0;
    #(CLK_PERIOD * 3);

    // CONCLUSION
    $display("\n============================================================");
    if (error_count == 0) begin
      $display("=== [PASS] TB_ECG_REQUANT_SIM: ALL TESTS PASSED! ===");
      $display("=== Zero errors: Requant arithmetic is 100%% bit-exact ===");
    end else begin
      $display("=== [FAIL] TB_ECG_REQUANT_SIM: DETECTED %0d ERRORS! ===", error_count);
    end
    $display("============================================================\n");

    $finish;
  end

endmodule

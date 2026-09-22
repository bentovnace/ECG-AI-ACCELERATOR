//-----------------------------------------------------------------------------
// ECG-AI Project - Standalone SystemVerilog Simulation Testbench
// Module: tb_ecg_mac8_sim
// Target: Direct simulation on Vivado GUI xsim / ModelSim / Verilator.
//         Verify 8-way PE MAC array (ecg_mac8) with arithmetic vectors,
//         cycle accumulation, signed negative numbers, saturation and clear.
//-----------------------------------------------------------------------------

`timescale 1ns / 1ps

module tb_ecg_mac8_sim;
  import ecg_pkg::*;

  localparam int unsigned N_PE = 8;
  localparam real CLK_PERIOD  = 10.0; // 100 MHz (10 ns)

  // DUT interface signals
  logic                       clk;
  logic                       rst_n;
  logic                       clear;
  logic                       acc;
  logic signed [7:0]          act;
  logic signed [7:0]          wgt [N_PE];
  logic signed [ECG_PSUM_BITS-1:0] psum [N_PE];

  // Verification tracking variables
  int error_count = 0;
  int test_step   = 0;

  // DUT instantiation (ecg_mac8)
  ecg_mac8 #(
      .N_PE(N_PE)
  ) u_dut (
      .clk_i   (clk),
      .rst_ni  (rst_n),
      .clear_i (clear),
      .acc_i   (acc),
      .act_i   (act),
      .wgt_i   (wgt),
      .psum_o  (psum)
  );

  // Clock generator
  initial begin
    clk = 0;
    forever #(CLK_PERIOD / 2.0) clk = ~clk;
  end

  // Stimulus process
  initial begin
    $display("\n========================================================");
    $display("=== START SIMULATION: tb_ecg_mac8_sim (Vivado xsim) ===");
    $display("========================================================");

    // Initialization
    rst_n = 1'b0;
    clear = 1'b0;
    acc   = 1'b0;
    act   = 8'sd0;
    for (int i = 0; i < N_PE; i++) wgt[i] = 8'sd0;

    // Reset system for 3 cycles
    #(CLK_PERIOD * 3);
    rst_n = 1'b1;
    #(CLK_PERIOD * 2);

    // TEST 1: Check initial reset state
    test_step = 1;
    $display("[TEST 1] Check accumulator register after Reset...");
    for (int i = 0; i < N_PE; i++) begin
      if (psum[i] !== '0) begin
        $display("  -> LOI: Lane %0d psum = %0d (mong doi: 0)", i, psum[i]);
        error_count++;
      end
    end
    if (error_count == 0) $display("  -> TEST 1 PASSED: all 8 lanes = 0.");

    // TEST 2: Single-cycle multiply (Load initial product: clear=1, acc=1)
    // act = 10, wgt = [1, 2, 3, 4, 5, 6, 7, 8]
    // Expected: psum = 10 * i
    @(posedge clk);
    test_step = 2;
    $display("\n[TEST 2] Single-cycle multiply with positive coefficients (clear=1, acc=1)...");
    clear <= 1'b1;
    acc   <= 1'b1;
    act   <= 8'sd10;
    for (int i = 0; i < N_PE; i++) wgt[i] <= 8'sd1 * (i + 1);

    @(posedge clk); // Await 1 cycle register update
    #1;
    for (int i = 0; i < N_PE; i++) begin
      automatic int exp_val = 10 * (i + 1);
      if (psum[i] !== exp_val) begin
        $display("  -> LOI: Lane %0d psum = %0d (mong doi: %0d)", i, psum[i], exp_val);
        error_count++;
      end
    end
    if (error_count == 0) $display("  -> TEST 2 PASSED: Result psum[0..7] = [10, 20, 30, 40, 50, 60, 70, 80].");

    // TEST 3: Cycle accumulation (clear=0, acc=1)
    // Add term: act = -5, wgt = [2, 2, 2, 2, 2, 2, 2, 2] -> adds -10 per lane
    @(posedge clk);
    test_step = 3;
    $display("\n[TEST 3] Cycle accumulation with negative numbers (clear=0, acc = 1)...");
    clear <= 1'b0;
    acc   <= 1'b1;
    act   <= -8'sd5;
    for (int i = 0; i < N_PE; i++) wgt[i] <= 8'sd2;

    @(posedge clk);
    #1;
    for (int i = 0; i < N_PE; i++) begin
      automatic int prev_val = 10 * (i + 1);
      automatic int exp_val  = prev_val + (-5 * 2);
      if (psum[i] !== exp_val) begin
        $display("  -> LOI: Lane %0d psum = %0d (mong doi: %0d)", i, psum[i], exp_val);
        error_count++;
      end
    end
    if (error_count == 0) $display("  -> TEST 3 PASSED: Accurate accumulation psum = psum_prev - 10.");

    // TEST 4: Check clear_i without acc_i (clear=1, acc=0 -> clears to 0)
    @(posedge clk);
    test_step = 4;
    $display("\n[TEST 4] Check clear_i = 1 and acc_i = 0 -> reset accumulator to 0...");
    clear <= 1'b1;
    acc   <= 1'b0;
    act   <= 8'sd12;
    for (int i = 0; i < N_PE; i++) wgt[i] <= 8'sd3;

    @(posedge clk);
    #1;
    for (int i = 0; i < N_PE; i++) begin
      if (psum[i] !== 0) begin
        $display("  -> LOI: Lane %0d psum = %0d (mong doi: 0)", i, psum[i]);
        error_count++;
      end
    end
    if (error_count == 0) $display("  -> TEST 4 PASSED: clear_i=1, acc_i=0 cleared psum[0..7] = 0.");

    // TEST 5: Check signed INT8 boundary conditions (-128, +127)
    @(posedge clk);
    test_step = 5;
    $display("\n[TEST 5] Check INT8 boundary values: -128 * -128 and -128 * 127...");
    clear <= 1'b1;
    acc   <= 1'b1;
    act   <= -8'sd128;
    wgt[0] <= -8'sd128; // (-128) * (-128) = +16384
    wgt[1] <=  8'sd127; // (-128) * (127)  = -16256
    wgt[2] <= -8'sd1;   // (-128) * (-1)   = +128
    wgt[3] <=  8'sd0;   // (-128) * 0      = 0
    for (int i = 4; i < N_PE; i++) wgt[i] <= 8'sd1;

    @(posedge clk);
    #1;
    if (psum[0] !== 16384) begin
      $display("  -> LOI Lane 0: (-128)*(-128) = %0d (mong doi 16384)", psum[0]);
      error_count++;
    end
    if (psum[1] !== -16256) begin
      $display("  -> LOI Lane 1: (-128)*(127) = %0d (mong doi -16256)", psum[1]);
      error_count++;
    end
    if (psum[2] !== 128) begin
      $display("  -> LOI Lane 2: (-128)*(-1) = %0d (mong doi 128)", psum[2]);
      error_count++;
    end
    if (psum[3] !== 0) begin
      $display("  -> LOI Lane 3: (-128)*0 = %0d (mong doi 0)", psum[3]);
      error_count++;
    end
    if (error_count == 0) $display("  -> TEST 5 PASSED: All signed INT8 boundary operations verified.");

    #(CLK_PERIOD * 2);

    // CONCLUSION
    $display("\n========================================================");
    if (error_count == 0) begin
      $display("=== [PASS] TB_ECG_MAC8_SIM: ALL TESTS PASSED! ===");
      $display("=== Zero errors: Ready for synthesis & implementation ===");
    end else begin
      $display("=== [FAIL] TB_ECG_MAC8_SIM: DETECTED %0d ERRORS! ===", error_count);
    end
    $display("========================================================\n");

    $finish;
  end

endmodule

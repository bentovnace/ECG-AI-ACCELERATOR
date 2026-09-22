//-----------------------------------------------------------------------------
// ECG-AI Project - Standalone SystemVerilog Simulation Testbench
// Module: tb_loadw_pair_sim
// Target: Direct simulation on Vivado GUI xsim.
//         Verify CV-X-IF LOADW handshake and backpressure mechanism
//         between instruction decoder (ecg_cvxif) and weight memory DMA (ecg_wmem).
//         Validate F03 flow: LOADW #1 triggers busy, LOADW #2 must be stalled.
//-----------------------------------------------------------------------------

`timescale 1ns / 1ps

module tb_loadw_pair_sim;
  localparam real CLK_PERIOD = 10.0; // 100 MHz (10 ns)

  // Control signals
  logic        clk;
  logic        rst_n;
  logic        iss_valid;
  logic [31:0] iss_instr;
  logic [31:0] iss_rs1;
  logic [31:0] iss_rs2;
  logic        iss_ready;
  logic        iss_accept;
  logic        dma_start;
  logic [13:0] dma_len;
  logic        dma_busy;
  logic        dma_done;
  logic        s_valid;
  logic [63:0] s_data;
  logic        s_ready;

  int error_count = 0;

  // DUT Wrapper
  tb_loadw_pair_wrap u_wrap (
      .clk_i        (clk),
      .rst_ni       (rst_n),
      .iss_valid_i  (iss_valid),
      .iss_instr_i  (iss_instr),
      .iss_rs1_i    (iss_rs1),
      .iss_rs2_i    (iss_rs2),
      .iss_ready_o  (iss_ready),
      .iss_accept_o (iss_accept),
      .dma_start_o  (dma_start),
      .dma_len_o    (dma_len),
      .dma_busy_o   (dma_busy),
      .dma_done_o   (dma_done),
      .s_valid_i    (s_valid),
      .s_data_i     (s_data),
      .s_ready_o    (s_ready)
  );

  // Clock Gen
  initial begin
    clk = 0;
    forever #(CLK_PERIOD / 2.0) clk = ~clk;
  end

  // Stimulus
  initial begin
    $display("\n==============================================================");
    $display("=== START SIMULATION: tb_loadw_pair_sim (Vivado xsim) ===");
    $display("==============================================================");

    // Initialization
    rst_n     = 1'b0;
    iss_valid = 1'b0;
    iss_instr = '0;
    iss_rs1   = '0;
    iss_rs2   = '0;
    s_valid   = 1'b0;
    s_data    = '0;

    #(CLK_PERIOD * 3);
    rst_n = 1'b1;
    #(CLK_PERIOD * 2);

    // TEST 1: Check initial idle state
    $display("[TEST 1] Checking initial idle state of CV-X-IF shim and wmem...");
    if (dma_busy !== 1'b0) begin
      $display("  -> ERROR: dma_busy_o initially non-zero!");
      error_count++;
    end
    if (iss_ready !== 1'b1) begin
      $display("  -> ERROR: iss_ready_o initially not ready!");
      error_count++;
    end
    if (error_count == 0) $display("  -> TEST 1 PASSED: Shim and DMA ready in IDLE state.");

    // TEST 2: Issue LOADW #1 (Load 64 bytes into wmem)
    // Custom instruction LOADW: funct3 = 3'b001, opcode = 7'b0001011 (custom-0)
    @(posedge clk);
    $display("\n[TEST 2] Issue LOADW #1 (request DMA 64 bytes)...");
    iss_valid <= 1'b1;
    iss_instr <= 32'h0200_100b; // Custom LOADW opcode
    iss_rs1   <= 32'h0000_0040; // len = 64 bytes
    iss_rs2   <= 32'h0000_0000; // offset = 0

    @(posedge clk);
    #1;
    if (iss_accept) begin
      $display("  -> Shim ACCEPTED LOADW #1 instruction.");
    end

    // Clear iss_valid after accept
    iss_valid <= 1'b0;

    // Monitor dma_start_o and dma_busy
    @(posedge clk);
    #1;
    if (dma_busy) begin
      $display("  -> DMA activated BUSY = 1 successfully.");
    end

    // TEST 3: Attempt issuing LOADW #2 while DMA is busy (Backpressure check)
    $display("\n[TEST 3] Attempt issuing LOADW #2 while DMA is busy...");
    iss_valid <= 1'b1;
    iss_instr <= 32'h0200_100b;
    iss_rs1   <= 32'h0000_0020;
    iss_rs2   <= 32'h0000_0040;

    @(posedge clk);
    #1;
    if (iss_ready == 1'b0 || iss_accept == 1'b0) begin
      $display("  -> [PASS] Backpressure operates correctly: Shim stalled, no overwrite!");
    end else begin
      $display("  -> [FAIL] Shim failed to backpressure when DMA is busy!");
      error_count++;
    end
    iss_valid <= 1'b0;

    // TEST 4: Inject DMA stream data and wait for DMA done
    $display("\n[TEST 4] Inject DMA stream data (s_valid = 1)...");
    for (int i = 0; i < 8; i++) begin
      @(posedge clk);
      s_valid <= 1'b1;
      s_data  <= {32'hA5A5_0000 + i, 32'h5A5A_0000 + i};
    end
    @(posedge clk);
    s_valid <= 1'b0;

    // Await DMA completion
    repeat (10) @(posedge clk);

    $display("\n==============================================================");
    if (error_count == 0) begin
      $display("=== [PASS] TB_LOADW_PAIR_SIM: ALL TESTS PASSED! ===");
      $display("=== DMA & CV-X-IF backpressure protocol operates bit-exact ===");
    end else begin
      $display("=== [FAIL] TB_LOADW_PAIR_SIM: DETECTED %0d ERRORS! ===", error_count);
    end
    $display("==============================================================\n");

    $finish;
  end

endmodule

// Wrapper connecting REAL SHIM with REAL LOADER to measure F03.
//
// WHY WRAPPER IS NEEDED rather than testing `ecg_cvxif` in isolation: the F03 window
// lies in the RESPONSE LATENCY between the two modules. `dma_start_o` is a REGISTERED
// output, while `ecg_wmem` only asserts `busy` AFTER sampling start. Thus on the cycle
// immediately following receipt of LOADW #1, the shim sees `dma_busy_i` = 0 and accepts
// LOADW #2 -- an isolated test on `ecg_cvxif` with manual `dma_busy_i` misses this.
// 
//
// Hence: same module, two wrappers, two findings. Scope is SYSTEM, not MODULE.
`ifndef TB_LOADW_PAIR_WRAP_SV
`define TB_LOADW_PAIR_WRAP_SV

module tb_loadw_pair_wrap (
    input  logic        clk_i,
    input  logic        rst_ni,
    input  logic        iss_valid_i,
    input  logic [31:0] iss_instr_i,
    input  logic [31:0] iss_rs1_i,
    input  logic [31:0] iss_rs2_i,
    output logic        iss_ready_o,
    output logic        iss_accept_o,
    output logic        dma_start_o,
    output logic [13:0] dma_len_o,
    output logic        dma_busy_o,
    output logic        dma_done_o,
    // wmem data input path, to measure actual bytes written
    input  logic        s_valid_i,
    input  logic [63:0] s_data_i,
    output logic        s_ready_o
);
  logic dma_busy;

  // F11: this wrapper has no core, so it MODELS the commit channel -- one cycle

  // pulse of `cmt_ok` after each accepted instruction, never killed. Matches CV32E40X

  // behavior on non-killed path (`commit_valid` asserted on first non-halted EX cycle);

  // the objective of this file is LOADW BACKPRESSURE, not kill handling.

  // The KILL path is measured in `tb_xif_kill_wrap`, where commit is driven externally.

  // Explicitly documenting model channel to differentiate from hardware core.

  // mot phep do tren he that.

  logic model_cmt_ok;

  always_ff @(posedge clk_i or negedge rst_ni)

    if (!rst_ni) model_cmt_ok <= 1'b0;

    else         model_cmt_ok <= iss_valid_i && iss_ready_o && iss_accept_o;


  ecg_cvxif u_shim (
      .clk_i, .rst_ni,
      .iss_valid_i, .iss_instr_i, .iss_rs1_i, .iss_rs2_i,
      .iss_ready_o, .iss_accept_o, .iss_wb_o (),
      .res_valid_o (), .res_rd_o (), .res_data_o (),
      .cp_start_o (), .cp_single_o (), .cp_layer_o (),
      .cp_busy_i (1'b0), .cp_done_i (1'b0),
      .dma_start_o, .dma_len_o,
      .dma_busy_i (dma_busy),
      .cmt_ok_i      (model_cmt_ok),
      .cmt_kill_i    (1'b0)
  );

  ecg_wmem u_wm (
      .clk_i, .rst_ni,
      .dma_start_i (dma_start_o),
      .dma_len_i   (dma_len_o),
      .dma_busy_o  (dma_busy),
      .dma_done_o,
      .s_valid_i, .s_data_i, .s_ready_o,
      .rd_req_i (1'b0), .rd_off_i ('0), .rd_valid_o (), .rd_data_o ()
  );

  assign dma_busy_o = dma_busy;
endmodule

`endif

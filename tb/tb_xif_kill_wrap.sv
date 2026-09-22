// Flat wrapper connecting REAL `ecg_xif_bridge` + `ecg_cvxif`.
//
// Unlike `tb_xif_bridge_wrap.sv`: that wrapper MOCKS the shim side (testbench
// drives `iss_accept_i`, `res_valid_i`...), measuring bridge behavior without
// coprocessor side-effects. This file connects the real shim and exposes coproc
// outputs to answer another question:
//
//   when an accepted instruction is killed via `commit_kill`, what changed?
//
// `ecg_cvxif` acts at ISSUE time (enqueues tag into FIFO, increments counter,
// and asserts `dma_start_o` for LOADW), so a killed instruction cannot rollback.
// The bridge has `kill_seen_o` for detection, verified in tb_ecg_xif_bridge
// (T5, T5a-T5d). This wrapper measures residual state after a kill, which differs
// depending on instruction opcode class.
//
// This is a HARNESS, not design RTL -- hence placed in `40-rtl/tb/`.

`ifndef TB_XIF_KILL_WRAP_SV
`define TB_XIF_KILL_WRAP_SV

module tb_xif_kill_wrap #(
    parameter int unsigned X_ID_WIDTH = 4
) (
    input  logic                    clk_i,
    input  logic                    rst_ni,

    // ── Core side (driven by wrapper) ────────────────────────────────────
    input  logic                    issue_valid_i,
    input  logic [31:0]             issue_instr_i,
    input  logic [X_ID_WIDTH-1:0]   issue_id_i,
    input  logic [31:0]             issue_rs1_i,
    input  logic [31:0]             issue_rs2_i,
    input  logic                    commit_valid_i,
    input  logic [X_ID_WIDTH-1:0]   commit_id_i,
    input  logic                    commit_kill_i,
    input  logic                    result_ready_i,

    // ── Core side (DUT outputs) ──────────────────────────────────────────
    output logic                    issue_ready_o,
    output logic                    issue_accept_o,
    output logic                    result_valid_o,
    output logic [31:0]             result_data_o,
    output logic                    kill_seen_o,

    // ── Coprocessor side: measurement points ─────────────────────────────
    input  logic                    cp_busy_i,
    input  logic                    cp_done_i,
    input  logic                    dma_busy_i,
    output logic                    cp_start_o,
    output logic                    cp_single_o,
    output logic [5:0]              cp_layer_o,
    output logic                    dma_start_o,
    output logic [13:0]             dma_len_o
);

  cv32e40x_if_xif #(.X_ID_WIDTH(X_ID_WIDTH)) xif ();

  always_comb begin
    xif.compressed_valid = 1'b0;
    xif.compressed_req   = '0;
    xif.issue_valid      = issue_valid_i;
    xif.issue_req        = '0;
    xif.issue_req.instr  = issue_instr_i;
    xif.issue_req.id     = issue_id_i;
    xif.issue_req.rs[0]  = issue_rs1_i;
    xif.issue_req.rs[1]  = issue_rs2_i;
    xif.issue_req.rs_valid = 2'b11;
    xif.commit_valid     = commit_valid_i;
    xif.commit           = '0;
    xif.commit.id        = commit_id_i;
    xif.commit.commit_kill = commit_kill_i;
    xif.mem_ready        = 1'b1;
    xif.mem_resp         = '0;
    xif.mem_result_valid = 1'b0;
    xif.mem_result       = '0;
    xif.result_ready     = result_ready_i;
  end

  assign issue_ready_o  = xif.issue_ready;
  assign issue_accept_o = xif.issue_resp.accept;
  assign result_valid_o = xif.result_valid;
  assign result_data_o  = xif.result.data;

  // ── Bridge <-> shim interconnect (internal wires) ────────────────────
  logic s_iss_valid, s_iss_ready, s_iss_accept, s_iss_wb;
  logic s_cmt_ok, s_cmt_kill;   // F11: Resolved commit pulse per ID
  logic [31:0] s_iss_instr, s_iss_rs1, s_iss_rs2;
  logic s_res_valid;
  logic [4:0]  s_res_rd;
  logic [31:0] s_res_data;

  ecg_xif_bridge #(.X_ID_WIDTH(X_ID_WIDTH)) u_bridge (
      .clk_i             (clk_i),
      .rst_ni            (rst_ni),
      .xif_compressed_if (xif),
      .xif_issue_if      (xif),
      .xif_commit_if     (xif),
      .xif_mem_if        (xif),
      .xif_mem_result_if (xif),
      .xif_result_if     (xif),
      .iss_valid_o       (s_iss_valid),
      .iss_instr_o       (s_iss_instr),
      .iss_rs1_o         (s_iss_rs1),
      .iss_rs2_o         (s_iss_rs2),
      .iss_ready_i       (s_iss_ready),
      .iss_accept_i      (s_iss_accept),
      .iss_wb_i          (s_iss_wb),
      .res_valid_i       (s_res_valid),
      .res_rd_i          (s_res_rd),
      .res_data_i        (s_res_data),
      .kill_seen_o       (kill_seen_o),
      .cmt_ok_o          (s_cmt_ok),
      .cmt_kill_o        (s_cmt_kill)
  );

  ecg_cvxif u_shim (
      .clk_i        (clk_i),
      .rst_ni       (rst_ni),
      .iss_valid_i  (s_iss_valid),
      .iss_instr_i  (s_iss_instr),
      .iss_rs1_i    (s_iss_rs1),
      .iss_rs2_i    (s_iss_rs2),
      .iss_ready_o  (s_iss_ready),
      .iss_accept_o (s_iss_accept),
      .iss_wb_o     (s_iss_wb),
      .res_valid_o  (s_res_valid),
      .res_rd_o     (s_res_rd),
      .res_data_o   (s_res_data),
      .cp_start_o   (cp_start_o),
      .cp_single_o  (cp_single_o),
      .cp_layer_o   (cp_layer_o),
      .cp_busy_i    (cp_busy_i),
      .cp_done_i    (cp_done_i),
      .dma_start_o  (dma_start_o),
      .dma_len_o    (dma_len_o),
      .dma_busy_i   (dma_busy_i),
      .cmt_ok_i          (s_cmt_ok),
      .cmt_kill_i        (s_cmt_kill)
  );

endmodule

`endif

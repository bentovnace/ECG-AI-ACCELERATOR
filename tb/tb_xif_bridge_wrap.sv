// Flat pin wrapper for ecg_xif_bridge, for simulation use.
//
// RATIONALE: simulation tools do not support interfaces at TOP-LEVEL, whereas
// ecg_xif_bridge interfaces via six `cv32e40x_if_xif.coproc_*` interface instances.
// This wrapper instantiates interface internally and exposes flat pins for C++ testbench.
//
// Comments starting with "verilator" are treated as PRAGMAs, not comments
// -- the above line originally began with that token and produced %Error-BADVLTPRAGMA.
//
// Verification harness, not synthesizable RTL design -- lives in `40-rtl/tb/`,
// not in `40-rtl/src/`. None of the synthesis flow scripts read this file.
//
// CPU side driven by wrapper; coproc side driven by DUT.
// Partitions follow interface modports, preventing multi-driven net issues.

`ifndef TB_XIF_BRIDGE_WRAP_SV
`define TB_XIF_BRIDGE_WRAP_SV

module tb_xif_bridge_wrap #(
    parameter int unsigned X_ID_WIDTH = 4
) (
    input  logic                    clk_i,
    input  logic                    rst_ni,

    // ── CORE side (driven by wrapper) ────────────────────────────────────────────
    input  logic                    issue_valid_i,
    input  logic [31:0]             issue_instr_i,
    input  logic [X_ID_WIDTH-1:0]   issue_id_i,
    input  logic [31:0]             issue_rs1_i,
    input  logic [31:0]             issue_rs2_i,
    input  logic                    commit_valid_i,
    input  logic [X_ID_WIDTH-1:0]   commit_id_i,
    input  logic                    commit_kill_i,
    input  logic                    result_ready_i,

    // ── CORE side (driven by DUT) ───────────────────────────────────────────────
    output logic                    issue_ready_o,
    output logic                    issue_accept_o,
    output logic                    issue_writeback_o,
    output logic                    result_valid_o,
    output logic [X_ID_WIDTH-1:0]   result_id_o,
    output logic [4:0]              result_rd_o,
    output logic [31:0]             result_data_o,
    // XIF-02: expose exception fields for testbench observation.
    output logic                    result_exc_o,
    output logic [5:0]              result_exccode_o,

    // ── SHIM side (driven by wrapper) ───────────────────────────────────────────
    input  logic                    iss_ready_i,
    input  logic                    iss_accept_i,
    input  logic                    iss_wb_i,
    input  logic                    res_valid_i,
    input  logic [4:0]              res_rd_i,
    input  logic [31:0]             res_data_i,

    // ── SHIM side (driven by DUT) ──────────────────────────────────────────────
    output logic                    iss_valid_o,
    output logic [31:0]             iss_instr_o,
    output logic [31:0]             iss_rs1_o,
    output logic [31:0]             iss_rs2_o,

    output logic                    kill_seen_o,
    // F11: expose to enable TB verification of OK path; previously internal wires.
    // 
    // 
    output logic                    cmt_ok_o,
    output logic                    cmt_kill_o
);

  cv32e40x_if_xif #(.X_ID_WIDTH(X_ID_WIDTH)) xif ();

  // ── CPU side wrapper drive ──────────────────────────────────────────────
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

  // ── coproc side read ──────────────────────────────────────────────────
  assign issue_ready_o     = xif.issue_ready;
  assign issue_accept_o    = xif.issue_resp.accept;
  assign issue_writeback_o = xif.issue_resp.writeback;
  assign result_valid_o    = xif.result_valid;
  assign result_id_o       = xif.result.id;
  assign result_rd_o       = xif.result.rd;
  assign result_data_o     = xif.result.data;
  assign result_exc_o      = xif.result.exc;
  assign result_exccode_o  = xif.result.exccode;



  ecg_xif_bridge #(.X_ID_WIDTH(X_ID_WIDTH)) u_dut (
      .clk_i             (clk_i),
      .rst_ni            (rst_ni),
      .xif_compressed_if (xif),
      .xif_issue_if      (xif),
      .xif_commit_if     (xif),
      .xif_mem_if        (xif),
      .xif_mem_result_if (xif),
      .xif_result_if     (xif),
      .iss_valid_o       (iss_valid_o),
      .iss_instr_o       (iss_instr_o),
      .iss_rs1_o         (iss_rs1_o),
      .iss_rs2_o         (iss_rs2_o),
      .iss_ready_i       (iss_ready_i),
      .iss_accept_i      (iss_accept_i),
      .iss_wb_i          (iss_wb_i),
      .res_valid_i       (res_valid_i),
      .res_rd_i          (res_rd_i),
      .res_data_i        (res_data_i),
      .kill_seen_o       (kill_seen_o),
      // F11: standalone bridge verification harness (shim is mocked).
      // Connected here to resolve `PINMISSING` warnings.
      // 
      .cmt_ok_o          (cmt_ok_o),
      .cmt_kill_o        (cmt_kill_o)
  );

endmodule

`endif

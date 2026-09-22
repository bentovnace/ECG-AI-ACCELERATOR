// Subsystem CORE + SHIM: CV32E40X connected to ecg_cvxif via actual CV-X-IF.
//
// WHY THIS FILE EXISTS, and why it is a REAL DESIGN rather than a test scaffold:
//
// Previous core area measurement (165,414.896 um2, 20,031 cells, 2,558 flops --
// §C29/§C33) used `tools/gen_core_wrap.py`, driving all 8 XIF inputs from a
// single 912-bit dummy port `xif_drive_i`. Area numbers were correct --
// measuring config `X_EXT = 1` -- but the pin ring was unphysical:
//
//   cv32e40x_core_wrap   1,602 / 2,126 pin ring sites = 75.4 %
//   ecg_coproc             349 / 8,168 sites          =  4.3 %   (17.6x sparser)
//   of those 1,602 pins, 1,024 came from xif_drive_i scaffold
//
// Phase 35 proved pin density prevented `detailed_route` convergence:
// iter 0 had 98,212 violations, iter 1 had 92,045 (~6 %/iter), taking ~37 hours
// without converging; 86.5 % of vias were pin access vias.
// Hypothesis "die too tight" was REFUTED: global route overflow was 0/0/0 at both
// util 45 and util 50, and expanding perimeter by 5.3 % changed nothing.
//
// Therefore this file makes XIF an INTERNAL INTERFACE, shrinking pin ring
// from ~1,602 to two OBI buses plus coprocessor control.
// Area represents a REAL SUBSYSTEM rather than an experimental wrapper.
//
//
// ── GROUNDED PINS AND RATIONALE ──────────────────────────────────────────
// Principle: GROUND actual constants in target system; DRIVE signals that truly vary.
// Previous violation eliminated 70 % of core registers (P34) via opt_dff over
// don't-care inputs. Grounding choices below are justified, and post-synth flop
// count must match standalone core baseline (2,558 flops).
//
//
//   scan_cg_en_i = 0     Normal mission mode. Scan is chip-level task.
//   boot_addr / mtvec / dm_halt / dm_exception   Constants in this SoC,
//                        kept as module parameters to allow change without RTL edits.
//   mhartid_i = 0        Single-core instance.
//   mimpid_patch_i = 0   No patch applied.
//   clic_* = 0           CLIC disabled (parameter CLIC = 0 default).
//   debug_req_i = 0      No debug module in this subsystem. DEBUG = 1 default
//                        retains logic in design; request net unasserted.
//   wu_wfe_i = 0         No external wake-for-event source.
//   fencei ack           fence.i has no cache to invalidate; acknowledged
//                        after 1 cycle (registered) avoiding combinational feedthrough.
//
//
//
//
//
//
//
// ── KNOWN LIMITATIONS ───────────────────────────────────────────────────
// ecg_cvxif acts at ISSUE, so cancelled instructions cannot roll back.
// C24 measured missing pieces (`dualwrite`, exception, `commit_kill`):
// 39 -> 56 LUT and 52 -> 96 FF; resolution for `commit_kill` is deferring to commit.
// Draft `40-rtl/est/ecg_cvxif_full.sv` is estimation code outside src/.
// Subsystem uses real shim and sticky flag `xif_kill_seen_o` to flag violations.
//
//
module ecg_core_xif import cv32e40x_pkg::*; #(
    parameter logic [31:0] BOOT_ADDR         = 32'h0000_0080,
    parameter logic [31:0] MTVEC_ADDR        = 32'h0000_0000,
    parameter logic [31:0] DM_HALT_ADDR      = 32'hF000_0800,
    parameter logic [31:0] DM_EXCEPTION_ADDR = 32'hF000_0808,
    parameter int unsigned X_ID_WIDTH        = 4,
    parameter int unsigned CVXIF_FIFO_DEPTH  = 4
) (
    input  logic        clk_i,
    input  logic        rst_ni,
    input  logic        fetch_enable_i,
    output logic        core_sleep_o,

    // ── Instruction OBI ──────────────────────────────────────────────
    output logic        instr_req_o,
    input  logic        instr_gnt_i,
    input  logic        instr_rvalid_i,
    output logic [31:0] instr_addr_o,
    output logic [1:0]  instr_memtype_o,
    output logic [2:0]  instr_prot_o,
    output logic        instr_dbg_o,
    input  logic [31:0] instr_rdata_i,
    input  logic        instr_err_i,

    // ── Data OBI ─────────────────────────────────────────────────────
    output logic        data_req_o,
    input  logic        data_gnt_i,
    input  logic        data_rvalid_i,
    output logic [31:0] data_addr_o,
    output logic [3:0]  data_be_o,
    output logic        data_we_o,
    output logic [31:0] data_wdata_o,
    output logic [1:0]  data_memtype_o,
    output logic [2:0]  data_prot_o,
    output logic        data_dbg_o,
    output logic [5:0]  data_atop_o,
    input  logic [31:0] data_rdata_i,
    input  logic        data_err_i,
    input  logic        data_exokay_i,

    // ── Interrupts and timers: truly dynamic, driven externally ───────
    input  logic [31:0] irq_i,
    input  logic [63:0] time_i,
    output logic [63:0] mcycle_o,

    // ── Debug observation ────────────────────────────────────────────
    output logic        debug_havereset_o,
    output logic        debug_running_o,
    output logic        debug_halted_o,
    output logic        debug_pc_valid_o,
    output logic [31:0] debug_pc_o,

    // ── Coprocessor interface: connects to ecg_coproc at top level ───
    output logic        cp_start_o,
    output logic        cp_single_o,
    output logic [5:0]  cp_layer_o,
    input  logic        cp_busy_i,
    input  logic        cp_done_i,
    output logic        dma_start_o,
    output logic [13:0] dma_len_o,
    input  logic        dma_busy_i,

    // Sticky flag: observed `commit_kill` on an accepted instruction.
    output logic        xif_kill_seen_o
);

    // ── CV-X-IF interface channel ────────────────────────────────────
  cv32e40x_if_xif #(
      .X_NUM_RS    (2),
      .X_ID_WIDTH  (X_ID_WIDTH),
      .X_MEM_WIDTH (32),
      .X_RFR_WIDTH (32),
      .X_RFW_WIDTH (32)
  ) xif ();

  // ── fence.i: acknowledge after ONE cycle (no cache to flush) ──────
  logic fencei_req, fencei_ack_q;
  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) fencei_ack_q <= 1'b0;
    else         fencei_ack_q <= fencei_req;
  end

  // ── Core instance ─────────────────────────────────────────────────
  cv32e40x_core #(
      .X_EXT       (1'b1),
      .X_ID_WIDTH  (X_ID_WIDTH),
      .X_NUM_RS    (2),
      .X_MEM_WIDTH (32),
      .X_RFR_WIDTH (32),
      .X_RFW_WIDTH (32)
  ) u_core (
      .clk_i                (clk_i),
      .rst_ni               (rst_ni),
      .scan_cg_en_i         (1'b0),

      .boot_addr_i          (BOOT_ADDR),
      .dm_exception_addr_i  (DM_EXCEPTION_ADDR),
      .dm_halt_addr_i       (DM_HALT_ADDR),
      .mhartid_i            (32'h0),
      .mimpid_patch_i       (4'h0),
      .mtvec_addr_i         (MTVEC_ADDR),

      .instr_req_o          (instr_req_o),
      .instr_gnt_i          (instr_gnt_i),
      .instr_rvalid_i       (instr_rvalid_i),
      .instr_addr_o         (instr_addr_o),
      .instr_memtype_o      (instr_memtype_o),
      .instr_prot_o         (instr_prot_o),
      .instr_dbg_o          (instr_dbg_o),
      .instr_rdata_i        (instr_rdata_i),
      .instr_err_i          (instr_err_i),

      .data_req_o           (data_req_o),
      .data_gnt_i           (data_gnt_i),
      .data_rvalid_i        (data_rvalid_i),
      .data_addr_o          (data_addr_o),
      .data_be_o            (data_be_o),
      .data_we_o            (data_we_o),
      .data_wdata_o         (data_wdata_o),
      .data_memtype_o       (data_memtype_o),
      .data_prot_o          (data_prot_o),
      .data_dbg_o           (data_dbg_o),
      .data_atop_o          (data_atop_o),
      .data_rdata_i         (data_rdata_i),
      .data_err_i           (data_err_i),
      .data_exokay_i        (data_exokay_i),

      .mcycle_o             (mcycle_o),
      .time_i               (time_i),
      .irq_i                (irq_i),
      .wu_wfe_i             (1'b0),

      .clic_irq_i           (1'b0),
      .clic_irq_id_i        ('0),
      .clic_irq_level_i     (8'h0),
      .clic_irq_priv_i      (2'b00),
      .clic_irq_shv_i       (1'b0),

      .fencei_flush_req_o   (fencei_req),
      .fencei_flush_ack_i   (fencei_ack_q),

      .debug_req_i          (1'b0),
      .debug_havereset_o    (debug_havereset_o),
      .debug_running_o      (debug_running_o),
      .debug_halted_o       (debug_halted_o),
      .debug_pc_valid_o     (debug_pc_valid_o),
      .debug_pc_o           (debug_pc_o),

      .fetch_enable_i       (fetch_enable_i),
      .core_sleep_o         (core_sleep_o),

      .xif_compressed_if    (xif),
      .xif_issue_if         (xif),
      .xif_commit_if        (xif),
      .xif_mem_if           (xif),
      .xif_mem_result_if    (xif),
      .xif_result_if        (xif)
  );

  // ── Interface bridge to flattened port ────────────────────────────
  logic        iss_valid, iss_ready, iss_accept, iss_wb;
  logic cmt_ok, cmt_kill;   // F11: commit beat resolved by ID
  logic [31:0] iss_instr, iss_rs1, iss_rs2;
  logic        res_valid;
  logic [4:0]  res_rd;
  logic [31:0] res_data;

  ecg_xif_bridge #(
      .X_ID_WIDTH (X_ID_WIDTH),
      .XLEN       (32)
  ) u_bridge (
      .clk_i              (clk_i),
      .rst_ni             (rst_ni),
      .xif_compressed_if  (xif),
      .xif_issue_if       (xif),
      .xif_commit_if      (xif),
      .xif_mem_if         (xif),
      .xif_mem_result_if  (xif),
      .xif_result_if      (xif),
      .iss_valid_o        (iss_valid),
      .iss_instr_o        (iss_instr),
      .iss_rs1_o          (iss_rs1),
      .iss_rs2_o          (iss_rs2),
      .iss_ready_i        (iss_ready),
      .iss_accept_i       (iss_accept),
      .iss_wb_i           (iss_wb),
      .cmt_ok_o           (cmt_ok),
      .cmt_kill_o         (cmt_kill),
      .res_valid_i        (res_valid),
      .res_rd_i           (res_rd),
      .res_data_i         (res_data),
      .kill_seen_o        (xif_kill_seen_o)
  );

  // ── Production shim (not estimation draft from 40-rtl/est) ────────
  ecg_cvxif #(
      .FIFO_DEPTH (CVXIF_FIFO_DEPTH)
  ) u_cvxif (
      .clk_i         (clk_i),
      .rst_ni        (rst_ni),
      .iss_valid_i   (iss_valid),
      .iss_instr_i   (iss_instr),
      .iss_rs1_i     (iss_rs1),
      .iss_rs2_i     (iss_rs2),
      .iss_ready_o   (iss_ready),
      .iss_accept_o  (iss_accept),
      .iss_wb_o      (iss_wb),
      .res_valid_o   (res_valid),
      .res_rd_o      (res_rd),
      .res_data_o    (res_data),
      .cp_start_o    (cp_start_o),
      .cp_single_o   (cp_single_o),
      .cp_layer_o    (cp_layer_o),
      .cp_busy_i     (cp_busy_i),
      .cp_done_i     (cp_done_i),
      .dma_start_o   (dma_start_o),
      .dma_len_o     (dma_len_o),
      .dma_busy_i    (dma_busy_i),
      .cmt_ok_i      (cmt_ok),
      .cmt_kill_i    (cmt_kill)
  );

endmodule

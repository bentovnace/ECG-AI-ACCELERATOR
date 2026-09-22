// ARBITER for two masters accessing a single OBI port of register block.
//
// RATIONALE: Architectural decision rather than an implementation detail.
// Standard requirement calls for an APB/AXI interface. An intuitive approach
// places APB BETWEEN RISC-V core and register block. That WORSENS LATENCY:
// direct OBI finishes in 1-2 cycles (`gnt_o` of ecg_mmio is tied to 1), whereas
// APB has two phases requiring at least 2 cycles for write and 3 for read --
// measured in tb_ecg_apb. Thus placing APB on CPU critical path would penalize
// the very low-latency requirement.
//
// Hence APB does NOT sit on local CPU path. It serves as a SECOND PORT for an
// EXTERNAL master -- Zynq PS via AXI-APB bridge, a second core, or a debugger
// on the APB bus. Local CPU retains the direct OBI interface.
//
// PRIORITY: Local CPU takes fixed high priority. Measured justification: APB
// access costs 2-3 cycles vs 1 cycle for OBI; prioritizing APB would stretch
// every CPU MMIO access -- and CPU cycle count is on the strict per-beat cycle
// budget (2603.7 -> 1437 measured cycles). External master is not time-critical.
//
// STARVATION FREEDOM: External master is delayed only while CPU accesses THIS
// block (`a_req_i`), and CPU cannot hold `a_req_i` permanently -- a store in
// CV32E40X emits a single-cycle req. The worst-case is back-to-back stores during
// sample window loading, which is strictly bounded (66 or 259 writes). Thus
// delay is finite, avoiding starvation -- verified in tb_ecg_mmio_mux.
//
module ecg_mmio_mux (
    input  logic        clk_i,
    input  logic        rst_ni,

    // ---- master A: local CPU (high priority) -------------------------------
    input  logic        a_req_i,
    output logic        a_gnt_o,
    input  logic [31:0] a_addr_i,
    input  logic        a_we_i,
    input  logic [3:0]  a_be_i,
    input  logic [31:0] a_wdata_i,
    output logic        a_rvalid_o,
    output logic [31:0] a_rdata_o,

    // ---- master B: external via APB (low priority) -------------------------
    input  logic        b_req_i,
    output logic        b_gnt_o,
    input  logic [31:0] b_addr_i,
    input  logic        b_we_i,
    input  logic [3:0]  b_be_i,
    input  logic [31:0] b_wdata_i,
    output logic        b_rvalid_o,
    output logic [31:0] b_rdata_o,

    // ---- slave: ecg_mmio --------------------------------------------------
    output logic        s_req_o,
    input  logic        s_gnt_i,
    output logic [31:0] s_addr_o,
    output logic        s_we_o,
    output logic [3:0]  s_be_o,
    output logic [31:0] s_wdata_o,
    input  logic        s_rvalid_i,
    input  logic [31:0] s_rdata_i
);

  // Selected master in the CURRENT cycle.
  logic sel_b;
  // MUC EXEC-08: fixed-priority arbitration, anti-starvation argument at lines 20-25.
  assign sel_b = b_req_i && !a_req_i;

  assign s_req_o   = a_req_i || b_req_i;
  assign s_addr_o  = sel_b ? b_addr_i  : a_addr_i;
  assign s_we_o    = sel_b ? b_we_i    : a_we_i;
  assign s_be_o    = sel_b ? b_be_i    : a_be_i;
  assign s_wdata_o = sel_b ? b_wdata_i : a_wdata_i;

  assign a_gnt_o = a_req_i && s_gnt_i;
  assign b_gnt_o = sel_b   && s_gnt_i;

  // ROUTING `rvalid` MUST FOLLOW WHO WAS GRANTED, NOT who is currently requesting.
  // `rvalid` from ecg_mmio arrives ONE cycle later, by which time `a_req_i` may
  // have deasserted or another master requested. Routing by current `sel_b`
  // would deliver read data to the wrong master silently. Must latch grant.
  // LATCH ON EVERY GRANTED ACCESS, NOT ONLY READS. Initially wrote `&& !s_we_o`,
  // which was buggy because ecg_mmio asserts `rvalid` for WRITES as well.
  // After a READ by B, `sel_b_q` remained 1, incorrectly routing CPU write `rvalid`
  // to master B, causing CV32E40X CPU hang (PC stuck at 0x6f6, core_sleep_o=0).
  //
  // Testbench model originally had the same bug (generating rvalid only on read).
  // Fixed by latching `sel_b` on every granted access (`s_req_o && s_gnt_i`).
  // core_sleep_o = 0).
  //
  // Va phep kiem cua toi khong bat duoc vi mo hinh slave trong tb CUNG sai theo
  // dung cach do: no chi sinh `rvalid` cho phep doc. Mot mo hinh doi chieu de
  // dai hon slave that khong lam phep kiem sai, nhung lam no KHONG THE phat hien
  // lop loi nay -- dung cau da ghi o P60 cho `ecg_actbuf`.
  logic sel_b_q;
  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) sel_b_q <= 1'b0;
    else if (s_req_o && s_gnt_i) sel_b_q <= sel_b;
  end

  assign a_rvalid_o = s_rvalid_i && !sel_b_q;
  assign b_rvalid_o = s_rvalid_i &&  sel_b_q;
  assign a_rdata_o  = s_rdata_i;
  assign b_rdata_o  = s_rdata_i;

`ifdef FORMAL
  // Never grant both masters in the same cycle.
  assert property (@(posedge clk_i) disable iff (!rst_ni) !(a_gnt_o && b_gnt_o));
  // `rvalid` must not be routed to both masters simultaneously.
  assert property (@(posedge clk_i) disable iff (!rst_ni) !(a_rvalid_o && b_rvalid_o));
`endif

endmodule

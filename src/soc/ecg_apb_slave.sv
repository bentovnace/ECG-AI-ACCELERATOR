// APB4 slave -> OBI: standard bus interface for coprocessor register block.
//
// RATIONALE: Previously, the coprocessor interfaced to RISC-V core via two
// non-standard paths: CV-X-IF (custom instruction interface) for control, and
// a register block wired directly to CV32E40X OBI data port for configuration.
// While functional, neither is a system bus: neither can integrate into an
// APB/AXI interconnect, cannot connect with external peripherals, and cannot
// leverage existing IP (Xilinx AXI-APB bridge, APB ILA on APB branch, etc.).
//
// LOW LATENCY CONSIDERATION: Measured sample load overhead is 10.05 cycles per
// MMIO write (linear cycle fit vs write count, tools/doc_soc_switch.py:244).
// The bus itself does not introduce 10 cycles; ecg_mmio gnt_o is tied to 1,
// completing an OBI write in 1-2 cycles. Ten cycles reflect firmware loop
// overhead plus writing exactly one INT8 sample in a 32-bit word. Changing bus
// alone does not reduce latency; latency reduction requires (a) packing multiple
// samples per word and (b) DMA block transfers. This module standardizes the
// interface; latency reduction belongs in ecg_apb_pre.sv and DMA path.
//
// PROTOCOL: APB4 has two phases: SETUP (psel high, penable low) then ACCESS
// (psel and penable high), terminating on the cycle pready is asserted. On OBI
// side: req is pulsed, gnt is tied to 1, and rvalid arrives one cycle later
// for READ operations only (ecg_mmio.sv:411 sets rd_q <= 0 by default, pulsed on read).
// Thus the FSM must separate read and write paths; waiting for rvalid on writes
// would stall indefinitely. This is the primary pitfall of this block.
module ecg_apb_slave #(
    parameter int unsigned ABITS = 32
) (
    input  logic              clk_i,
    input  logic              rst_ni,

    // ---- APB4 side (slave) --------------------------------------------------
    input  logic              psel_i,
    input  logic              penable_i,
    input  logic              pwrite_i,
    input  logic [ABITS-1:0]  paddr_i,
    input  logic [31:0]       pwdata_i,
    input  logic [3:0]        pstrb_i,
    output logic [31:0]       prdata_o,
    output logic              pready_o,
    output logic              pslverr_o,

    // ---- OBI side (master, connects to ecg_mmio) ----------------------------
    output logic              req_o,
    input  logic              gnt_i,
    output logic [31:0]       addr_o,
    output logic              we_o,
    output logic [3:0]        be_o,
    output logic [31:0]       wdata_o,
    input  logic              rvalid_i,
    input  logic [31:0]       rdata_i
);

  typedef enum logic [1:0] {
    S_IDLE,      // wait for APB ACCESS phase
    S_REQ,       // hold `req` until `gnt`
    S_READ,      // `gnt` granted for READ: wait for `rvalid`
    S_DONE       // single cycle `pready`
  } state_e;

  state_e state_q, state_d;
  logic [31:0] rdata_q;
  logic        we_q;
  logic        err_q;   // transaction returning is a rejected write

  // APB transaction begins on the first ACCESS cycle.
  logic start;
  assign start = psel_i && penable_i && (state_q == S_IDLE);

  // W1-D (F07+F08): Word-only access contract. Writes with `pstrb != 4'hF` are
  // rejected: no OBI transaction issued (no side effects) and returns `pslverr`.
  //
  // Previous version treated `pstrb == 0` as full word. Measured (tb_ecg_apb_mmio):
  // all 15 partial masks, even empty mask, triggered cp_start_o, cp_single_o,
  // and dma_start_o -- a partial write could trigger the START bit of CTRL,
  // and the bus falsely reported success (pslverr tied to 0).
  //
  // Runtime heuristic (pstrb == 0 -> full word) is unsafe: guesses master intent
  // from a valid value meaning the opposite. If an APB3 master is needed, use
  // a dedicated wrapper/parameter.
  // `ecg_mmio.h:8` -- all firmware MMIO accesses are full 32-bit words, so this
  // contract does not break existing firmware.
  logic unaligned_write;
  assign unaligned_write = psel_i && penable_i && pwrite_i && (pstrb_i != 4'b1111);

  assign req_o   = !unaligned_write && ((state_q == S_REQ) || start);
  assign addr_o  = {{(32 - ABITS){1'b0}}, paddr_i};
  assign we_o    = unaligned_write ? 1'b0 : (start ? pwrite_i : we_q);
  assign wdata_o = pwdata_i;
  // `pstrb` in APB4 and `be` in OBI share identical semantics (1 bit per byte).
  // A master that does not set pstrb (APB3) will write nothing if be = 0 --
  // so treating '0 as "full word" was wrong: it should not silently drop writes.
  // DO NOT substitute: `pstrb` passes straight through. An empty mask writes
  // no bytes; converting it to `4'b1111` turns a no-write into a 4-byte write.
  //
  // MUC APB-01 (danh gia 2026-09-05): pass correct strobe for APB4 -- `be_o` is
  // `pstrb_i` intact, without reinterpretation. Test: `make sim-apb`
  // (tb_ecg_apb.cpp, 16/16 masks).
  assign be_o    = pstrb_i;

  assign prdata_o  = rdata_q;
  assign pready_o  = (state_q == S_DONE);
  // This register block never rejects (`gnt_o` tied to 1) and foreign addresses
  // are IGNORED without error -- see `default` comment in ecg_mmio.sv:513.
  // Thus no internal source generates `pslverr`, and tying to 0 was intentional:
  // an unasserted `pslverr` is better than a spurious `pslverr` asserted for
  // reasons unknown to this block.
  // `pslverr` now has one source: rejected unaligned/partial writes. Other cases
  // remain 0 (foreign addresses are IGNORED without error -- see `default`
  // in ecg_mmio.sv).
  assign pslverr_o = (state_q == S_DONE) && err_q;

  always_comb begin
    state_d = state_q;
    unique case (state_q)
      S_IDLE: if (psel_i && penable_i) begin
                // Unaligned write: respond immediately with error, no transaction issued.
                if (unaligned_write) state_d = S_DONE;
                // gnt is tied to 1 in ecg_mmio, but do not assume: hold req if
                // another slave stalls.
                else if (!gnt_i)     state_d = S_REQ;
                else if (pwrite_i)   state_d = S_DONE;   // WRITE: complete immediately, no rvalid
                else                 state_d = S_READ;   // READ: wait for rvalid
              end
      S_REQ:  if (gnt_i) state_d = we_q ? S_DONE : S_READ;
      S_READ: if (rvalid_i) state_d = S_DONE;
      S_DONE: state_d = S_IDLE;
      default: state_d = S_IDLE;
    endcase
  end

  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
      state_q <= S_IDLE; rdata_q <= '0; we_q <= 1'b0; err_q <= 1'b0;
    end else begin
      state_q <= state_d;
      if (start)    we_q <= pwrite_i;
      if (start)    err_q <= unaligned_write;
      if (rvalid_i) rdata_q <= rdata_i;
    end
  end

`ifdef FORMAL
  // `pready` must be high for at most one cycle per access: holding pready for
  // two cycles makes the APB master infer two transfers.
  assert property (@(posedge clk_i) disable iff (!rst_ni)
      pready_o |=> !pready_o);
  // Must not assert `req` without an active APB access.
  assert property (@(posedge clk_i) disable iff (!rst_ni)
      req_o |-> psel_i);
`endif

endmodule

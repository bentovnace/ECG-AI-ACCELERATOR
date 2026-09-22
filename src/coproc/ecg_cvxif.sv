// CV-X-IF shim: the twelve custom instructions of docs/isa.md, and the command
// FIFO that makes them non-blocking.
//
// The execution model is the one in isa.md §4.1: firmware issues one compute
// instruction per layer, does not wait between them, and closes the beat with a
// single `ecg.store.barrier`. The instruction sequence for a beat is fixed per
// model and lives in flash; switching model changes a descriptor-table pointer,
// not the sequence. So the shim's job is narrow and specific:
//
//   * decode the custom-0 opcode space and REJECT anything else, so the core can
//     fall through to its own illegal-instruction path,
//   * accept a compute instruction in one cycle by pushing a layer index into a
//     FIFO and writing a token to rd -- this is what "non-blocking" means, and it
//     is the condition for LOADW of layer i+1 to overlap the compute of layer i,
//   * hold `ecg.store.barrier` until the FIFO is empty and the coprocessor is
//     idle, then return the result count in rd.
//
// Only one instruction blocks, and that is on purpose (ecg_pkg::ecg_op_blocking):
// making compute instructions blocking would serialise LOADW behind compute and
// T_switch would lose the overlap it depends on.
//
// The FIFO is four deep. That is not a guess: firmware issues at most one compute
// instruction per layer and the coprocessor retires one at a time, so depth only
// has to cover the burst a core can issue while a layer runs -- and the shortest
// layer in the four models is 32 cycles. Four gives the core room to run ahead
// without the shim ever being the reason it stalls.

`ifndef ECG_CVXIF_SV
`define ECG_CVXIF_SV

module ecg_cvxif
  import ecg_pkg::*;
#(
    parameter int unsigned FIFO_DEPTH = 4,
    localparam int unsigned PTR_W = $clog2(FIFO_DEPTH)
) (
    input  logic        clk_i,
    input  logic        rst_ni,

    // ---- Issue interface. Subset of CV-X-IF: one instruction per cycle, non-speculative.
    input  logic        iss_valid_i,
    // Full 32-bit instruction word. Bits 24:15 are rs2/rs1 register indices, and shim
    // does not use them: core reads registers and passes VALUES via iss_rs1_i/iss_rs2_i.
    // This is CV-X-IF division of labor, not an ignored field -- waiver here is intentional
    // rather than left silent.
    /* verilator lint_off UNUSEDSIGNAL */
    input  logic [31:0] iss_instr_i,
    /* verilator lint_on UNUSEDSIGNAL */
    input  logic [31:0] iss_rs1_i,
    input  logic [31:0] iss_rs2_i,
    output logic        iss_ready_o,    // shim can accept this instruction this cycle
    output logic        iss_accept_o,   // instruction belongs to shim space
    output logic        iss_wb_o,       // instruction will write back to rd

    // ---- Result interface.
    output logic        res_valid_o,
    output logic [4:0]  res_rd_o,
    output logic [31:0] res_data_o,

    // ---- Coprocessor control.
    output logic        cp_start_o,
    output logic        cp_single_o,
    output logic [5:0]  cp_layer_o,
    input  logic        cp_busy_i,
    input  logic        cp_done_i,

    // ---- Weight DMA.
    output logic        dma_start_o,
    output logic [13:0] dma_len_o,
    input  logic        dma_busy_i,

    // ---- F11: COMMIT beat of pending instruction, resolved by id at bridge ----
    input  logic        cmt_ok_i,
    input  logic        cmt_kill_i
);

  // ------------------------------------------------------------- decode
  logic [6:0]  major;
  logic [4:0]  op;
  logic [2:0]  fn3;
  logic [4:0]  rd;

  assign major = iss_instr_i[6:0];
  assign op    = iss_instr_i[ECG_OP_MSB:ECG_OP_LSB];
  assign fn3   = iss_instr_i[ECG_FN3_MSB:ECG_FN3_LSB];
  assign rd    = iss_instr_i[11:7];

  logic in_space, is_barrier, is_loadw, is_compute;
  // AN INSTRUCTION IS A TUPLE (op, fn3), not two independent fields -- earlier version
  // of this line misread that. Measured in simulation: a 'STORE' with fn3 = 000
  // (DMA WRITE variant, isa.md §3.3) passed through shim and became A COMPUTE LAYER --
  // tb reported "1 layer issued / 0 expected". Reason: 'is_compute' was defined as
  // COMPLEMENT ('!is_barrier && !is_loadw'), so all undefined pairs fell into it.
  // Three pairs fell into wrong place: op 0..9 with fn3 != 000, LOADW with fn3 != 001, and STORE
  // with fn3 = 000. See `ecg_enc_defined` in ecg_pkg.sv for why an instruction
  // IN THE SPEC is rejected.
  assign in_space   = (major == ECG_MAJOR_OPCODE) && (op < 5'(ECG_N_OPCODE))
                   && ecg_enc_defined(op, fn3);
  assign is_barrier = in_space && (op == 5'(ECG_STORE))
                   && (fn3 == 3'(ECG_CLASS_CTRL));
  assign is_loadw   = in_space && (op == 5'(ECG_LOADW));
  // All remaining opcodes are compute layers: layer index comes from rs1.
  assign is_compute = in_space && !is_barrier && !is_loadw;

  // ------------------------------------------------------------- state
  // Declare BEFORE every use. Verilator accepts using a signal before declaration,
  // but slang does not -- and slang follows the LRM. Hit this bug four times
  // in P5, so `make lint-rtl` now runs BOTH linters.
  logic        start_q;
  logic [5:0]  cp_layer_q;
  logic        res_valid_q;
  logic [4:0]  res_rd_q;
  logic [31:0] res_data_q;
  // 6 bits, not 32. Max layers per beat is 16 (m2 and m4 have 13), and
  // ECG_N_LAYER_TOTAL = 50 across all four families -- 6 bits is sufficient. `make depth`
  // measured ecg_cvxif depth at 114 logic levels, more than ecg_mac8, caused by two 32-bit
  // counters forming unnecessary 32-bit carry chains. Value returned to rd is still
  // 32 bits; only the counters are narrowed.
  logic [5:0]  n_issued_q;
  logic [5:0]  n_retired_q;
  logic        dma_start_q;
  logic [13:0] dma_len_q;

  // ITEM LOAD-02: REAL handshake between shim and loader, not relying solely on `busy`.
  // Backpressure branch: removing `!loadw_q_valid_q` from `iss_ready_o` keeps ALL THREE
  // LOADW targets PASSING -- that branch is correct but no test guarded it.
  // 1-DEEP LOADW QUEUE (F03) -- and why NOT stalling at the issue port.
  //
  // `!dma_busy_i` alone is insufficient: `dma_start_o` is registered and `ecg_wmem`
  // only asserts `busy` AFTER taking start. Thus, one cycle after receiving LOADW #1,
  // busy is still 0 and LOADW #2 is accepted -- then start fires twice while loader
  // is busy, and loader has NO queue. Measured on SYSTEM
  // (`tb_ecg_loadw_pair`): `ecg_wmem.sv:188` fires at distance 0.
  //
  // TWO PREVIOUS FIXES WERE WRONG, both proven by measurement:
  //   1. Deasserting `iss_ready_o` when loader is busy -> `sim-soc` FAILS completely.
  //      CPU both issues LOADW and SUPPLIES data (firmware pushes word-by-word via MMIO);
  //      stalling issue port halts CPU, no data is fed, `busy` never drops -- DEADLOCK.
  //   2. 1-cycle delayed latch + `!dma_busy_i` -> `sim-soc` still drifts: final phase token
  //      36/32, tb measurement windows fall empty. Less noise but still affects firmware.
  // Conclusion: a fix turning DROPPED INSTRUCTION into DEADLOCK is worse. Therefore:
  // ACCEPT then QUEUE. `iss_ready_o` for LOADW only drops when queue IS FULL,
  // so CPU keeps running to supply data.
  //
  //
  logic        loadw_inflight_q;   // 1-cycle delayed version of "just issued start"
  // ITEM LOAD-01: pending/inflight latch. Set when accepting LOADW while loader is busy,
  // cleared ONLY when loader is ready (`loadw_ready`).
  logic        loadw_q_valid_q;    // one pending instruction, start not yet issued
  logic [13:0] loadw_q_len_q;

  // ---------------------------------------------------------------- FIFO
  logic [5:0]      fifo [FIFO_DEPTH];
  logic [PTR_W:0]  wptr_q, rptr_q;
  logic            full, empty;

  assign empty = (wptr_q == rptr_q);
  assign full  = (wptr_q[PTR_W] != rptr_q[PTR_W])
              && (wptr_q[PTR_W-1:0] == rptr_q[PTR_W-1:0]);

  // Barrier holds issue path; all other instructions are accepted in one cycle.
  //
  // `!start_q` is required, not redundant: between FIFO pop and coprocessor asserting
  // `busy`, there is one cycle where FIFO is empty and `busy` is still low. Without it,
  // barrier opens early while last layer is still running -- assertion comparing completed
  // layers against issued instructions catches this.
  // ---- F11: AN ACCEPTED INSTRUCTION MUST NOT ACT UNTIL COMMITTED ------
  //
  // Measured before this fix (`tb_ecg_xif_kill`): an accepted instruction cancelled by
  // `commit_kill` STILL left modified state -- CONV1D/FC fired `cp_start` (coprocessor ran a layer),
  // LOADW fired `dma_start` (DMA wrote weight memory), and all 4 opcodes STILL returned a result.
  // 3 of 4 opcodes had architectural leakage.
  //
  // And kills CANNOT be prevented: `cv32e40x_controller_fsm.sv:1472` sets
  //   commit_kill = xif_csr_error_i || ctrl_fsm_o.kill_ex || kill_rejected
  // so a taken branch, exception, interrupt, or debug entry kills EX AFTER acceptance.
  // Assumption "this core has no speculative issue" (XIF-16) was REFUTED by measurement.
  //
  //
  // Therefore payload is LATCHED at issue and ACTION waits until commit. One cycle
  // latency, and that is the full cost paid.
  //
  // 1-outstanding makes a 1-deep pending register sufficient: bridge does not issue second
  // instruction while one is awaiting result (`iss_acceptable` in ecg_xif_bridge).
  logic        spec_valid_q;
  logic        spec_compute_q, spec_loadw_q, spec_barrier_q;
  logic [4:0]  spec_rd_q;
  logic [5:0]  spec_layer_q;
  logic [13:0] spec_len_q;

  logic accept_now;
  assign accept_now = iss_valid_i && iss_ready_o && in_space;

  logic act_now;                     // ACTUAL action cycle
  assign act_now = spec_valid_q && cmt_ok_i;

  logic drained;
  // `!spec_valid_q` is NOT defensive: while a compute instruction waits for commit,
  // FIFO is still EMPTY, so a barrier behind it would read "drained" while a layer
  // is about to run. Missing this term lets barrier open early -- same error class as
  // `!start_q` previously guarded.
  assign drained = empty && !cp_busy_i && !dma_busy_i && !start_q && !spec_valid_q;

  logic push, pop;
  // F11: push into FIFO at COMMIT, not at issue. `!full` remains because 1-outstanding
  // guarantees FIFO can only DRAIN between issue and commit, never fill.
  assign push = act_now && spec_compute_q && !full;
  assign pop  = !empty && !cp_busy_i && !cp_start_o;

  assign iss_accept_o = in_space;
  // F11 adds `!spec_valid_q` to ALL THREE branches: an uncommitted instruction occupies
  // the sole slot in the pending register. Bridge maintains 1-outstanding so this doesn't
  // change normal behavior -- it is a structural interlock making overwrite IMPOSSIBLE
  // rather than merely UNLIKELY.
  //
  assign iss_ready_o  = spec_valid_q ? 1'b0
                      : is_barrier   ? drained
                      : is_compute   ? !full
                                     : !loadw_q_valid_q;
  // THIRD BRANCH is LOADW, and before 2026-09-04 it was `1'b1` -- i.e. NO
  // backpressure. `dma_busy_i` only appeared in `drained` (barrier branch),
  // so a second LOADW was accepted and re-fired `dma_start_o` with NEW length
  // while DMA was running. Downstream `ecg_wmem` waits for `!busy_q` to latch
  // new transfer, so that instruction was SILENTLY DROPPED after reporting accepted.
  // Measured in `tb_ecg_cvxif_loadw`.
  //
  //
  // `!dma_busy_i` DOES NOT serialize LOADW after compute -- keeping overlap intact:
  // it only stalls LOADW behind another LOADW (one transfer at a time).
  // `cp_busy_i` is not in this branch.
  // Only barrier returns count; other instructions return a token to rd.
  assign iss_wb_o     = in_space;

  assign cp_single_o = 1'b1;
  // LATCH value at pop, do not read combinational value from read pointer: `start_q`
  // is registered so it rises ONE CYCLE after rptr advances, pointing to the NEXT item.
  // Originally read combinationally, causing layer indices to be offset by 1 --
  // symptom was output stream 1,2,3,4 instead of 0,1,2,3.
  assign cp_layer_o  = cp_layer_q;

  assign cp_start_o = start_q;
  assign res_valid_o = res_valid_q;
  assign res_rd_o    = res_rd_q;
  assign res_data_o  = res_data_q;

  // Loader ready to accept new start: not busy, and we did not just issue start
  // (ONE-cycle blind spot between `dma_start_q` and `busy`).
  logic loadw_ready;
  assign loadw_ready = !dma_busy_i && !dma_start_q && !loadw_inflight_q;

  assign dma_start_o = dma_start_q;
  assign dma_len_o   = dma_len_q;

  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
      wptr_q      <= '0;
      rptr_q      <= '0;
      start_q     <= 1'b0;
      cp_layer_q  <= '0;
      res_valid_q <= 1'b0;
      res_rd_q    <= '0;
      res_data_q  <= '0;
      n_issued_q  <= '0;
      n_retired_q <= '0;
      dma_start_q <= 1'b0;
      dma_len_q   <= '0;
      loadw_inflight_q <= 1'b0;
      loadw_q_valid_q  <= 1'b0;
      loadw_q_len_q    <= '0;
      spec_valid_q     <= 1'b0;
      spec_compute_q   <= 1'b0;
      spec_loadw_q     <= 1'b0;
      spec_barrier_q   <= 1'b0;
      spec_rd_q        <= '0;
      spec_layer_q     <= '0;
      spec_len_q       <= '0;
    end else begin
      start_q     <= 1'b0;
      res_valid_q <= 1'b0;
      dma_start_q <= 1'b0;
      loadw_inflight_q <= dma_start_q;
      // Pop pending instruction when loader is ready. Accept logic below cannot
      // overwrite because `iss_ready_o` deasserts when queue is full.
      if (loadw_q_valid_q && loadw_ready) begin
        dma_start_q     <= 1'b1;
        dma_len_q       <= loadw_q_len_q;
        loadw_q_valid_q <= 1'b0;
      end

      if (push) begin
        // F11: pop from PENDING REGISTER, not from issue port. At commit time,
        // `iss_rs1_i` belongs to a different instruction (or garbage) -- this exact bug
        // was introduced when changing action timing, silent if missed.
        fifo[wptr_q[PTR_W-1:0]] <= spec_layer_q;
        wptr_q <= wptr_q + 1'b1;
      end

      if (pop) begin
        start_q    <= 1'b1;
        cp_layer_q <= fifo[rptr_q[PTR_W-1:0]];
        rptr_q     <= rptr_q + 1'b1;
      end

      // Count completed layers. Barrier returns this count, and assertion below
      // compares it against issued instructions -- two counting paths for the same value.
      if (cp_done_i) n_retired_q <= n_retired_q + 6'd1;

      // ---- F11: LATCH at issue ------------------------------------------------
      // Record payload only. No external pulses here.
      if (accept_now) begin
        spec_valid_q   <= 1'b1;
        spec_compute_q <= is_compute;
        spec_loadw_q   <= is_loadw;
        spec_barrier_q <= is_barrier;
        spec_rd_q      <= rd;
        spec_layer_q   <= iss_rs1_i[5:0];
        spec_len_q     <= iss_rs2_i[13:0];
      end

      // ---- F11: ACT at commit -------------------------------------------------
      // `cmt_kill_i` has no separate branch: it clears `spec_valid_q` below.
      // A killed instruction leaves no trace -- that is the entire purpose of F11.
      if (act_now) begin
        spec_valid_q <= 1'b0;
        res_valid_q <= 1'b1;
        res_rd_q    <= spec_rd_q;
        if (spec_barrier_q) begin
          // Barrier returns NUMBER OF COMPLETED LAYERS for this beat, then resets both
          // counters. Returning issued instructions would say nothing about completion.
          //
          // PLUS cp_done_i: done pulse of final layer arrives at SAME cycle barrier opens
          // (busy drops and done asserts simultaneously), so n_retired_q has not incremented.
          // Without it, barrier returns 1 less than total layers.
          res_data_q  <= 32'({26'b0, n_retired_q + 6'({5'b0, cp_done_i})});
          n_issued_q  <= '0;
          n_retired_q <= '0;
        end else if (spec_compute_q) begin
          res_data_q <= 32'({26'b0, n_issued_q + 6'd1});
          n_issued_q <= n_issued_q + 6'd1;
        end else begin
          // LOADW is not a layer, so it does not increment layer counter. Originally
          // incremented, causing barrier to open with "8 completed / 10 issued" --
          // two numbers counting two different things.
          res_data_q <= 32'({26'b0, n_issued_q});
        end
        if (spec_loadw_q) begin
          if (loadw_ready) begin
            dma_start_q <= 1'b1;
            dma_len_q   <= spec_len_q;
          end else begin
            loadw_q_valid_q <= 1'b1;
            loadw_q_len_q   <= spec_len_q;
          end
        end
      end

      // ---- F11: KILLED -> leaves no side-effects ------------------------------
      // Clear pending state only. No `res_valid_q`, no `dma_start_q`, no `push`:
      // CV-X-IF requires an instruction with `commit_kill` to produce NO result,
      // and returning a result would write back a value for a discarded instruction.
      if (spec_valid_q && cmt_kill_i) begin
        spec_valid_q <= 1'b0;
      end
    end
  end

`ifndef SYNTHESIS
  /* verilator coverage_off */
  // F11: count cycles an accepted instruction waits for commit. VERIFICATION STATE,
  // not RTL design -- wrapped in `coverage_off` so high bit doesn't degrade
  // toggle coverage of the module for a measurement artifact.
  logic [3:0] f_cho_q;
  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni)           f_cho_q <= '0;
    else if (!spec_valid_q) f_cho_q <= '0;
    else if (f_cho_q != 4'hF) f_cho_q <= f_cho_q + 4'd1;
  end
  /* verilator coverage_on */

  always_ff @(posedge clk_i or negedge rst_ni) begin
    // Instructions outside custom-0 space must be REJECTED, not silently ignored:
    // core requires iss_accept_o = 0 to take illegal instruction trap.
    // A permissive shim turns an invalid instruction into a silent NOP.
    if (rst_ni && iss_valid_i && (major != ECG_MAJOR_OPCODE) && iss_accept_o)
      $error("ecg_cvxif: nhan lenh ngoai khong gian custom-0 (opcode %b)", major);
    // Barrier, FIFO full, and LOADW-when-DMA-busy are the THREE valid stall reasons.
    // If a compute instruction stalls issue path (other than FIFO full), next layer's
    // LOADW cannot overlap previous layer compute and T_switch loses overlap
    // (isa.md §4.3).
    //
    // `is_loadw && dma_busy_i` was added 2026-09-04 along with backpressure.
    // Invariant "only barrier stalls" is written in TWO places -- here and formal
    // assert #2 -- so modifying RTL without updating both triggers assertion.
    // That is intentional: an invariant written twice fires twice.
    // F11 ADDS A FOURTH STALL REASON, declared here instead of firing this invariant.
    // Reason: `spec_valid_q`: accepted instruction waiting for commit occupies
    // the pending register.
    //
    // What this invariant PROTECTS: a compute instruction must not stall for reasons
    // that SERIALIZE it with LOADW. Commit wait does NOT do that -- it lasts
    // 1-2 cycles, whereas T_switch overlap lasts HUNDREDS of cycles (coprocessor busy).
    // Hence clause is added, and its COST is BOUNDED by assertion below -- so
    // "only one or two cycles" is a MEASUREMENT rather than an assertion.
    //
    if (rst_ni && iss_valid_i && in_space && !iss_ready_o && !is_barrier
        && !(is_compute && full)
        && !(is_loadw && loadw_q_valid_q)
        && !spec_valid_q)
      $error("ecg_cvxif: lenh khong phai rao chan bi chan");

    // F11: UPPER BOUND ON COMMIT WAIT TIME. If `spec_valid_q` hangs, issue path hangs
    // and appears as a slow coprocessor rather than deadlock. CV32E40X commits at first
    // non-halted cycle of EX (1-2 cycles); 8 cycles is generous and catches
    // real hangs.
    //
    if (rst_ni && (f_cho_q > 4'd8))
      $error("ecg_cvxif: cho commit qua %0d chu ky, duong issue dang treo", f_cho_q);
    if (rst_ni && push && full)
      $error("ecg_cvxif: day FIFO khi da day");
    // Reserved bits [26:25] must be 0. Reserved for future fields, non-zero indicates
    // firmware uses an incompatible encoding.
    //
    if (rst_ni && iss_valid_i && in_space && (iss_instr_i[26:25] != 2'b00))
      $error("ecg_cvxif: hai bit du dia = %b, khong phai 00",
             iss_instr_i[26:25]);
    // Layer index in rs1[5:0]. Larger value indicates firmware bug; ignoring silently
    // would execute wrong layer instead of reporting error.
    if (rst_ni && iss_valid_i && is_compute && (iss_rs1_i[31:6] != '0))
      $error("ecg_cvxif: chi so lop %0d vuot 6 bit", iss_rs1_i);
    // DMA length in rs2[13:0].
    if (rst_ni && iss_valid_i && is_loadw && (iss_rs2_i[31:14] != '0))
      $error("ecg_cvxif: do dai LOADW %0d vuot 14 bit", iss_rs2_i);
    // When barrier unblocks, completed layers must equal issued instructions.
    if (rst_ni && iss_valid_i && is_barrier && iss_ready_o
        && ((n_retired_q + 6'({5'b0, cp_done_i})) != n_issued_q))
      $error("ecg_cvxif: rao chan mo voi %0d lop xong / %0d lenh nhan",
             n_retired_q + 6'({5'b0, cp_done_i}), n_issued_q);
    // 6 bits covers max layers per beat; overflow indicates too many instructions
    // between barriers.
    if (rst_ni && is_compute && iss_valid_i && iss_ready_o
        && (n_issued_q == 6'h3F))
      $error("ecg_cvxif: qua 63 lenh tinh toan giua hai rao chan");
  end
`endif

`ifdef FORMAL
  logic f_started_q = 1'b0;
  always_ff @(posedge clk_i) f_started_q <= 1'b1;
  always_comb if (!f_started_q) assume (!rst_ni);

  // Environment model: coprocessor accepts layer on `start`, drops `busy` and
  // fires `done` on completion. Without this model, formal engine allows arbitrary
  // transitions on busy/done, making scheduling properties meaningless.
  logic [1:0] f_out_q;
  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) f_out_q <= '0;
    else f_out_q <= f_out_q + (cp_start_o ? 2'd1 : 2'd0)
                            - (cp_done_i  ? 2'd1 : 2'd0);
  end
  always_comb if (f_started_q) begin
    assume (cp_busy_i == (f_out_q != 2'd0));
    assume (!cp_done_i || (f_out_q != 2'd0));
  end

  always_ff @(posedge clk_i) begin
    if (rst_ni && f_started_q) begin
      // 1. FIFO never overflows.
      //
      //    Previous assertion was `assert (!(push && full))`, which was structurally VACUOUS:
      //    `push` is assigned `iss_valid_i && is_compute && !full`, so `push && full` is FALSE.
      //    Did not prove FIFO cannot overflow; merely restated assignment above.
      //    A bug in `full` expression -- e.g. pointer MSB error -- would not fire it.
      //
      //
      //
      //    Proper property must evaluate STATE: number of elements in FIFO never exceeds depth.
      //    Requires reasoning about wrapped pointer subtraction, so an index bit error
      //    in `full` WILL fire it.
      //
      assert ((wptr_q - rptr_q) <= (PTR_W + 1)'(FIFO_DEPTH));

      // 1b. `empty` and `full` are mutually exclusive. Guards pointer comparison bits
      //     of both flags, which earlier property did not touch.
      assert (!(empty && full));

      // 2. ONLY barrier stalls issue path. If compute stalls (outside full FIFO),
      //    next layer's LOADW cannot overlap previous compute and T_switch loses overlap
      //    (isa.md §4.3).
      //
      //    RESTATEMENT: `iss_ready_o` is assigned from those branches, so expression is
      //    tautological with current RTL. See note in property 3.
      //
      //
      //
      //    THIRD BRANCH changed 2026-09-04: LOADW now stalls on `dma_busy_i`,
      //    so clause `|| (is_loadw && dma_busy_i)` was added to restate accurately.
      //    Dropping that clause with new RTL makes assertion FIRE.
      //
      //    FOURTH BRANCH added 2026-09-05 with F11: `spec_valid_q`. Same rationale --
      //    dropping this clause with new RTL makes assertion FIRE, meaning it remains
      //    a restatement.
      assert (!(iss_valid_i && in_space && !iss_ready_o)
              || is_barrier || (is_compute && full)
              || (is_loadw && loadw_q_valid_q)
              || spec_valid_q);

      // 2b. F11: Upper bound on commit wait time. Proves STATE (counter) rather than
      //     combinational assignment; bug preventing `spec_valid_q` clearance
      //     WILL fire it.
      //
      assert (f_cho_q <= 4'd8);

      // 3. `accept` EQUALS "in custom-0 space and valid opcode".
      //    Overly permissive shim swallows illegal instruction trap;
      //    overly strict rejects valid instruction.
      //
      //    NOTE: Like property 2, this is a RESTATEMENT of `iss_accept_o = in_space`
      //    combined with `in_space` definition, so it cannot fire with current RTL.
      //    Its value is a REGRESSION test: broadening accept condition fires the property.
      //    This is valid but WEAKER than a state property, and the project's
      //    "27 formal properties" must be understood in this context.
      //
      //
      assert (iss_accept_o == ((iss_instr_i[6:0] == ECG_MAJOR_OPCODE)
                               && (iss_instr_i[ECG_OP_MSB:ECG_OP_LSB]
                                   < 5'(ECG_N_OPCODE))
                               && ecg_enc_defined(
                                    iss_instr_i[ECG_OP_MSB:ECG_OP_LSB],
                                    iss_instr_i[ECG_FN3_MSB:ECG_FN3_LSB])));

      // 4. Never more than ONE layer active concurrently. Overlapping layers would corrupt
      //    shared buffer layout in ecg_coproc.
      //
      //    Earlier assertion was `!(cp_start_o && cp_busy_i)` which fired at cycle 4 --
      //    correctly, because `start` high while `busy` RISES is standard behavior:
      //    coprocessor raises busy IN RESPONSE TO start. Assertion was incorrect, not RTL.
      //    Correct property requires active layer counter and environment model:
      //    `busy` strictly equals "layer currently active".
      assert (f_out_q <= 2'd1);
    end
  end
`endif

endmodule

`endif  // ECG_CVXIF_SV

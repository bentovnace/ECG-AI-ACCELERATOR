// Bridge between CV-X-IF interface of CV32E40X and flat signal interface of
// ecg_cvxif. Exists because the two sides speak two different protocols:
//
//   - Core declares six `cv32e40x_if_xif` interfaces with `coproc_*` modports and structs
//     `x_issue_req_t` / `x_issue_resp_t` / `x_result_t`.
//   - ecg_cvxif declares flat ports (`iss_valid_i`, `iss_instr_i`, ...), and
//     does NOT have an `id` field.
//
// Prior to this file, core area measurement routed all 8 XIF inputs to
// top-level pins (`xif_drive_i`, see tools/gen_core_wrap.py). That was scaffolding,
// not design: occupying 1024 of 1602 pin rings on `cv32e40x_core_wrap`
// `cv32e40x_core_wrap` (75,4 % so voi 4,3 % cua ecg_coproc), va P35 do duoc rang
// pin access caused detailed routing divergence (98.212 vi pham,
// giam ~6 %/vong, chieu 64 vong ~ 37 gio). This bridge turns XIF into INTERNAL LOGIC,
// reducing pin overhead so measured area reflects a real subsystem
// rather than scaffolding.
//
// THREE PROTOCOL ADAPTATION FUNCTIONS that ecg_cvxif cannot satisfy directly:
// 
//
//   1. PASS `id`: ecg_cvxif lacks `id`, but `x_result_t` requires matching `result.id`.
//      In ecg_cvxif, `res_valid_q` is set only on accepted issue and cleared next cycle,
//      yielding exactly ONE result per accepted instruction, delayed by 1 cycle.
//      Thus a 1-entry id register is sufficient -- no FIFO needed.
//      
//
//   2. RESULT SKID BUFFER: CV-X-IF requires `result_valid` held until `result_ready`.
//      ecg_cvxif `res_valid_o` is a 1-cycle pulse. If core is not ready, data would be lost.
//      A 1-entry skid buffer captures this, and blocks new issues (`issue_ready` low)
//      while full, preventing subsequent instructions from overwriting the buffer.
//      
//      
//
//   3. DETECT `commit_kill`: ecg_cvxif acts at ISSUE time (pushes layer into FIFO
//      upon issue handshake), so a canceled instruction cannot be rewound.
//      A sticky flag (`kill_seen_o`) records if any canceled instruction was accepted.
//      
//      
//      
//      
//
//      
//      
//      
//      
//      
//
// Grounded channels: compressed rejected (`accept`=0); memory channel unused (`mem_valid`=0).
//   - 
//     
//     
//   - 
//     
//   - 

module ecg_xif_bridge #(
    parameter int unsigned X_ID_WIDTH = 4,
    parameter int unsigned XLEN       = 32
) (
    input  logic clk_i,
    input  logic rst_ni,

    // ── Core side: CV-X-IF interface, coprocessor modports ──────────
    cv32e40x_if_xif.coproc_compressed  xif_compressed_if,
    cv32e40x_if_xif.coproc_issue       xif_issue_if,
    cv32e40x_if_xif.coproc_commit      xif_commit_if,
    cv32e40x_if_xif.coproc_mem         xif_mem_if,
    cv32e40x_if_xif.coproc_mem_result  xif_mem_result_if,
    cv32e40x_if_xif.coproc_result      xif_result_if,

    // ── Shim side: ecg_cvxif flat ports ────────────────────────────
    output logic        iss_valid_o,
    output logic [31:0] iss_instr_o,
    output logic [31:0] iss_rs1_o,
    output logic [31:0] iss_rs2_o,
    input  logic        iss_ready_i,
    input  logic        iss_accept_i,
    input  logic        iss_wb_i,
    input  logic        res_valid_i,
    input  logic [4:0]  res_rd_i,
    input  logic [31:0] res_data_i,

    // Sticky flag: set if at least one accepted instruction saw `commit_kill`.
    // Cleared by reset. Indicates whether invariant holds.
    // 
    output logic        kill_seen_o,

    // ---- F11: COMMIT pulse for pending instruction -------------------------------
    // ecg_cvxif acts at ISSUE; forwarded commit is resolved by id matching.
    // Mutually exclusive pulses marking completion of pending instruction.
    // 
    // 
    //
    // 
    output logic        cmt_ok_o,      // lenh dang cho da commit, KHONG bi huy
    output logic        cmt_kill_o
);

  // ── Instruction issue ────────────────────────────────────────────────────────
  // `skid_full` chan Instruction issue de muc giu khong bi ghi de. Khong co dieu kien
  // 
  logic skid_full_q;

  // Symmetric backpressure on BOTH sides (`issue_ready` and `iss_valid_o`). 
  // 
  //
  //   1. coproc returns result -> skid_full_q <= 1
  //   2. bridge deasserts issue_ready to core
  //   3. core HOLDS issue_valid per CV-X-IF protocol
  //   4. bridge must also gate iss_valid_o so shim does not re-push
  //   
  //      
  //      
  //
  // 
  // Symmetric gating ensures exact handshake alignment between core and shim.
  // 
  //
  // 
  // 
  // Declarations placed before first use.
  // 
  // 
  // 
  // 
  // 
  logic [X_ID_WIDTH-1:0] id_pend_q;
  // `id_pend_valid_q` = exactly one accepted instruction pending response.
  // 
  // 
  logic                  id_pend_valid_q;

  // BRIDGE INVARIANT: ONE-OUTSTANDING. At most one in-flight instruction.
  // `iss_acceptable` is the single condition on both sides.
  // 
  //
  // 
  // 
  // 
  // 
  // 
  // 
  //     
  //     
  //     
  // 
  // 
  //
  // 
  // 
  logic nhan_duoc;
  // MUC XIF-03: !skid_full_q gates incoming requests when skid buffer is full.
  // !id_pend_valid_q enforces MUC XIF-01 (one-outstanding).
  // 
  assign nhan_duoc = !skid_full_q && !id_pend_valid_q;

  // F11: Match commit id against pending instruction.
  // 
  // 
  // 
  logic cmt_matched;
  assign cmt_matched   = xif_commit_if.commit_valid && id_pend_valid_q
                   && (xif_commit_if.commit.id == id_pend_q);
  assign cmt_kill_o = cmt_matched &&  xif_commit_if.commit.commit_kill;
  assign cmt_ok_o   = cmt_matched && !xif_commit_if.commit.commit_kill;

  assign iss_valid_o = xif_issue_if.issue_valid && nhan_duoc;
  assign iss_instr_o = xif_issue_if.issue_req.instr;
  assign iss_rs1_o   = xif_issue_if.issue_req.rs[0];
  assign iss_rs2_o   = xif_issue_if.issue_req.rs[1];

  assign xif_issue_if.issue_ready = iss_ready_i && nhan_duoc;

  always_comb begin
    xif_issue_if.issue_resp           = '0;
    xif_issue_if.issue_resp.accept    = iss_accept_i;
    xif_issue_if.issue_resp.writeback = iss_wb_i;
    // dualwrite = 0: each custom instruction writes at most one rd (verified in §C24).
    // dualread / loadstore / ecswrite / exc = 0: unused in this coprocessor.
    // 
    // 
  end

  // ── bat tay Instruction issue da hoan tat ────────────────────────────────────
  logic issue_done;
  assign issue_done = xif_issue_if.issue_valid && xif_issue_if.issue_ready
                   && iss_accept_i;

  // ── ID & result skid buffer ─────────────────────────────────────────────
  // Single entry is sufficient: ecg_cvxif returns results 1 cycle after handshake,
  // and issue_ready is blocked while skid buffer is full.
  logic [4:0]            rd_q;
  logic [31:0]           data_q;

  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
      id_pend_q       <= '0;
      id_pend_valid_q <= 1'b0;
      rd_q        <= '0;
      data_q      <= '0;
      skid_full_q <= 1'b0;
      kill_seen_o <= 1'b0;
    end else begin
      // MUC XIF-02: captures id at handshake; rd/data accompany result.
      // 
      //
      // EXCEPTION FIELD: ecg_cvxif has no exception sources; result.exc = 0
      // is correct behavior per specification.
      // 
      // 
      // 
      // 
      // 
      //
      // 
      // 
      // 
      // 
      // 
      // 
      // 
      // Latch id at issue handshake; result arrives one cycle later.
      if (issue_done) begin
        id_pend_q       <= xif_issue_if.issue_req.id;
        id_pend_valid_q <= 1'b1;
      end else if (skid_full_q && xif_result_if.result_ready) begin
        id_pend_valid_q <= 1'b0;
      end

      // Load skid buffer when shim emits result.
      if (res_valid_i) begin
        rd_q        <= res_rd_i;
        data_q      <= res_data_i;
        skid_full_q <= 1'b1;
      end else if (skid_full_q && xif_result_if.result_ready) begin
        skid_full_q <= 1'b0;
      end

      // Record commit_kill event for pending instruction.
      //
      // ID comparison required: only set flag if killed id matches pending.
      //   
      // 
      // 
      // 
      // 
      // 
      //
      // 
      // 
      // 
      // 
      if (cmt_kill_o) begin
        kill_seen_o <= 1'b1;
      end
    end
  end

`ifndef SYNTHESIS
  /* verilator coverage_off */
  // Verification state only; excluded from toggle coverage.
  // 
  // 
  // 
  // ── W1-A: Two bridge invariants for simulation ──────────────
  //
  // 1. F02: Payload must remain stable during `result_valid & !result_ready`.
  //    
  //    
  //    
  //    
  //    
  //
  // MUC XIF-05: Scoreboard counters.
  // 
  // 2. Scoreboard: `recv - kill - resp == outstanding`.
  //    
  //    
  //    
  //    
  //    
  //
  //    
  //    
  //    
  //    
  //    
  logic                  f_rv_q, f_rr_q;
  logic [4:0]            f_rd_q;
  logic [31:0]           f_data_q;
  logic [X_ID_WIDTH-1:0] f_id_q;
  logic [31:0]           f_n_recv_q, f_n_resp_q, f_n_killed_q;

  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
      f_rv_q <= 1'b0; f_rr_q <= 1'b0; f_rd_q <= '0; f_data_q <= '0; f_id_q <= '0;
      f_n_recv_q <= '0; f_n_resp_q <= '0; f_n_killed_q <= '0;
    end else begin
      // MUC XIF-07: payload stability check while valid and unacknowledged.
      // 
      // 
      // 1. hold payload
      if (f_rv_q && !f_rr_q) begin
        if (!xif_result_if.result_valid)
          $error("ecg_xif_bridge: result_valid HA trong luc chua duoc rut");
        if (xif_result_if.result.rd != f_rd_q
            || xif_result_if.result.data != f_data_q
            || xif_result_if.result.id != f_id_q)
          $error("ecg_xif_bridge: payload ket qua DOI trong luc valid & !ready");
      end
      f_rv_q   <= xif_result_if.result_valid;
      f_rr_q   <= xif_result_if.result_ready;
      f_rd_q   <= xif_result_if.result.rd;
      f_data_q <= xif_result_if.result.data;
      f_id_q   <= xif_result_if.result.id;

      // 2. scoreboard
      if (issue_done) f_n_recv_q <= f_n_recv_q + 1;
      if (xif_result_if.result_valid && xif_result_if.result_ready)
        f_n_resp_q <= f_n_resp_q + 1;
      if (xif_commit_if.commit_valid && xif_commit_if.commit.commit_kill
          && id_pend_valid_q && (xif_commit_if.commit.id == id_pend_q))
        f_n_killed_q <= f_n_killed_q + 1;
      // 
      // 
      // 
      // 
      // 
      // 
    end
  end

  final begin
    $display("ecg_xif_bridge: [W1-A] nhan %0d · tra %0d · huy %0d · outstanding cuoi %0d",
             f_n_recv_q, f_n_resp_q, f_n_killed_q, id_pend_valid_q);
    if (f_n_recv_q == 0)
      $display("ecg_xif_bridge: [W1-A] CANH BAO -- 0 lenh duoc nhan, nen hai bat bien tren dung TAM THUONG o luot nay");
  end
  /* verilator coverage_on */
`endif

  // ── Result return ──────────────────────────────────────────────────────
  assign xif_result_if.result_valid = skid_full_q;

  always_comb begin
    xif_result_if.result      = '0;
    xif_result_if.result.id   = id_pend_q;
    xif_result_if.result.rd   = rd_q;
    xif_result_if.result.data = data_q;
    // we: one bit per XLEN word. Every accepted instruction writes rd.
    // 
    xif_result_if.result.we   = {(32/XLEN){1'b1}};
    // exc / exccode / err / dbg / ecsdata / ecswe = 0: see §C24, exceptions unused.
    // 
  end

  // ── Grounded channels ────────────────────────────────────────
  assign xif_compressed_if.compressed_ready = 1'b1;
  always_comb begin
    xif_compressed_if.compressed_resp        = '0;
    xif_compressed_if.compressed_resp.accept = 1'b0;
  end

  assign xif_mem_if.mem_valid = 1'b0;
  always_comb begin
    xif_mem_if.mem_req = '0;
  end

  // xif_mem_result_if: input only on coproc_mem_result modport, undriven.

endmodule

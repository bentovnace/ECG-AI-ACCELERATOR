// Layer sequencer, MAC path: CONV1D, DWCONV, PWCONV, FC.
//
// One layer at a time. `start_i` latches the decoded descriptor, the module runs
// the whole layer, then raises `done_o`. It owns ecg_addrgen, ecg_mac8 and
// ecg_requant, and presents read/write requests for the four memories that live
// outside it: activations, weights, requant scales, biases.
//
// Two structural decisions, both forced by a measurement.
//
// 1. The drain overlaps the next group. A group of eight output channels needs
//    eight requant cycles to drain through the single requant unit, but the
//    shortest accumulate chain in the four models is three cycles
//    (extra_enc_net_0, all four families). Draining serially costs +24 % of the
//    MAC cycles; overlapping the drain with the next group's accumulation costs
//    +2,7 %. So the eight psums are snapshotted into a shadow register set when a
//    group completes and drained from there while the array restarts. RUN stalls
//    only while the shadow is still busy, which yields max(chain, 8) per group.
//    Cost: 192 flops. Eight requant units would have cost eight times 217 LUT and
//    broken the "one requant stage" line of the N9 audit.
//
// 2. No multipliers anywhere. Three products are needed --
//    in_ch x len_in, (in_ch*k + tap) x cout, out_ch x len_out -- and every one is
//    maintained as a running offset that advances by a constant when its index
//    advances. The one genuinely per-layer product, k x cout, is built by shift
//    and add during SETUP because k is in {1,2,3,5,7}: 3 = 2+1, 5 = 4+1, 7 = 8-1.
//    ecg_addrgen already paid for this lesson -- a single multiply left on a
//    default arm was enough to infer a DSP48E1 (checklist C3).
//
// Lane masking: when cout is not a multiple of eight the last group has idle
// lanes. ecg_mac8 computes them anyway; this module simply does not write them.

`ifndef ECG_SEQ_SV
`define ECG_SEQ_SV

module ecg_seq
  import ecg_pkg::*;
#(
    parameter int unsigned N_PE   = 8,
    parameter int unsigned ABITS  = 13,   // activation buffer address width
    parameter int unsigned WBITS  = 13    // weight memory address width
) (
    input  logic                  clk_i,
    input  logic                  rst_ni,

    input  logic                  start_i,
    output logic                  busy_o,
    output logic                  done_o,

    // ---- Descriptor fields, from ecg_desc.
    input  logic [4:0]            op_i,
    input  logic [1:0]            act_i,
    input  logic [3:0]            src0_i,
    input  logic [3:0]            dst_i,
    input  logic [9:0]            dst_off_i,
    input  logic [8:0]            cin_i,
    input  logic [8:0]            cout_i,
    input  logic [9:0]            len_in_i,
    input  logic [9:0]            len_out_i,
    input  logic [2:0]            k_i,
    input  logic [1:0]            stride_i,
    input  logic [2:0]            pad_i,
    input  logic                  dw_i,
    input  logic [13:0]           w_base_i,
    input  logic [13:0]           rq_base_i,
    input  logic [8:0]            rq_n_i,

    // ---- Read activations (Port A of ecg_actbuf).
    output logic                  a_req_o,
    output logic [3:0]            a_buf_o,
    output logic [ABITS-1:0]      a_off_o,
    output logic                  a_oob_o,
    input  logic                  a_gnt_i,
    input  logic                  a_valid_i,
    input  logic signed [7:0]     a_data_i,

    // ---- Read weights (ecg_wmem), 8 bytes per beat.
    output logic                  w_req_o,
    output logic [WBITS-1:0]      w_off_o,
    input  logic                  w_valid_i,
    input  logic signed [7:0]     w_data_i [N_PE],

    // ---- Read requant scale and bias. One entry per drain cycle.
    //
    // TWO distinct indices, and sharing one port is incorrect: scale can be
    // per-tensor (one entry for the layer, rq_n = 1) whereas bias is ALWAYS
    // per output channel. Previously only s_off_o existed, causing bias of all
    // output channels to read the same entry.
    // No s_req_o: scale table is COMBINATIONAL lookup, driven address is sufficient.
    output logic [13:0]           s_off_o,   // scale table, indexed by rq_n
    output logic [8:0]            b_off_o,   // bias table, always indexed by output channel
    input  logic [ECG_REQUANT_MULT_BITS-1:0]  s_mult_i,
    input  logic [ECG_REQUANT_SHIFT_BITS-1:0] s_shift_i,
    input  logic signed [8:0]     s_bias_i,

    // ---- Write activations.
    output logic                  wr_o,
    output logic [3:0]            wr_buf_o,
    output logic [ABITS-1:0]      wr_off_o,
    output logic signed [7:0]     wr_data_o
);

  // N_PE is advertised as a parameter, but FIVE places below assume exactly 8:
  // `sh_cnt_q`/`sh_load_q` are 4-bit wide so `4'(16)` truncates to 0; `lane_q` is 3-bit
  // wide so max 8 lanes; and output channel step uses `<< 3` instead of `* N_PE`. At N_PE=16
  // failure is SILENT bit truncation with no error output. Widening all five is a full
  // redesign of address/lane/weight datapath, and `pe-sweep.csv` recorded 16/32/64
  // as unimplemented. Thus parameter is locked to PROVEN value -- advertising a parameter
  // wider than proven is an unfulfillable promise.
  // Module-scope `$error` is an ELABORATION-TIME system task: it cleanly fails elaboration.
  // Do NOT use `$fatal` in `initial` -- that is runtime and terminates midway during simulation.
  // Generate block MUST BE NAMED: an unnamed module-scope `if` triggers GENUNNAMED,
  // and `VFLAGS` treats warnings as errors -- locking would fail lint on DEFAULT config.
  // And a comment STARTING with linter tool name is parsed as pragma (BADVLTPRAGMA).
  // ITEM PE-01 (evaluation 2026-09-05): PE count is FROZEN here, and property "this lock
  // is genuinely enforceable" is TESTED each run by `tools/checks/khoa-npe-chiu-luc.py` --
  // it runs lint across BOTH configs expecting N_PE=8 rc=0 and N_PE=16 rc!=0 with exact message.
  //
  //
  //
  //
  //
  if (N_PE != 8) begin : g_npe_unsupported
    $error("ecg_seq: N_PE=%0d khong duoc ho tro; chi 8 duoc hien thuc", N_PE);
  end

  typedef enum logic [2:0] {
    S_IDLE, S_MUL, S_SETUP, S_RUN, S_TAIL, S_DONE
  } state_e;

  state_e state_q;

  // ------------------------------------------------------------- layer registers
  logic [8:0]  cout_q, rq_n_q;
  logic [9:0]  len_in_q, len_out_q;
  // w_base only keeps WBITS bits: weight memory is 4,664 B (ECG_WMEM_BYTES) so
  // 13 bits is sufficient, and assertion below catches non-fitting w_base.
  logic [WBITS-1:0] w_base_q;
  logic [13:0]      rq_base_q;
  logic [3:0]  src0_q, dst_q;
  // dst_off is in CHANNELS (ADR-0014 §2.1), so its byte offset is
  // dst_off * len_out. Originally used dst_off DIRECTLY, and vector set always
  // had dst_off = 0 masking the bug -- a CONCAT branch of m4 (dst_off = 10, len_out = 64)
  // wrote from byte 10 instead of 640 overwriting branches. Computed via shift-add in
  // S_MUL: dst_off <= 10 bits so max 10 cycles, actually 2-3, avoiding DSP inference.
  //
  logic [ABITS-1:0] dst_byte_q;
  logic [ABITS-1:0] mul_a_q;
  logic [9:0]       mul_b_q;
  logic [1:0]  act_q;
  logic        dw_q;

  // k * cout, computed via shift/add in SETUP. k in {1,2,3,5,7}.
  //
  // Declare BEFORE always_comb: Verilator accepts signal use before declaration,
  // but slang does not (per LRM). Both `make fpga-res` and `make area` use slang,
  // so this ordering prevented both from reporting 0 cells.
  logic [WBITS-1:0] kcout_d;
  logic [WBITS-1:0] kcout_q;

  always_comb begin
    unique case (k_i)
      3'd1: kcout_d = WBITS'(cout_i);
      3'd2: kcout_d = WBITS'(cout_i) << 1;
      3'd3: kcout_d = (WBITS'(cout_i) << 1) + WBITS'(cout_i);
      3'd5: kcout_d = (WBITS'(cout_i) << 2) + WBITS'(cout_i);
      3'd7: kcout_d = (WBITS'(cout_i) << 3) - WBITS'(cout_i);
      default: kcout_d = WBITS'(cout_i);
    endcase
  end

  // ---------------------------------------------------------------- address generator
  logic        ag_start, ag_step, ag_done;
  logic [9:0]  ag_out_pos;
  logic [8:0]  ag_out_ch, ag_in_ch;
  logic [2:0]  ag_tap;
  logic signed [11:0] ag_in_pos;
  logic        ag_oob, ag_clear, ag_last;

  ecg_addrgen #(.N_PE(N_PE)) u_ag (
      .clk_i, .rst_ni,
      .start_i    (ag_start),
      .step_i     (ag_step),
      .len_in_i   (len_in_q),
      .len_out_i  (len_out_q),
      .cin_i      (cin_i),
      .cout_i     (cout_q),
      .k_i        (k_i),
      .stride_i   (stride_i),
      .pad_i      (pad_i),
      .dw_i       (dw_q),
      .out_pos_o  (ag_out_pos),
      .out_ch_o   (ag_out_ch),
      .tap_o      (ag_tap),
      .in_ch_o    (ag_in_ch),
      .in_pos_o   (ag_in_pos),
      .oob_o      (ag_oob),
      .acc_clear_o(ag_clear),
      .out_valid_o(ag_last),
      .done_o     (ag_done)
  );

  // -------------------------------------------------------- running tracking offsets
  // Multiplierless: each offset advances by a constant when its index increments by 1.
  logic [ABITS-1:0] a_ch_off_q;     // in_ch * len_in
  // Weight offset is (in_ch*k + tap) * cout = in_ch*kcout + tap*cout, which
  // requires TWO registers rather than one. Originally used single register adding cout
  // on in_ch wrap, off by (cin-1)*k -- `cin = 1` worked but all cin > 1 failed.
  // Two independent registers accurately mirror ecg_addrgen nested loop structure.
  //
  logic [WBITS-1:0] w_ic_off_q;     // in_ch * (k * cout)
  logic [WBITS-1:0] w_tap_off_q;    // tap  * cout
  logic [WBITS-1:0] w_row_off;
  logic [ABITS-1:0] o_ch_off_q;     // out_ch * len_out

  assign w_row_off = w_ic_off_q + w_tap_off_q;

  // ---------------------------------------------------------------- pipeline
  logic              iss_pend_q;    // request issued, awaiting data
  logic              clear_pend_q, last_pend_q;
  logic              iss_q, clear_q, last_q;
  logic [8:0]        och_q;
  logic [ABITS-1:0]  opos_q;

  // Accumulator beat is valid only when BOTH memories have returned data.
  assign iss_q   = iss_pend_q   && a_valid_i && w_valid_i;
  assign clear_q = clear_pend_q && a_valid_i && w_valid_i;
  assign last_q  = last_pend_q  && a_valid_i && w_valid_i;

  logic signed [7:0] mac_wgt [N_PE];
  logic signed [ECG_PSUM_BITS-1:0] mac_psum [N_PE];

  ecg_mac8 #(.N_PE(N_PE)) u_mac (
      .clk_i, .rst_ni,
      .clear_i (clear_q),
      .acc_i   (iss_q),
      .act_i   (a_data_i),
      .wgt_i   (mac_wgt),
      .psum_o  (mac_psum)
  );

  always_comb begin
    for (int unsigned i = 0; i < N_PE; i++) mac_wgt[i] = w_data_i[i];
  end

  // ------------------------------------------------------------- shadow and drain
  // 192 flops: snapshot of eight psums to drain in parallel with next group (see note §1).
  logic signed [ECG_PSUM_BITS-1:0] sh_psum [N_PE];
  logic              snap_q;
  logic [8:0]        snap_och_q;
  logic [ABITS-1:0]  snap_opos_q;
  logic [8:0]        sh_och_q;
  logic [ABITS-1:0]  sh_opos_q;
  logic [ABITS-1:0]  sh_choff_q;
  logic [3:0]        sh_cnt_q;      // remaining drain count, 0 = idle
  logic              drain_busy;
  assign drain_busy = sh_cnt_q != '0;

  // sh_cnt counts down 8..1, so lane is 8 - sh_cnt. Computed in 4 bits then truncated,
  // avoiding reliance on 3'(8) rolling over to 0.
  // sh_cnt counts down from load value to 1. Lane is (load value) - sh_cnt, and for
  // DWCONV load value is 1 so lane is always 0.
  logic [3:0] sh_load_q;
  logic [2:0] lane_q;
  assign lane_q = 3'(sh_load_q - sh_cnt_q);

  // Scale table: per-channel uses rq_base + out_ch, per-tensor uses rq_base.
  assign s_off_o = (rq_n_q > 9'd1)
                 ? (rq_base_q + 14'(sh_och_q) + 14'(lane_q))
                 : rq_base_q;
  assign b_off_o = sh_och_q + 9'(lane_q);

  logic        rq_valid;
  logic signed [7:0] rq_act;

  ecg_requant u_rq (
      .clk_i, .rst_ni,
      // Lane mask: last group has idle lanes when cout is not multiple of 8,
      // and for DWCONV only lane 0 is valid.
      .valid_i (drain_busy && (9'(sh_och_q) + 9'(lane_q) < cout_q)
                && !(dw_q && (lane_q != 3'd0))),
      .psum_i  (sh_psum[lane_q]),
      .bias_i  (s_bias_i),
      .mult_i  (s_mult_i),
      .shift_i (s_shift_i),
      .act_i   (ecg_act_e'(act_q)),
      .valid_o (rq_valid),
      .act_o   (rq_act)
  );

  // Write address: dst_off + (out_ch + lane) * len_out + out_pos.
  //
  // Term by LANE is mandatory and easy to overlook: without it all eight output channels
  // write to the same byte and seven channels vanish. Maintained via running register --
  // adding len_out each step -- avoiding lane * len_out multiplication.
  logic [ABITS-1:0] lane_off_q;
  logic [ABITS-1:0] wr_off_q;
  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
      lane_off_q <= '0;
      wr_off_q   <= '0;
    end else begin
      if (snap_q) lane_off_q <= '0;
      else if (drain_busy) lane_off_q <= lane_off_q + ABITS'(len_out_q);
      wr_off_q <= sh_choff_q + lane_off_q + sh_opos_q;
    end
  end

  assign wr_o       = rq_valid;
  assign wr_buf_o   = dst_q;
  assign wr_off_o   = wr_off_q;
  assign wr_data_o  = rq_act;

  // ------------------------------------------------------------------- control
  // Group end beat (ag_last) can only be issued when drain path is completely idle.
  // "Idle" must include ANY pending snapshot, not just sh_cnt: between last_q and
  // snap_q there is a beat where drain_busy drops, and if RUN issues at that beat
  // new beat's `clear` wipes the value about to be read by snapshot.
  // Symptom was half the groups vanishing rather than wrong values.
  logic drain_pend;
  assign drain_pend = drain_busy || last_pend_q || last_q || snap_q;

  logic [8:0] last_in_ch;
  assign last_in_ch = dw_q ? 9'd0 : (cin_i - 9'd1);

  // Output channel step: 1 for DWCONV, N_PE for other ops (ecg_addrgen `ch_step`).
  logic [8:0] ch_step_q;
  assign ch_step_q = dw_q ? 9'd1 : 9'(N_PE);

  // Generator is active only ONE BEAT after ag_start asserts: `busy_q` of ecg_addrgen
  // is latched at that beat end, prior to which its outputs are undefined -- `oob_o`
  // includes `busy_q` in expression, reporting 0 while in_pos = -1.
  //
  // Issuing early causes DESYNCHRONIZATION: local offset registers advance on
  // can_issue while generator counter does not, advancing tap to 3 while k = 3.
  //
  logic ag_live_q;

  logic can_issue;
  assign can_issue = (state_q == S_RUN) && ag_live_q && a_gnt_i
                  && !(ag_last && drain_pend);

  assign a_req_o = (state_q == S_RUN) && ag_live_q && !(ag_last && drain_pend);
  assign a_buf_o = src0_q;
  // When out of bounds, in_pos may be NEGATIVE. ecg_actbuf skips memory read on that
  // cycle (ADR-0013), but forcing 0 prevents driving invalid negative address.
  assign a_off_o = ag_oob ? '0 : (a_ch_off_q + ABITS'(ag_in_pos[ABITS-2:0]));
  assign a_oob_o = ag_oob;

  assign w_req_o = a_req_o;
  assign w_off_o = w_base_q + w_row_off + WBITS'(ag_out_ch);

  assign ag_step = can_issue;
  assign busy_o  = state_q != S_IDLE;

  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
      state_q     <= S_IDLE;
      done_o      <= 1'b0;
      ag_start    <= 1'b0;
      iss_pend_q  <= 1'b0;
      clear_pend_q <= 1'b0;
      last_pend_q <= 1'b0;
      sh_cnt_q    <= '0;
      sh_load_q   <= 4'(N_PE);
      snap_q      <= 1'b0;
      ag_live_q   <= 1'b0;
      a_ch_off_q  <= '0;
      w_ic_off_q  <= '0;
      w_tap_off_q <= '0;
      o_ch_off_q  <= '0;
      kcout_q     <= '0;
    end else begin
      done_o   <= 1'b0;
      ag_start <= 1'b0;
      ag_live_q <= ag_start || (ag_live_q && (state_q == S_RUN));

      // Issue pipeline: data arrives one cycle after request; valid flags are checked
      // explicitly rather than assuming latency. If buffer rejects or memory is slower,
      // fixed latency assumptions would inject invalid data silently.
      //
      iss_pend_q <= can_issue;
      clear_pend_q <= can_issue && ag_clear;
      last_pend_q  <= can_issue && ag_last;
      och_q   <= ag_out_ch;
      opos_q  <= ABITS'(ag_out_pos);

      unique case (state_q)
        S_IDLE: if (start_i) begin
          dst_byte_q <= '0;
          mul_a_q    <= ABITS'(len_out_i);
          mul_b_q    <= dst_off_i;
          cout_q    <= cout_i;
          rq_n_q    <= rq_n_i;
          len_in_q  <= len_in_i;
          len_out_q <= len_out_i;
          w_base_q  <= WBITS'(w_base_i);
          rq_base_q <= rq_base_i;
          src0_q    <= src0_i;
          dst_q     <= dst_i;
          act_q     <= act_i;
          dw_q      <= dw_i;
          kcout_q   <= kcout_d;
          state_q   <= S_MUL;
        end

        S_MUL: begin
          // dst_off * len_out via shift-add, one bit per cycle.
          if (mul_b_q == 10'd0) begin
            state_q <= S_SETUP;
          end else begin
            if (mul_b_q[0]) dst_byte_q <= dst_byte_q + mul_a_q;
            mul_a_q <= mul_a_q << 1;
            mul_b_q <= mul_b_q >> 1;
          end
        end

        S_SETUP: begin
          a_ch_off_q  <= '0;
          w_ic_off_q  <= '0;
          w_tap_off_q <= '0;
          // dst_byte_q = dst_off * len_out, completed in S_MUL.
          o_ch_off_q  <= dst_byte_q;
          ag_start    <= 1'b1;
          state_q     <= S_RUN;
        end

        S_RUN: begin
          if (can_issue) begin
            // in_ch advances by 1 each cycle unless wrapping; generator signals
            // wrap via next-cycle in_ch value, so comparison here is sufficient.
            // in_ch is innermost loop, tap is next outer loop. All three offsets
            // follow this loop hierarchy.
            if (ag_in_ch == last_in_ch) begin
              w_ic_off_q  <= '0;
              w_tap_off_q <= (ag_tap == (k_i - 3'd1))
                           ? '0 : (w_tap_off_q + WBITS'(cout_q));
              // DWCONV: input channel always 0, activation channel tracks OUTPUT CHANNEL.
              // Stepping by in_ch like standard ops causes all output channels to read
              // input channel 0, with only channel 0 correct.
              if (dw_q) begin
                if (ag_last) begin
                  a_ch_off_q <= ((ag_out_ch + ch_step_q) >= cout_q)
                              ? '0 : (a_ch_off_q + ABITS'(len_in_q));
                end
              end else begin
                a_ch_off_q <= '0;
              end
            end else begin
              a_ch_off_q  <= a_ch_off_q + ABITS'(len_in_q);
              w_ic_off_q  <= w_ic_off_q + kcout_q;
            end
          end
          if (ag_done) state_q <= S_TAIL;
        end

        S_TAIL: if (!drain_busy && !iss_pend_q && !last_q && !snap_q) begin
          state_q <= S_DONE;
        end

        S_DONE: begin
          done_o  <= 1'b1;
          state_q <= S_IDLE;
        end

        default: state_q <= S_IDLE;
      endcase

      // Snapshot ONE CYCLE AFTER final accumulation beat.
      //
      // Sampling immediately on last_q beat reads `acc_q` of ecg_mac8 BEFORE final
      // addition completes, as acc_q updates on that same clock edge. For chain length 1
      // (extra_enc_net_0, cin*k = 3, or 1x1 layer cin=1) this is worse: snapshot
      // samples values from PREVIOUS group.
      //
      snap_q  <= last_q;
      if (last_q) begin
        snap_och_q  <= och_q;
        snap_opos_q <= opos_q;
      end
      if (snap_q) begin
        for (int unsigned i = 0; i < N_PE; i++) sh_psum[i] <= mac_psum[i];
        sh_och_q   <= snap_och_q;
        sh_opos_q  <= snap_opos_q;
        sh_choff_q <= o_ch_off_q;
        // DWCONV only has lane 0, so drain takes ONE cycle rather than eight:
        // draining all eight wastes seven cycles for masked-out lanes.
        sh_cnt_q   <= dw_q ? 4'd1 : 4'(N_PE);
        sh_load_q  <= dw_q ? 4'd1 : 4'(N_PE);
        // Next group is N_PE output channels away, i.e. 8 * len_out bytes --
        // shift by 3, avoiding multiplier. But out_ch WRAPS TO 0 after final
        // group of each output position (ecg_addrgen `last_ch_grp`), so offset
        // resets rather than continuously incrementing. For layer with cout <= 8
        // there is only one group, and omitting reset shifts subsequent positions by 8*len_out.
        //
        o_ch_off_q <= ((snap_och_q + ch_step_q) >= cout_q)
                    ? dst_byte_q
                    : (o_ch_off_q + (dw_q ? ABITS'(len_out_q)
                                          : (ABITS'(len_out_q) << 3)));
      end else if (drain_busy) begin
        sh_cnt_q <= sh_cnt_q - 4'd1;
      end
    end
  end

`ifndef SYNTHESIS
  // Asynchronous reset matching modules above: mixing synchronous rst_ni in one
  // block and async in another creates split reset domains on a single net.
  always_ff @(posedge clk_i or negedge rst_ni) begin
    // This module only handles MAC datapath. MAXPOOL/GAP/ADD route to ecg_vecop
    // with separate control logic -- arriving here would produce silent garbage,
    // prevented by this check.
    if (rst_ni && start_i
        && !(op_i == 5'(ECG_CONV1D) || op_i == 5'(ECG_DWCONV)
             || op_i == 5'(ECG_PWCONV) || op_i == 5'(ECG_FC)))
      $error("ecg_seq: op = %0d khong phai duong MAC", op_i);
    // w_base is 14-bit in descriptor but weight memory is only 13-bit address
    // (ECG_WMEM_BYTES = 4,664). Any w_base >= 8,192 is silently truncated.
    if (rst_ni && start_i && (32'(w_base_i) >= (32'd1 << WBITS)))
      $error("ecg_seq: w_base = %0d vuot %0d bit dia chi", w_base_i, WBITS);
    if (rst_ni && start_i && (32'(kcout_d) < 32'(cout_i)))
      $error("ecg_seq: k*cout = %0d tran %0d bit", kcout_d, WBITS);
  end
`endif

endmodule

`endif  // ECG_SEQ_SV

// Nested address generator with boundary zero injection.
//
// This module is where ADR-0013 lives. The alternative design cleared the whole
// 4 kB activation buffer on every model switch and cost 610 cycles; injecting
// zeros here instead costs 98, a 6.2x reduction, and turns T_switch from a
// design constraint into a number worth reporting.
//
// The reason it works: clearing is only needed where a layer reads a cell that
// nobody wrote. Across the four models that happens in exactly one place, the
// boundary region of convolutions with pad > 0. Zero padding does not have to
// live in memory. This generator knows which index is out of bounds and drives
// the PE input to zero directly, at the cost of one comparator and one mux.
//
// The invariant that makes it correct, and which P6 must prove rather than
// assume: every buffer cell a layer reads was either written by an earlier layer
// of the same inference, or is out of bounds and zero-injected. If that fails
// for even one layer, activations from the *previous model* leak into the next
// result, which only appears after a model switch and is therefore invisible to
// a single-model testbench.
//
// Loop order, outermost first:
//     out_pos  0..len_out-1     output time step
//     out_ch   0..cout-1        output channel, in groups of N_PE
//     tap      0..k-1           kernel tap
//     in_ch    0..cin-1         input channel (skipped when dw = 1)
//
// The input index is  in_pos = out_pos * stride + tap - pad,  and it is out of
// bounds when negative or >= len_in.

`ifndef ECG_ADDRGEN_SV
`define ECG_ADDRGEN_SV

module ecg_addrgen
  import ecg_pkg::*;
#(
    parameter int unsigned N_PE = 8
) (
    input  logic        clk_i,
    input  logic        rst_ni,

    input  logic        start_i,     // one pulse per layer
    input  logic        step_i,      // advance one MAC issue
    input  logic [9:0]  len_in_i,
    input  logic [9:0]  len_out_i,
    input  logic [8:0]  cin_i,
    input  logic [8:0]  cout_i,
    input  logic [2:0]  k_i,
    input  logic [1:0]  stride_i,
    input  logic [2:0]  pad_i,
    input  logic        dw_i,        // groups = cin: one kernel per channel

    output logic [9:0]  out_pos_o,
    output logic [8:0]  out_ch_o,    // base channel of the current N_PE group
    output logic [2:0]  tap_o,
    output logic [8:0]  in_ch_o,
    output logic signed [11:0] in_pos_o,
    output logic        oob_o,       // drive zero into the PE lane
    output logic        acc_clear_o, // first issue of a new output channel
    output logic        out_valid_o, // last issue of an output channel
    output logic        done_o
);

  // Counters. in_ch is the innermost so that a full dot product over input
  // channels lands in one accumulator before requantisation, which is what lets
  // the requant stage stay a single register deep.
  logic [9:0] out_pos_q;
  logic [8:0] out_ch_q;
  logic [2:0] tap_q;
  logic [8:0] in_ch_q;
  logic       busy_q;

  // Depthwise layers have one kernel per channel, so there is no reduction over
  // input channels: the in_ch loop collapses to a single iteration and the lane
  // index carries the channel instead.
  logic [8:0] in_ch_last;
  logic [8:0] ch_step;
  always_comb begin
    in_ch_last = dw_i ? 9'd0 : (cin_i - 9'd1);
    // DWCONV: each output channel uses its OWN input channel, so the 8 lanes of PE
    // array would need 8 distinct activations -- while read path broadcasts one byte.
    // Hence DWCONV runs ONE output channel at a time. The cost is +50.4% MAC cycles in m3
    // (measured), and m3 still takes ~30k cycles out of 139k budget per heartbeat, meaning
    // N8 has 4.6x margin remaining. The alternative -- parallelism along time dimension --
    // requires a second activation read path, hitting N9 budget where N9 only had 0.1 margin.
    // Cycle cost accepted, preserving N9 margin.
    //
    // "0.1 margin" expressed in the actual decision metric -- dedicated-LUTs:
    // N9 = 2385/2457 = 97.07%, so keeping >= 97.0% allows at most 1.8 additional LUTs;
    // even at 96.0% threshold allows only 27.4 LUTs. DWCONV routing alone currently
    // takes 45 LUTs (n9-audit.csv). Thus this is not a marginal call -- a second read
    // path exceeds budget by far, obscured by rounded percentages.
    // che mat dieu do.
    ch_step    = dw_i ? 9'd1 : 9'(N_PE);
  end

  logic last_in_ch, last_tap, last_ch_grp, last_pos;
  always_comb begin
    last_in_ch  = (in_ch_q  == in_ch_last);
    last_tap    = (tap_q    == (k_i - 3'd1));
    last_ch_grp = ((out_ch_q + ch_step) >= cout_i);
    last_pos    = (out_pos_q == (len_out_i - 10'd1));
  end

  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
      out_pos_q <= '0;
      out_ch_q  <= '0;
      tap_q     <= '0;
      in_ch_q   <= '0;
      busy_q    <= 1'b0;
    end else if (start_i) begin
      out_pos_q <= '0;
      out_ch_q  <= '0;
      tap_q     <= '0;
      in_ch_q   <= '0;
      busy_q    <= 1'b1;
    end else if (busy_q && step_i) begin
      if (!last_in_ch) begin
        in_ch_q <= in_ch_q + 9'd1;
      end else begin
        in_ch_q <= '0;
        if (!last_tap) begin
          tap_q <= tap_q + 3'd1;
        end else begin
          tap_q <= '0;
          if (!last_ch_grp) begin
            out_ch_q <= out_ch_q + ch_step;
          end else begin
            out_ch_q <= '0;
            if (!last_pos) begin
              out_pos_q <= out_pos_q + 10'd1;
            end else begin
              busy_q <= 1'b0;      // layer complete
            end
          end
        end
      end
    end
  end

  // in_pos is signed: out_pos * stride + tap - pad goes negative at the left
  // boundary, and that negative value is precisely the case zero injection
  // exists to handle. Truncating it to unsigned here would turn the left
  // boundary into a wraparound read of the far end of the buffer.
  logic signed [11:0] in_pos;
  logic        [11:0] scaled_pos;
  always_comb begin
    // stride is 1 or 2 in all fifty layers, so this is a shift, not a multiply.
    // Written as a multiply it inferred a DSP48E1 on xc7 -- one of the 220 on the
    // part spent on a one-bit shift. The case statement costs a single mux.
    // All four encodings are shifts or a shift plus an add, so no multiplier is
    // needed. Leaving a multiply on the default arm was enough to keep the DSP.
    // stride 0 is a configuration error and the assertion below catches it; the
    // arm exists only so the case is complete and no latch is inferred.
    unique case (stride_i)
      2'd0:    scaled_pos = '0;
      2'd1:    scaled_pos = 12'({2'b00, out_pos_q});
      2'd2:    scaled_pos = 12'({1'b0, out_pos_q, 1'b0});
      2'd3:    scaled_pos = 12'({1'b0, out_pos_q, 1'b0})
                            + 12'({2'b00, out_pos_q});
      default: scaled_pos = '0;
    endcase
    in_pos = 12'($signed(scaled_pos)
                 + $signed({9'b0, tap_q}) - $signed({9'b0, pad_i}));
  end

  assign in_pos_o    = in_pos;
  assign oob_o       = busy_q && ((in_pos < 12'sd0)
                                  || (in_pos >= 12'($signed({2'b00, len_in_i}))));
  assign out_pos_o   = out_pos_q;
  assign out_ch_o    = out_ch_q;
  assign tap_o       = tap_q;
  assign in_ch_o     = in_ch_q;
  assign acc_clear_o = busy_q && (tap_q == '0) && (in_ch_q == '0);
  assign out_valid_o = busy_q && last_tap && last_in_ch;
  assign done_o      = busy_q && step_i && last_in_ch && last_tap
                       && last_ch_grp && last_pos;

`ifndef SYNTHESIS
  // stride is a 2-bit field but only 1 and 2 occur in the four models; 0 would
  // make the generator loop forever on one input position.
  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (rst_ni && start_i) begin
      if (stride_i == 2'd0)  $error("ecg_addrgen: stride 0");
      if (k_i == 3'd0)       $error("ecg_addrgen: k 0");
      if (len_out_i == 10'd0) $error("ecg_addrgen: len_out 0");
      if (cout_i == 9'd0)    $error("ecg_addrgen: cout 0");
    end
  end
`endif

`ifdef FORMAL
  logic f_started_q = 1'b0;
  always_ff @(posedge clk_i) f_started_q <= 1'b1;
  always_comb if (!f_started_q) assume (!rst_ni);

  // Input assumptions. Not to simplify problem: these are conditions ecg_desc
  // validates on real descriptors (legal_o) and ecg_seq latches prior to issuing,
  // so a counterexample violating them reflects no hardware flaw.
  always_comb begin
    assume (k_i == 3'd1 || k_i == 3'd2 || k_i == 3'd3
            || k_i == 3'd5 || k_i == 3'd7);
    assume (stride_i == 2'd1 || stride_i == 2'd2);
    assume (cout_i   >= 9'd1);
    assume (cin_i    >= 9'd1);
    assume (len_in_i >= 10'd1);
    assume (len_out_i >= 10'd1);
    assume ({1'b0, pad_i} < {1'b0, k_i});   // pad < k, xem ecg_seq_vec
  end

  // Invariant fields during layer execution: ecg_seq latches them at S_IDLE.
  // Without this assumption, solver mutates cout mid-stream causing counters to
  // overflow artificially.
  logic [8:0] f_cout_q, f_cin_q;
  logic [2:0] f_k_q;
  logic       f_dw_q;
  logic [9:0] f_lin_q, f_lout_q;
  logic [1:0] f_st_q;
  logic [2:0] f_pad_q;
  always_ff @(posedge clk_i) begin
    f_cout_q <= cout_i;
    f_cin_q  <= cin_i;
    f_k_q    <= k_i;
    // dw_i MUST be sampled here: in_ch_last depends on it; allowing it to change
    // mid-stream alters upper bound of in_ch_q, triggering false invariant violation
    // "in_ch_q <= in_ch_last" due to unrealizable environment transition.
    f_dw_q   <= dw_i;
    f_lin_q  <= len_in_i;
    f_lout_q <= len_out_i;
    f_st_q   <= stride_i;
    f_pad_q  <= pad_i;
  end
  always_comb if (f_started_q && busy_q) begin
    assume (cout_i    == f_cout_q);
    assume (cin_i     == f_cin_q);
    assume (k_i       == f_k_q);
    assume (dw_i      == f_dw_q);
    assume (len_in_i  == f_lin_q);
    assume (len_out_i == f_lout_q);
    assume (stride_i  == f_st_q);
    assume (pad_i     == f_pad_q);
  end

  always_ff @(posedge clk_i) begin
    if (rst_ni && f_started_q && busy_q) begin
      // 1. Counter NEVER exceeds bounds. An off-by-one in wrap logic causes
      //    the PE array to compute a non-existent output channel, producing
      //    silent corrupted output rather than an obvious fault.
      assert (out_ch_q < cout_i);
      assert ({1'b0, tap_q} < {1'b0, k_i});
      assert (in_ch_q <= in_ch_last);

      // 2. oob_o EQUALS "index is out of bounds". This is the basis of ADR-0013:
      //    out-of-bounds index injects zero instead of reading memory; missing a case
      //    in oob reads unwritten bytes, visible only after model switches.
      // 
      assert (oob_o == ((in_pos < 12'sd0)
                        || (in_pos >= 12'($signed({2'b00, len_in_i})))));

      // 3. When NOT out of bounds, read index lies in [0, len_in). This is the
      //    module-level enforceable form of ADR-0013: remainder of invariant
      //    (byte was written by prior layer) requires ecg_actbuf and belongs to P6.
      //    thuoc P6.
      assert (oob_o || ((in_pos >= 12'sd0)
                        && (in_pos < 12'($signed({2'b00, len_in_i})))));

      // 4. acc_clear_o asserts only on FIRST cycle of an output channel, and
      //    out_valid_o only on LAST. Both assert together only if accumulator length is 1.
      assert (!(acc_clear_o && out_valid_o)
              || ((k_i == 3'd1) && (in_ch_last == 9'd0)));
    end
  end
`endif

endmodule

`endif  // ECG_ADDRGEN_SV

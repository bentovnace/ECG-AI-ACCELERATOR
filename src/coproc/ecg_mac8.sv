// PE array: eight int8 MACs, eight independent accumulators.
//
// The eight lanes are eight OUTPUT CHANNELS, not eight terms of one dot product.
// That is the mapping the rest of the design already assumes and it is written
// down in two places: tools/cycle_model.py, whose layer_cycles says "song song
  // layer_cycles calculates "parallel along output channels" and derives T_infer,
// 16" decision (checklist C8) from ceil(cout / PE) scans; and ecg_addrgen, whose
// counter nest advances out_ch by N_PE while in_ch and tap run innermost.
//
// So one activation is broadcast to all eight lanes, each lane holds the weight
// of its own output channel, and each lane owns its own psum register. The
// reduction runs over (tap, in_ch) IN TIME, inside each lane.
//
// This module previously summed the eight lanes through an adder tree into a
// single psum -- reduction parallelism, the opposite mapping. It passed its own
// testbench because the testbench's reference summed eight lanes too: the harness
// and the RTL agreed with each other and both disagreed with the architecture.
// That is the failure the "independent implementation of the same SPEC" gate
// exists to catch, and it slipped through because the reference described this
// module's behaviour instead of the system's dataflow. The reference now derives
// its expectation from the loop nest in ecg_addrgen, not from this file.
//
// Lane masking is the sequencer's job, not this module's: when cout is not a
// multiple of eight the last scan has idle lanes, and the sequencer simply does
// not store their results. Masking here would need a cout comparison per lane and
// buy nothing.

`ifndef ECG_MAC8_SV
`define ECG_MAC8_SV

module ecg_mac8
  import ecg_pkg::*;
#(
    parameter int unsigned N_PE = 8
) (
    input  logic                       clk_i,
    input  logic                       rst_ni,

    // clear_i wins over acc_i: a new output position starts from zero, and the
    // bias is added later by ecg_requant in the output domain (ADR-0014 §2.3).
    input  logic                       clear_i,
    input  logic                       acc_i,

    // One activation, broadcast to all eight lanes. Eight weights, one per output channel.
    input  logic signed [7:0]                          act_i,
    // Array is UNPACKED, and this is intentional: with a packed array
    // `logic signed [N_PE-1:0][7:0]`, the signed keyword applies to WHOLE vector
    // while `wgt_i[i]` is a part-select -- and part-selects of packed arrays are
    // UNSIGNED. Multiplication would treat negative weights as large positive numbers,
    // producing garbage results. Experienced this exact bug earlier.
    input  logic signed [7:0]                          wgt_i [N_PE],

    // UNPACKED array. With packed 8 x 24 = 192 bit array, Verilator
    // represents elements as 32-bit words, causing `psum_o[j]` to index by WORD
    // rather than lane -- lane 0 matches coincidentally, while remaining lanes output noise.
    // Unpacked array eliminates bit slicing from harness.
    output logic signed [ECG_PSUM_BITS-1:0]            psum_o [N_PE]
);

  logic signed [ECG_PSUM_BITS-1:0] acc_q [N_PE];

  // 8 x 8 -> 16 bit signed product. -128 * -128 = 16,384 requires full 16 bits;
  // 15 bits would overflow on this single input case.
  logic signed [15:0] prod [N_PE];

  always_comb begin
    for (int unsigned i = 0; i < N_PE; i++) begin
      prod[i] = act_i * wgt_i[i];
    end
  end

  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
      for (int unsigned i = 0; i < N_PE; i++) acc_q[i] <= '0;
    end else if (clear_i) begin
      // Clear AND accumulate in same cycle: a new scan starts with first
      // product directly, avoiding an idle cycle. Splitting into two cycles
      // adds one cycle per output channel, deviating from cycle model.
      for (int unsigned i = 0; i < N_PE; i++) begin
        acc_q[i] <= acc_i ? ECG_PSUM_BITS'(prod[i]) : '0;
      end
    end else if (acc_i) begin
      for (int unsigned i = 0; i < N_PE; i++) begin
        acc_q[i] <= acc_q[i] + ECG_PSUM_BITS'(prod[i]);
      end
    end
  end

  always_comb begin
    for (int unsigned i = 0; i < N_PE; i++) psum_o[i] = acc_q[i];
  end

`ifndef SYNTHESIS
  // Accumulator overflow check. Longest sequence across four models is 210 terms
  // (m4.inc2.b2.0, cin=30 k=7), so theoretical max |psum| is
  // 210 x 127 x 127 = 3,387,090, fitting in 23 bits signed (ecg_pkg.sv §psum).
  // Firing this assertion indicates 24-bit width is insufficient, invalidating area estimates.
  // 
  logic signed [ECG_PSUM_BITS-1:0] nxt [N_PE];
  always_comb begin
    for (int unsigned i = 0; i < N_PE; i++) begin
      nxt[i] = acc_q[i] + ECG_PSUM_BITS'(prod[i]);
    end
  end

  // Asynchronous reset matches surrounding blocks; mixing synchronous and
  // asynchronous resets across blocks triggers Verilator SYNCASYNCNET warnings.
  // 
  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (rst_ni && acc_i && !clear_i) begin
      for (int unsigned i = 0; i < N_PE; i++) begin
        // Sign check: adding two numbers with same sign yielding opposite sign indicates overflow.
        if ((acc_q[i][ECG_PSUM_BITS-1] == prod[i][15])
            && (acc_q[i][ECG_PSUM_BITS-1] != nxt[i][ECG_PSUM_BITS-1]))
        // NUM-02: out of three architectural options, project chooses REJECTING OUT-OF-BOUND SHAPES;
        // exact rule: accumulator handles up to 511 terms with boundary data. `make sim-mac8`
        // tests both 511 (must pass) AND 512 (must trigger assertion), validating bound empirically.
        // 
        // 
          $error("ecg_mac8: lan %0d tran psum %0d bit", i, ECG_PSUM_BITS);
      end
    end
  end
`endif

`ifdef FORMAL
  // FUNCTIONAL property (differs from other 27 structural invariants):
  // proves that 24-bit psum is SUFFICIENT, not merely "unobserved overflow".
  //
  // $error above only catches overflow when simulation trajectory visits the case.
  // The formal goal is stronger: for all valid sequences, |psum| never overflows.
  // If untrue, ECG_PSUM_BITS would need expansion and area recalculated.
  // 
  // 
  //
  // Longest sequence across 4 families is 210 terms (m4.inc2.b2.0: cin=30, k=7).
  // This is an ENVIRONMENT constraint guaranteed by ecg_addrgen (loop tap x in_ch),
  // expressed here as `assume`.
  // Standard formal convention: without this assumption initial state is unconstrained,
  // causing acc_q to start at arbitrary values and falsely fire on step 1.
  // 
  // 
  logic f_started_q = 1'b0;
  always_ff @(posedge clk_i) f_started_q <= 1'b1;
  always_comb if (!f_started_q) assume (!rst_ni);

  // OVERRIDABLE term bound, default 210 -- default behavior UNCHANGED.
  //
  // RATIONALE: mutation test (`40-rtl/est/mac8_mut.sby`) relaxes assumption to
  // 211 terms while keeping bound at 210; counterexample requires ~211 cycles
  // since `nterm_q` increments by 1 per cycle. BMC at depth 24 cannot reach it.
  // With `-DECG_F_MAX_TERMS=12`, identical property structure generates counterexample
  // in ~13 cycles, validating test harness non-vacuity in seconds.
  // 
  // 
  // 
  //
  // Enclosed in `ifdef FORMAL`: zero impact on synthesis netlist.
`ifndef ECG_F_MAX_TERMS
  `define ECG_F_MAX_TERMS 210
`endif
  localparam int unsigned ECG_MAX_TERMS = `ECG_F_MAX_TERMS;
  localparam int          ECG_PSUM_MAX  = ECG_MAX_TERMS * ECG_QMAX * ECG_QMAX;

  logic [8:0] nterm_q;   // sized for 210 + margin
  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni)          nterm_q <= '0;
    else if (clear_i)     nterm_q <= acc_i ? 9'd1 : 9'd0;
    else if (acc_i)       nterm_q <= nterm_q + 9'd1;
  end

  always_comb begin
    assume (act_i >= -ECG_QMAX && act_i <= ECG_QMAX);
    for (int unsigned i = 0; i < N_PE; i++) begin
      assume (wgt_i[i] >= -ECG_QMAX && wgt_i[i] <= ECG_QMAX);
    end
    // Environment constraint: single scan does not exceed 210 terms.
    assume (nterm_q <= ECG_MAX_TERMS);
  end

  always_ff @(posedge clk_i) begin
    if (rst_ni) begin
      for (int unsigned i = 0; i < N_PE; i++) begin
        // p_acc_ti_le: lam manh de QUY NAP DUOC. Chi rieng |acc| <= PSUM_MAX thi
        // Strengthened inductive invariant: |acc| <= PSUM_MAX alone is not inductive.
        // Needs proportional form bounded by terms accumulated so far.
        assert (acc_q[i] <=  signed'(ECG_QMAX * ECG_QMAX) * signed'({1'b0, nterm_q}));
        assert (acc_q[i] >= -signed'(ECG_QMAX * ECG_QMAX) * signed'({1'b0, nterm_q}));
        // Sufficient psum capacity: follows from above with nterm_q <= 210.
        assert (acc_q[i] <=  ECG_PSUM_MAX);
        assert (acc_q[i] >= -ECG_PSUM_MAX);
      end
    end
  end
`endif

endmodule

`endif  // ECG_MAC8_SV

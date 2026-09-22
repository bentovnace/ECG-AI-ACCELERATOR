// MAXPOOL, GAP and ADD in one datapath.
//
// These three are the whole non-MAC side of the compute set, and they are one
// module rather than three so that two pieces of hardware are shared for real:
//
//   * one 8-bit compare/select serves MAXPOOL's running max AND both halves of
//     ADD's saturating clamp,
//   * one 24-bit adder serves GAP's accumulator AND ADD's sum.
//
// ADD is a STREAM of two elements, not a pair of simultaneous operands. That
// looks like a detail and is not: each ADD operand carries its own requant scale,
// so each has to pass through the single requant stage separately anyway, which
// means the two operands arrive one after the other. Once they do, the second
// activation read port exists for nobody -- CONCAT stopped needing one when
// ADR-0014 §2.1 removed its record. Dropping it takes the shared-logic fraction
// of the N9 audit from 90,1 % to 92,6 %, because that port was the thinnest
// family-specific item in the table (65 gates, m2 only). The cost is one extra
// cycle per added element, and T_infer for m2 is 31.558 cycles against a 139.000
// beat budget.
//
// Measured cost of the merged block: 124 LUT, 57 FF, 13 CARRY4, 0 DSP
// (`make fpga-res`). What this does NOT yet claim is a number for N9. The audit
// in tools/n9_audit.py is a hand gate estimate and already lists MAXPOOL's
// comparator and ADD's second read as separate items, so merging the modules
// does not move that table on its own. Whether the sharing shows up in the
// shared-logic fraction is C10's job -- N9 read off a real synthesis report --
// and until then the honest claim is the LUT count above, not a delta on 90,1 %.
//
// GAP emits a psum, not an int8: the divide by length is folded into the requant
// multiplier by the compiler (tools/compile_model.py, `s_in / length / s_out`),
// so this module must not scale. ADD emits int8 because both its operands were
// already requantised to the output scale before they got here, which is why the
// clamp lives on this side and not in ecg_requant.
//
// The window protocol is explicit rather than counted here: `first_i` opens an
// accumulation, `last_i` closes it and raises the output. The counters that decide
// where a window starts and ends belong to ecg_addrgen, which already owns the
// padding and the ADR-0013 boundary zeros; duplicating them here would give two
// places to disagree about where a window ends.

`ifndef ECG_VECOP_SV
`define ECG_VECOP_SV

module ecg_vecop
  import ecg_pkg::*;
#(
    localparam int unsigned PSUM_W = ECG_PSUM_BITS
) (
    input  logic                     clk_i,
    input  logic                     rst_ni,

    // ECG_MAXPOOL, ECG_GAP or ECG_ADD. Other opcodes are illegal.
    input  logic [4:0]               mode_i,

    input  logic                     valid_i,
    input  logic                     first_i,   // open window / begin accumulation
    input  logic                     last_i,    // close window, emit output
    input  logic signed [7:0]        a_i,

    output logic                     out_valid_o,
    output logic signed [7:0]        out_i8_o,   // MAXPOOL, ADD
    output logic signed [PSUM_W-1:0] out_psum_o  // GAP
);

  logic is_pool, is_gap, is_add;
  assign is_pool = (mode_i == 5'(ECG_MAXPOOL));
  assign is_gap  = (mode_i == 5'(ECG_GAP));
  assign is_add  = (mode_i == 5'(ECG_ADD));

  // ------------------------------------------------------- shared comparator
  // MAXPOOL needs "a > acc"; ADD needs dual comparator bounds. Both pass through
  // here. Clamper uses QMAX (127), not 128: symmetric int8 (isa.md §3.1).
  logic signed [PSUM_W-1:0] sum;
  logic signed [PSUM_W-1:0] acc_q;

  logic signed [PSUM_W-1:0] a_ext;
  assign a_ext = PSUM_W'(a_i);

  // Shared 24-bit adder: GAP accumulates over time series, ADD accumulates
  // exactly two terms. Both share single datapath, differing only at output stage.
  // 
  assign sum = (first_i ? PSUM_W'(0) : acc_q) + a_ext;

  logic gt;
  assign gt = a_ext > acc_q;

  // Next value of accumulation register.
  logic signed [PSUM_W-1:0] acc_d;
  always_comb begin
    if (is_pool) begin
      // Initialize window with first sample, not 0: all-negative window
      // returns 0 if initialized to 0, a bug apparent on negative data.
      // 
      acc_d = (first_i || gt) ? a_ext : acc_q;
    end else begin
      acc_d = sum;
    end
  end

  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) acc_q <= '0;
    else if (valid_i) acc_q <= acc_d;
  end

  // ------------------------------------------------------------------ dau ra
  // Saturating clamper, reusing comparator path above.
  // Compared at psum width, but output is 8-bit.
  // 
  logic signed [7:0] clamped;
  always_comb begin
    if (acc_d > PSUM_W'(ECG_QMAX)) clamped = 8'sd127;
    else if (acc_d < -PSUM_W'(ECG_QMAX)) clamped = -8'sd127;
    else clamped = acc_d[7:0];
  end

  logic                     ovalid_q;
  logic signed [7:0]        oi8_q;
  logic signed [PSUM_W-1:0] opsum_q;

  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
      ovalid_q <= 1'b0;
      oi8_q    <= '0;
      opsum_q  <= '0;
    end else begin
      ovalid_q <= valid_i && last_i;
      oi8_q    <= clamped;
      opsum_q  <= acc_d;
    end
  end

  assign out_valid_o = ovalid_q;
  assign out_i8_o    = oi8_q;
  assign out_psum_o  = opsum_q;

`ifndef SYNTHESIS
  logic acc_open_q;
  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) acc_open_q <= 1'b0;
    else if (valid_i) acc_open_q <= first_i ? !last_i : !last_i;
  end

  always_ff @(posedge clk_i) begin
    if (valid_i && !(is_pool || is_gap || is_add))
      $error("ecg_vecop: mode_i = %0d khong phai MAXPOOL/GAP/ADD", mode_i);
    // All three operations are streaming; data must not precede first_i.
    if (valid_i && !first_i && !acc_open_q)
      $error("ecg_vecop: du lieu den truoc first_i");
  end
`endif

`ifdef FORMAL
  // Initial state constraints (see ecg_requant.sv for rationale).
  logic f_started_q = 1'b0;
  always_ff @(posedge clk_i) f_started_q <= 1'b1;
  always_comb if (!f_started_q) assume (!rst_ni);

  // Mode must be valid: any other mode_i indicates sequencer bug,
  // outside the specification of this execution unit.
  always_comb assume (is_pool || is_gap || is_add);
  // Mode must remain stable between first and last of an operation stream.
  // Window tracked with dedicated register rather than referencing `acc_open_q`.
  // 
  // 
  ecg_op_e f_mode_q;
  logic    f_open_q;
  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
      f_mode_q <= ECG_MAXPOOL;
      f_open_q <= 1'b0;
    end else begin
      f_mode_q <= ecg_op_e'(mode_i);
      if (valid_i) f_open_q <= !last_i;
    end
  end
  always_comb if (f_started_q && f_open_q) assume (mode_i == 5'(f_mode_q));

  always_ff @(posedge clk_i) begin
    if (rst_ni && f_started_q) begin
      // 1. int8 output ALWAYS within symmetric domain [-127, 127].
      //    For ADD, sum of two int8s needs 9 bits; clamper restricts to domain.
      // 
      assert (!out_valid_o || (out_i8_o >= -8'sd127 && out_i8_o <= 8'sd127));

      // 2. MAXPOOL never outputs value SMALLER than arriving input sample.
      //    Guards against improper 0 initialization of all-negative windows.
      // 
      // 
      assert (!(valid_i && is_pool && !first_i) || (acc_d >= a_ext));

      // 3. GAP does not clamp: psum output must match accumulator register,
      //    as division by length is integrated into downstream requant scale.
      // 
      assert (!(out_valid_o && is_gap) || (out_psum_o == acc_q));
    end
  end
`endif

endmodule

`endif  // ECG_VECOP_SV

// Vector sequencer: MAXPOOL, GAP, ADD.
//
// Separate from ecg_seq because the three of them are memory-bound, not
// MAC-bound, and that changes the loop nest rather than just the datapath. The
// PE array's eight lanes are eight output channels sharing ONE broadcast
// activation, which pays off only when eight output channels read the same input
// byte -- true for CONV1D, PWCONV and FC, false for everything here. MAXPOOL, GAP
// and ADD each need a different byte per channel, and there is one activation
// read port (the N9 decision in ecg_actbuf), so they run one channel at a time.
//
// Measured consequence, and the reason this is acceptable: T_infer with the
// port-limited formulas is 14.898 / 31.558 / 32.382 / 35.278 cycles for the four
// families against a 139.000-cycle beat budget, so N8 still holds with 3,9x to
// 9,3x margin. The old cycle model divided these three by the PE count and
// understated m3 by 60 %.
//
// MAXPOOL padding: PyTorch pads with -inf, ADR-0013 injects zero. Those agree
// only while every pooled input is non-negative. Measured on 64 real DS2 beats,
// all four MAXPOOL layers of m1 and m4 have exactly zero negative inputs,
// because each is fed by a ReLU'd convolution -- but that is a property of these
// four models, not of the hardware. So this module does not inject zero for
// MAXPOOL: it SKIPS out-of-bounds taps and opens the window on the first valid
// sample. Correct for any sign, and it costs one flag.

`ifndef ECG_SEQ_VEC_SV
`define ECG_SEQ_VEC_SV

module ecg_seq_vec
  import ecg_pkg::*;
#(
    parameter int unsigned ABITS = 13
) (
    input  logic                  clk_i,
    input  logic                  rst_ni,

    input  logic                  start_i,
    output logic                  busy_o,
    output logic                  done_o,

    input  logic [4:0]            op_i,
    input  logic [1:0]            act_i,
    input  logic [3:0]            src0_i,
    input  logic [3:0]            src1_i,
    input  logic [3:0]            dst_i,
    input  logic [9:0]            dst_off_i,
    input  logic [8:0]            cout_i,
    input  logic [9:0]            len_in_i,
    input  logic [9:0]            len_out_i,
    input  logic [2:0]            k_i,
    input  logic [1:0]            stride_i,
    input  logic [2:0]            pad_i,
    input  logic [13:0]           rq_base_i,

    // SINGLE activation read port for all three ops. ADD reads two operands SEQUENTIALLY:
    // each operand has its own requant scale so must pass through single requant
    // stage sequentially anyway; sequential read eliminates need for second read port.
    // 
    output logic                  a_req_o,
    output logic [3:0]            a_buf_o,
    output logic [ABITS-1:0]      a_off_o,
    input  logic                  a_gnt_i,
    input  logic                  a_valid_i,
    input  logic signed [7:0]     a_data_i,

    // Requant scale: GAP 1 entry, ADD 2 entries (one per operand).
    output logic [13:0]           s_off_o,
    input  logic [ECG_REQUANT_MULT_BITS-1:0]  s_mult_i,
    input  logic [ECG_REQUANT_SHIFT_BITS-1:0] s_shift_i,

    output logic                  wr_o,
    output logic [3:0]            wr_buf_o,
    output logic [ABITS-1:0]      wr_off_o,
    output logic signed [7:0]     wr_data_o
);

  typedef enum logic [2:0] { V_IDLE, V_MUL, V_RUN, V_TAIL, V_DONE } state_e;
  state_e state_q;

  logic [4:0]  op_q;
  logic [1:0]  act_q;
  logic [3:0]  src0_q, src1_q, dst_q;
  logic [8:0]  cout_q;
  logic [9:0]  len_in_q, len_out_q, inner_q;
  logic [1:0]  stride_q;
  logic [2:0]  pad_q;
  logic [13:0] rq_base_q;
  // dst_off is per CHANNEL, so byte offset is dst_off * len_out -- same multiply
  // needed by ecg_seq. Handled correctly using shift-and-add logic.
  // 
  // 
  logic [ABITS-1:0] dst_byte_q;
  logic [ABITS-1:0] mul_a_q;
  logic [9:0]       mul_b_q;

  // Declare BEFORE usage. Verilator permits usage before declaration, slang does
  // not -- slang requires declaration before use.
  // 
  // 
  logic is_pool, is_gap, is_add;
  assign is_pool = (op_q == 5'(ECG_MAXPOOL));
  assign is_gap  = (op_q == 5'(ECG_GAP));
  assign is_add  = (op_q == 5'(ECG_ADD));

  logic       s2_valid_q, s2_first_q, s2_last_q, s2_opnd_q;
  logic [ABITS-1:0] s2_off_q;
  logic       s3_valid_q, s3_first_q, s3_last_q;
  logic [ABITS-1:0] s3_off_q;
  logic       opened_q;      // window opened with a valid sample

  // Counters: channel -> output position -> inner index. `inner_q` is loop bound:
  // k for MAXPOOL, len_in for GAP, 1 for ADD.
  logic [8:0]  ch_q;
  logic [9:0]  pos_q;
  logic [9:0]  t_q;

  logic last_t, last_pos, last_ch;
  assign last_t   = (t_q   == (inner_q - 10'd1));
  assign last_pos = (pos_q == (len_out_q - 10'd1));
  assign last_ch  = (ch_q  == (cout_q - 9'd1));

  // Read index in source buffer per operation.
  // pos * stride computed via SHIFT, not multiplier. Stride has few values,
  // handled via `unique case` as in ecg_addrgen (§C C3).
  // 
  // 
  logic signed [11:0] scaled_pos;
  always_comb begin
    unique case (stride_q)
      2'd0:    scaled_pos = 12'sd0;
      2'd1:    scaled_pos = 12'($signed({2'b00, pos_q}));
      2'd2:    scaled_pos = 12'($signed({1'b0, pos_q, 1'b0}));
      2'd3:    scaled_pos = 12'($signed({1'b0, pos_q, 1'b0}))
                          + 12'($signed({2'b00, pos_q}));
      default: scaled_pos = 12'($signed({2'b00, pos_q}));
    endcase
  end

  logic signed [11:0] in_pos;
  always_comb begin
    if (is_pool) in_pos = scaled_pos + 12'($signed({9'b0, t_q[2:0]}))
                                     - 12'($signed({9'b0, pad_q}));
    else         in_pos = 12'($signed({2'b00, t_q}));
  end

  logic oob;
  assign oob = (in_pos < 12'sd0)
            || (in_pos >= 12'($signed({2'b00, len_in_q})));

  // Incremental channel offsets, avoided multipliers.
  logic [ABITS-1:0] ch_in_off_q;    // ch * len_in
  logic [ABITS-1:0] ch_out_off_q;   // dst_off + ch * len_out

  // ADD: innermost loop length 2; step 0 reads src0, step 1 reads src1.
  assign a_buf_o = (is_add && (t_q == 10'd1)) ? src1_q : src0_q;
  // ADD reads same element across both buffers; innermost index is operand selector.
  // 
  assign a_off_o = is_add ? (ch_in_off_q + ABITS'(pos_q))
                          : (ch_in_off_q + ABITS'(in_pos[ABITS-2:0]));

  // MAXPOOL SKIPS out-of-bounds taps rather than injecting 0 (see header comments).
  // GAP and ADD never read out-of-bounds.
  logic skip;
  assign skip = is_pool && oob;

  // `last` phai gan vao tap HOP LE CUOI CUNG, khong vao t == k-1.
  //
  // `last` must attach to the FINAL VALID tap, not raw t == k-1.
  // For MAXPOOL k=3 pad=1, last output position has tap outside boundary;
  // assigning `last` to t == k-1 would prevent window closure on edge cases.
  // 
  logic tap_last_valid;
  assign tap_last_valid = last_t
                       || (is_pool
                           && ((in_pos + 12'sd1)
                               >= 12'($signed({2'b00, len_in_q}))));

  logic step;
  assign step = (state_q == V_RUN) && (skip || a_gnt_i);

  assign a_req_o = (state_q == V_RUN) && !skip;

  // Scale: GAP 1 entry; ADD 2 consecutive entries, one per operand.
  //
  // Index tracks RETURNING operand, not REQUESTED operand:
  // requant executes in stage 2, one cycle after request emission.
  // 
  // 
  assign s_off_o = rq_base_q + (is_add ? 14'({13'b0, s2_opnd_q}) : 14'd0);

  // ---------------------------------------------------------------- duong ong
  //
  // Requant stage sits at TWO DIFFERENT POSITIONS depending on operation:
  // co ba tang chu khong hai:
  //
  //   MAXPOOL : read -> vecop -> write          (no requant)
  //   GAP     : read -> vecop -> requant -> write (psum to int8, once per channel)
  //   ADD     : read -> requant -> vecop -> write (per operand scale)
  //
  // Vecop control delayed by one additional cycle for ADD.
  // Stage 2: data returned from buffer.
  logic s2_hit;
  assign s2_hit = s2_valid_q && a_valid_i;

  // Requant truoc vecop (ADD) hay sau vecop (GAP).
  logic              rq_valid_i;
  logic signed [ECG_PSUM_BITS-1:0] rq_psum_i;
  logic              rq_valid;
  logic signed [7:0] rq_act;

  logic              vo_valid;
  logic signed [7:0] vo_i8;
  logic signed [ECG_PSUM_BITS-1:0] vo_psum;

  assign rq_valid_i = is_add ? s2_hit : (vo_valid && is_gap);
  assign rq_psum_i  = is_add ? ECG_PSUM_BITS'(a_data_i) : vo_psum;

  ecg_requant u_rq (
      .clk_i, .rst_ni,
      .valid_i (rq_valid_i),
      .psum_i  (rq_psum_i),
      .bias_i  (9'sd0),          // GAP va ADD khong co bias
      .mult_i  (s_mult_i),
      .shift_i (s_shift_i),
      // ACT_NONE always. For ADD, requant operates PER OPERAND while ReLU
      // applies AFTER addition; ReLU located in write stage below.
      // 
      // 
      .act_i   (ECG_ACT_NONE),
      .valid_o (rq_valid),
      .act_o   (rq_act)
  );

  ecg_vecop u_vec (
      .clk_i, .rst_ni,
      .mode_i      (op_q),
      .valid_i     (is_add ? s3_valid_q : s2_hit),
      .first_i     (is_add ? s3_first_q : s2_first_q),
      .last_i      (is_add ? s3_last_q  : s2_last_q),
      .a_i         (is_add ? rq_act : a_data_i),
      .out_valid_o (vo_valid),
      .out_i8_o    (vo_i8),
      .out_psum_o  (vo_psum)
  );

  // Write address delayed to MATCH datapath depth across operations:
  // nhau:
  //   MAXPOOL : enters vecop at s2, outputs 1 cycle later
  //   ADD     : enters vecop at s3 (post-requant), outputs 1 cycle later
  //   GAP     : enters vecop at s2, 1 cycle later to requant, 1 cycle to output
  // 
  // 
  logic [ABITS-1:0] vec_in_off;
  assign vec_in_off = is_add ? s3_off_q : s2_off_q;

  logic vec_in_valid;
  assign vec_in_valid = is_add ? s3_valid_q : s2_hit;

  logic [ABITS-1:0] off_vo_q;    // thang hang voi vo_valid
  logic [ABITS-1:0] off_rq_q;    // aligned with rq_valid (GAP only)

  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
      off_vo_q <= '0;
      off_rq_q <= '0;
    end else begin
      if (vec_in_valid) off_vo_q <= vec_in_off;
      if (vo_valid)     off_rq_q <= off_vo_q;
    end
  end

  // ReLU at write stage: clamp, solely active for ADD.
  logic signed [7:0] wr_raw;
  assign wr_raw = is_gap ? rq_act : vo_i8;

  assign wr_o      = is_gap ? rq_valid : vo_valid;
  assign wr_buf_o  = dst_q;
  assign wr_off_o  = is_gap ? off_rq_q : off_vo_q;
  assign wr_data_o = (act_q == 2'(ECG_ACT_RELU)) ? ((wr_raw < 8'sd0) ? 8'sd0
                                                                    : wr_raw)
                                                 : wr_raw;

  assign busy_o = state_q != V_IDLE;

  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
      state_q      <= V_IDLE;
      done_o       <= 1'b0;
      s2_valid_q   <= 1'b0;
      s2_first_q   <= 1'b0;
      s2_last_q    <= 1'b0;
      s2_opnd_q    <= 1'b0;
      s3_valid_q   <= 1'b0;
      s3_first_q   <= 1'b0;
      s3_last_q    <= 1'b0;
      opened_q     <= 1'b0;
      ch_q         <= '0;
      pos_q        <= '0;
      t_q          <= '0;
      ch_in_off_q  <= '0;
      ch_out_off_q <= '0;
    end else begin
      done_o <= 1'b0;

      unique case (state_q)
        V_IDLE: if (start_i) begin
          dst_byte_q <= '0;
          mul_a_q    <= ABITS'(len_out_i);
          mul_b_q    <= dst_off_i;
          op_q       <= op_i;
          act_q      <= act_i;
          src0_q     <= src0_i;
          src1_q     <= src1_i;
          dst_q      <= dst_i;
          cout_q     <= cout_i;
          len_in_q   <= len_in_i;
          len_out_q  <= len_out_i;
          stride_q   <= stride_i;
          pad_q      <= pad_i;
          rq_base_q  <= rq_base_i;
          // Innermost loop: k for MAXPOOL, len_in for GAP, 2 for ADD --
          // two operands read sequentially via single port.
          // 
          inner_q    <= (op_i == 5'(ECG_MAXPOOL)) ? 10'({7'b0, k_i})
                      : (op_i == 5'(ECG_GAP))     ? len_in_i
                                                  : 10'd2;
          ch_q         <= '0;
          pos_q        <= '0;
          t_q          <= '0;
          ch_in_off_q  <= '0;
          opened_q     <= 1'b0;
          state_q      <= V_MUL;
        end

        V_MUL: begin
          if (mul_b_q == 10'd0) begin
            ch_out_off_q <= dst_byte_q;
            state_q      <= V_RUN;
          end else begin
            if (mul_b_q[0]) dst_byte_q <= dst_byte_q + mul_a_q;
            mul_a_q <= mul_a_q << 1;
            mul_b_q <= mul_b_q >> 1;
          end
        end

        V_RUN: begin
          if (step) begin
            if (!last_t) begin
              t_q <= t_q + 10'd1;
            end else begin
              t_q      <= '0;
              opened_q <= 1'b0;
              if (!last_pos) begin
                pos_q <= pos_q + 10'd1;
              end else begin
                pos_q <= '0;
                if (!last_ch) begin
                  ch_q         <= ch_q + 9'd1;
                  ch_in_off_q  <= ch_in_off_q + ABITS'(len_in_q);
                  ch_out_off_q <= ch_out_off_q + ABITS'(len_out_q);
                end else begin
                  state_q <= V_TAIL;
                end
              end
            end
            if (!skip) opened_q <= tap_last_valid ? 1'b0 : 1'b1;
          end
        end

        V_TAIL: if (!s2_valid_q && !s3_valid_q && !vo_valid && !rq_valid)
                  state_q <= V_DONE;

        V_DONE: begin
          done_o  <= 1'b1;
          state_q <= V_IDLE;
        end

        default: state_q <= V_IDLE;
      endcase

      // Stage 1 -> 2: request issued this cycle, data arrives next cycle. `first`
      // is first VALID sample of window, not tap 0: MAXPOOL skips out-of-bounds taps.
      // 
      s2_valid_q <= step && !skip;
      s2_first_q <= step && !skip && !opened_q;
      s2_last_q  <= step && !skip && tap_last_valid;
      s2_off_q   <= ch_out_off_q + ABITS'(pos_q);
      s2_opnd_q  <= t_q[0];

      // Stage 2 -> 3 (ADD only): one cycle for per-operand requant.
      s3_valid_q <= s2_hit;
      s3_first_q <= s2_hit && s2_first_q;
      s3_last_q  <= s2_hit && s2_last_q;
      s3_off_q   <= s2_off_q;
    end
  end

`ifndef SYNTHESIS
  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (rst_ni && start_i
        && !(op_i == 5'(ECG_MAXPOOL) || op_i == 5'(ECG_GAP)
             || op_i == 5'(ECG_ADD)))
      $error("ecg_seq_vec: op = %0d khong phai MAXPOOL/GAP/ADD", op_i);
    // A MAXPOOL window consisting entirely of out-of-bounds taps yields no output,
    // which represents an invalid descriptor: with pad < k every window contains >= 1 valid tap.
    // 
    if (rst_ni && start_i && (op_i == 5'(ECG_MAXPOOL))
        && ({1'b0, pad_i} >= {1'b0, k_i}))
      $error("ecg_seq_vec: MAXPOOL pad = %0d >= k = %0d", pad_i, k_i);
    // MAXPOOL negative values: skips out-of-bounds taps and initializes window with first valid sample.
    //
    // Verified on DS2 64 beats: all 4 MAXPOOL layers of m1 and m4 contain non-negative
    // elements, as each receives from a prior ReLU'd conv.
    // Skipping boundary taps avoids data-dependent 0 injection assumptions.
    // 
    // 
    // 
    // minh dieu do.
  end
`endif

endmodule

`endif  // ECG_SEQ_VEC_SV

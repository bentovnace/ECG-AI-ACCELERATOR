// The coprocessor: one inference, one model, from a blob.
//
// This is the module N2 is about -- bit-exactness of the whole datapath against
// the integer reference. It owns the activation buffer, the weight memory, the
// descriptor decode and the two sequencers, and it walks the descriptor table one
// layer at a time.
//
// Three things worth naming.
//
// Buffer bases come from the descriptors, one at a time. Each layer descriptor
// carries the byte base of ITS OWN destination buffer in the 13 low bits, and
// this module writes that into the base register file of ecg_actbuf when it
// launches the layer. Nothing carries the base of a SOURCE buffer, and nothing
// needs to: a buffer is always written before it is read, so by the time a layer
// reads buffer n, the layer that wrote n has already programmed n's base. That is
// what makes a 16-byte descriptor enough (ADR-0014).
//
// The two input ports are at a fixed place, derived not stored. The ECG window
// sits at byte 0 and the RR features immediately after it, so their bases are 0
// and in_len -- both known from the Model Descriptor. tools/compile_model.py
// asserts that its allocator produces exactly this layout, so the convention is a
// contract rather than a coincidence.
//
// Two sequencers, not one. ecg_seq drives the MAC array for CONV1D/DWCONV/PWCONV/
// FC; ecg_seq_vec drives MAXPOOL/GAP/ADD, which are memory-bound and run one
// channel at a time. Only one is ever active, so their memory requests are muxed
// rather than arbitrated.

`ifndef ECG_COPROC_SV
`define ECG_COPROC_SV

module ecg_coproc
  import ecg_pkg::*;
#(
    // ---- Bon khoa tinh nang, dung DUY NHAT de do N9 bang phep loai tru.
    //
    // Bang n9_audit.py la uoc luong gate viet tay. De co mot con so DO duoc,
    // cach la tong hop lai voi tung tinh nang bi tat va lay hieu so cell. Cac
    // khoa nay khong tao duong du lieu moi: chung ep mot truong descriptor thanh
    // hang so, va yosys lan hang so do di roi cat het phan logic phu thuoc.
    //
    // Ho nao can gi (do tu ban ghi that, tools/opcode_audit.py):
    //   m1 CNN       : MAXPOOL, per-channel
    //   m2 ResNet    : ADD,     per-channel
    //   m3 MobileNet : DWCONV
    //   m4 Inception : MAXPOOL
    parameter bit EN_MAXPOOL = 1'b1,
    parameter bit EN_ADD     = 1'b1,
    parameter bit EN_DW      = 1'b1,
    parameter bit EN_PERCH   = 1'b1,

    parameter int unsigned N_PE  = 8,
    parameter int unsigned ABITS = $clog2(ECG_ACT_BYTES),
    parameter int unsigned WBITS = $clog2(ECG_WMEM_BYTES)
) (
    input  logic                  clk_i,
    input  logic                  rst_ni,

    // ---- Chay mot lan suy luan.
    input  logic                  start_i,
    // Two layer dispatch modes, both fully supported:
    // EXEC-02: single-layer mode is NOT random access -- measured at
    // tb_ecg_negative.cpp:879. Conditional clause applies, see `b_off_o`.
    //   single_i = 0: module walks descriptor table from layer 0 to `last`.
    //   single_i = 1: execute layer `layer_i` -- VALID ONLY WHEN issued SEQUENTIALLY.
    // Execution model in docs/isa.md §4.1 is SINGLE LAYER mode: core issues one
    // compute instruction per layer into CV-X-IF shim FIFO, and instruction stream
    // for a beat is fixed per model, stored in flash. Auto-walk mode is what N2
    // verifies (make sim-coproc) and is the fastest path to run a full model
    // in simulation. Both are retained as they answer two distinct requirements.
    input  logic                  single_i,
    input  logic [5:0]            layer_i,
    input  logic [5:0]            n_layers_i,   // tu Model Descriptor
    input  logic [9:0]            in_len_i,     // 256; nen cua cong RR
    output logic                  busy_o,
    output logic                  done_o,

    // ---- Descriptor table read: one 16 B record per layer.
    output logic                  desc_req_o,
    output logic [5:0]            desc_idx_o,
    input  logic                  desc_valid_i,
    input  logic [ECG_DESC_BYTES*8-1:0] desc_word_i,

    // ---- Requant scale table and bias table.
    output logic [13:0]           s_off_o,
    // 10-bit: bias table is ONE flat table for entire model; layer base offset
    // is ACCUMULATED rather than stored in descriptor -- all 128 bits used. All
    // multiplying layers across all 4 models have bias, so accumulating cout
    // after each layer is an exact rule. Total table: 105/133/221/103 entries
    // for m1..m4, exactly 562 B as listed in N7.
    output logic [9:0]            b_off_o,
    input  logic [ECG_REQUANT_MULT_BITS-1:0]  s_mult_i,
    input  logic [ECG_REQUANT_SHIFT_BITS-1:0] s_shift_i,
    input  logic signed [8:0]     s_bias_i,

    // ---- Weight DMA (LOADW), passes straight to ecg_wmem.
    input  logic                  dma_start_i,
    input  logic [WBITS:0]        dma_len_i,
    output logic                  dma_busy_o,
    output logic                  dma_done_o,
    input  logic                  ws_valid_i,
    input  logic [63:0]           ws_data_i,
    output logic                  ws_ready_o,

    // ---- Input loader: active only when idle, shares activation buffer write port.
    input  logic                  pre_we_i,
    input  logic [3:0]            pre_buf_i,
    input  logic [ABITS-1:0]      pre_off_i,
    input  logic signed [7:0]     pre_data_i,

    // ---- Write monitor stream. Final layer (last = 1) outputs results here.
    output logic                  wr_o,
    output logic [3:0]            wr_buf_o,
    output logic [ABITS-1:0]      wr_off_o,
    output logic signed [7:0]     wr_data_o,
    output logic                  wr_last_layer_o,
    // Single-cycle pulse when an ILLEGAL descriptor is fetched. Latched by ecg_mmio
    // into a sticky bit readable via ERRSTAT: an external audit must be able to observe
    // this, which `$error` inside `ifndef SYNTHESIS` cannot provide.
    output logic                  desc_illegal_o
);

  typedef enum logic [2:0] {
    C_IDLE, C_BASE, C_FETCH, C_LAUNCH, C_RUN, C_NEXT, C_DONE
  } state_e;

  state_e state_q;
  logic [5:0] layer_q;
  logic [9:0] bias_base_q;   // bias base offset of active layer

  // ------------------------------------------------------------- giai ma
  logic [4:0]  d_op;
  logic [1:0]  d_act;
  logic [3:0]  d_src0, d_src1, d_dst;
  logic [9:0]  d_dst_off, d_len_in, d_len_out;
  logic [8:0]  d_cin, d_cout, d_rq_n;
  logic [2:0]  d_k, d_pad;
  logic [1:0]  d_stride;
  logic        d_dw, d_last, d_cat_part;
  logic [13:0] d_w_base, d_rq_base;
  logic [12:0] d_buf_base;
  logic        d_is_mac, d_is_pool, d_is_gap, d_is_add, d_is_store;
  logic        d_uses_src1, d_per_ch, d_relu, d_legal;

  ecg_desc u_desc (
      .word_i      (desc_word_i),
      .valid_i     (desc_valid_i && (state_q == C_FETCH)),
      .op_o        (d_op),        .act_o      (d_act),
      .src0_o      (d_src0),      .src1_o     (d_src1),
      .dst_o       (d_dst),       .dst_off_o  (d_dst_off),
      .cin_o       (d_cin),       .cout_o     (d_cout),
      .len_in_o    (d_len_in),    .len_out_o  (d_len_out),
      .k_o         (d_k),         .stride_o   (d_stride),
      .pad_o       (d_pad),       .dw_o       (d_dw),
      .w_base_o    (d_w_base),    .rq_base_o  (d_rq_base),
      .rq_n_o      (d_rq_n),      .last_o     (d_last),
      .cat_part_o  (d_cat_part),  .buf_base_o (d_buf_base),
      .is_mac_o    (d_is_mac),    .is_pool_o  (d_is_pool),
      .is_gap_o    (d_is_gap),    .is_add_o   (d_is_add),
      .is_store_o  (d_is_store),  .uses_src1_o(d_uses_src1),
      .per_ch_o    (d_per_ch),    .relu_o     (d_relu),
      .legal_o     (d_legal)
  );

  // Latch descriptor on arrival so fields remain stable during layer execution.
  logic [4:0]  q_op;
  logic [1:0]  q_act;
  logic [3:0]  q_src0, q_src1, q_dst;
  logic [9:0]  q_dst_off, q_len_in, q_len_out;
  logic [8:0]  q_cin, q_cout, q_rq_n;
  logic [2:0]  q_k, q_pad;
  logic [1:0]  q_stride;
  logic        q_dw, q_last;
  logic [13:0] q_w_base, q_rq_base;
  logic        q_is_mac;

  // ------------------------------------------------------- activation buffer
  logic              ab_base_we;
  logic [3:0]        ab_base_sel;
  // Beat start pulse (NOT per layer). In single-layer mode, firmware issues one
  // COMPUTE instruction per layer for the SAME beat, so `start_i` cannot differentiate.
  // The condition `!single_i || layer_i == 0` is the EXACT check C_IDLE uses
  // for `bias_base_q` -- reused here rather than inventing new logic.
  logic              ab_infer_start;
  assign ab_infer_start = (state_q == C_IDLE) && start_i
                       && (!single_i || (layer_i == 6'd0));
  logic [ABITS-1:0]  ab_base;

  logic              ab_req, ab_gnt, ab_rvalid;
  logic [3:0]        ab_buf;
  logic [ABITS-1:0]  ab_off;
  logic              ab_oob;
  logic signed [7:0] ab_rdata;

  logic              ab_we;
  logic [3:0]        ab_wbuf;
  logic [ABITS-1:0]  ab_woff;
  logic signed [7:0] ab_wdata;

  ecg_actbuf #(.BYTES(ECG_ACT_BYTES)) u_ab (
      .clk_i, .rst_ni,
      .base_we_i (ab_base_we),
      .base_sel_i(ab_base_sel),
      .infer_start_i (ab_infer_start),
      .base_i    (ab_base),
      .ra_req_i  (ab_req),
      .ra_buf_i  (ab_buf),
      .ra_off_i  (ab_off),
      .ra_oob_i  (ab_oob),
      .ra_gnt_o  (ab_gnt),
      .rdata_valid_o(ab_rvalid),
      .rdata_o   (ab_rdata),
      .we_i      (ab_we),
      .w_buf_i   (ab_wbuf),
      .w_off_i   (ab_woff),
      .wdata_i   (ab_wdata)
  );

  // ------------------------------------------------------------ weights
  logic              wm_req, wm_valid;
  logic [WBITS-1:0]  wm_off;
  logic [63:0]       wm_word;
  logic signed [7:0] wm_data [N_PE];

  // ecg_wmem emits ONE 64-bit word; ecg_seq receives EIGHT independent signed bytes.
  // The unpacking is performed here, with low byte mapped to lane 0 -- matching DMA
  // loading order and checked by tools/rtl_ref/wmem_ref.py.
  // 
  always_comb begin
    for (int unsigned i = 0; i < N_PE; i++) begin
      wm_data[i] = signed'(wm_word[8*i +: 8]);
    end
  end

  ecg_wmem #(.BYTES(ECG_WMEM_BYTES)) u_wm (
      .clk_i, .rst_ni,
      .dma_start_i, .dma_len_i, .dma_busy_o, .dma_done_o,
      .s_valid_i (ws_valid_i),
      .s_data_i  (ws_data_i),
      .s_ready_o (ws_ready_o),
      .rd_req_i  (wm_req),
      .rd_off_i  (wm_off),
      .rd_valid_o(wm_valid),
      .rd_data_o (wm_word)
  );

  // ------------------------------------------------------------ sequencer
  logic sq_start, sq_busy, sq_done;
  logic sv_start, sv_busy, sv_done;

  logic              sq_areq, sq_aoob;
  logic [3:0]        sq_abuf;
  logic [ABITS-1:0]  sq_aoff;
  logic              sq_wreq;
  logic [WBITS-1:0]  sq_woff;
  logic [13:0]       sq_soff;
  logic [8:0]        sq_boff;
  logic              sq_wr;
  logic [3:0]        sq_wbuf;
  logic [ABITS-1:0]  sq_woff_a;
  logic signed [7:0] sq_wdata;

  ecg_seq #(.N_PE(N_PE), .ABITS(ABITS), .WBITS(WBITS)) u_sq (
      .clk_i, .rst_ni,
      .start_i (sq_start),
      .busy_o  (sq_busy),
      .done_o  (sq_done),
      .op_i    (q_op),     .act_i    (q_act),
      .src0_i  (q_src0),   .dst_i    (q_dst),
      .dst_off_i(q_dst_off),
      .cin_i   (q_cin),    .cout_i   (q_cout),
      .len_in_i(q_len_in), .len_out_i(q_len_out),
      .k_i     (q_k),      .stride_i (q_stride),
      .pad_i   (q_pad),    .dw_i     (q_dw),
      .w_base_i(q_w_base), .rq_base_i(q_rq_base),
      .rq_n_i  (q_rq_n),
      .a_req_o (sq_areq),  .a_buf_o  (sq_abuf),
      .a_off_o (sq_aoff),  .a_oob_o  (sq_aoob),
      .a_gnt_i (ab_gnt),   .a_valid_i(ab_rvalid),
      .a_data_i(ab_rdata),
      .w_req_o (sq_wreq),  .w_off_o  (sq_woff),
      .w_valid_i(wm_valid), .w_data_i(wm_data),
      .s_off_o  (sq_soff),
      .b_off_o (sq_boff),
      .s_mult_i(s_mult_i), .s_shift_i(s_shift_i),
      .s_bias_i(s_bias_i),
      .wr_o    (sq_wr),    .wr_buf_o (sq_wbuf),
      .wr_off_o(sq_woff_a), .wr_data_o(sq_wdata)
  );

  logic              sv_areq;
  logic [3:0]        sv_abuf;
  logic [ABITS-1:0]  sv_aoff;
  logic [13:0]       sv_soff;
  logic              sv_wr;
  logic [3:0]        sv_wbuf;
  logic [ABITS-1:0]  sv_woff;
  logic signed [7:0] sv_wdata;

  ecg_seq_vec #(.ABITS(ABITS)) u_sv (
      .clk_i, .rst_ni,
      .start_i (sv_start),
      .busy_o  (sv_busy),
      .done_o  (sv_done),
      .op_i    (q_op),     .act_i   (q_act),
      .src0_i  (q_src0),   .src1_i  (q_src1),
      .dst_i   (q_dst),    .dst_off_i(q_dst_off),
      .cout_i  (q_cout),
      .len_in_i(q_len_in), .len_out_i(q_len_out),
      .k_i     (q_k),      .stride_i(q_stride),
      .pad_i   (q_pad),    .rq_base_i(q_rq_base),
      .a_req_o (sv_areq),  .a_buf_o (sv_abuf),
      .a_off_o (sv_aoff),
      .a_gnt_i (ab_gnt),   .a_valid_i(ab_rvalid),
      .a_data_i(ab_rdata),
      .s_off_o (sv_soff),
      .s_mult_i(s_mult_i), .s_shift_i(s_shift_i),
      .wr_o    (sv_wr),    .wr_buf_o(sv_wbuf),
      .wr_off_o(sv_woff),  .wr_data_o(sv_wdata)
  );

  // ------------------------------------------------------------------ mux
  // Only one sequencer runs at a time, so this is a mux, not an arbiter.
  logic use_mac;
  assign use_mac = q_is_mac;

  assign ab_req  = use_mac ? sq_areq : sv_areq;
  assign ab_buf  = use_mac ? sq_abuf : sv_abuf;
  assign ab_off  = use_mac ? sq_aoff : sv_aoff;
  assign ab_oob  = use_mac ? sq_aoob : 1'b0;

  assign wm_req  = use_mac && sq_wreq;
  assign wm_off  = sq_woff;

  assign s_off_o = use_mac ? sq_soff : sv_soff;
  assign b_off_o = bias_base_q + 10'({1'b0, sq_boff});

  // Write port: sequencer during execution, input loader when idle.
  logic seq_wr;
  assign seq_wr = use_mac ? sq_wr : sv_wr;

  assign ab_we    = (state_q == C_IDLE) ? pre_we_i : seq_wr;
  assign ab_wbuf  = (state_q == C_IDLE) ? pre_buf_i
                                        : (use_mac ? sq_wbuf : sv_wbuf);
  assign ab_woff  = (state_q == C_IDLE) ? pre_off_i
                                        : (use_mac ? sq_woff_a : sv_woff);
  assign ab_wdata = (state_q == C_IDLE) ? pre_data_i
                                        : (use_mac ? sq_wdata : sv_wdata);

  assign wr_o            = seq_wr && (state_q == C_RUN);
  assign wr_buf_o        = use_mac ? sq_wbuf : sv_wbuf;
  assign wr_off_o        = use_mac ? sq_woff_a : sv_woff;
  assign wr_data_o       = use_mac ? sq_wdata : sv_wdata;
  assign wr_last_layer_o = q_last;

  assign desc_req_o = (state_q == C_FETCH);
  assign desc_idx_o = layer_q;
  assign busy_o     = state_q != C_IDLE;

  // ------------------------------------------------------------------ FSM
  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
      state_q     <= C_IDLE;
      layer_q     <= '0;
      bias_base_q <= '0;
      done_o         <= 1'b0;
      desc_illegal_o <= 1'b0;
      sq_start    <= 1'b0;
      sv_start    <= 1'b0;
      ab_base_we  <= 1'b0;
      ab_base_sel <= '0;
      ab_base     <= '0;
      q_is_mac    <= 1'b0;
    end else begin
      done_o          <= 1'b0;
      desc_illegal_o  <= 1'b0;
      sq_start   <= 1'b0;
      sv_start   <= 1'b0;
      ab_base_we <= 1'b0;

      unique case (state_q)
        C_IDLE: if (start_i) begin
          layer_q     <= single_i ? layer_i : 6'd0;
          // Single-layer mode does not reset bias base: must accumulate across
          // instructions within same beat. Layer 0 resets, and shim always starts from 0.
          if (!single_i || (layer_i == 6'd0)) bias_base_q <= '0;
          // Base of ECG input channel: byte 0 by convention (see top comments).
          ab_base_we  <= 1'b1;
          ab_base_sel <= ECG_SRC_IN;
          ab_base     <= '0;
          state_q     <= C_BASE;
        end

        C_BASE: begin
          // Base of RR port: immediately following ECG window.
          ab_base_we  <= 1'b1;
          ab_base_sel <= ECG_SRC_RR;
          ab_base     <= ABITS'(in_len_i);
          state_q     <= C_FETCH;
        end

        C_FETCH: if (desc_valid_i) begin
          // DESC-04: invalid descriptor -> no write/start, finite error code,
          // and no hang (formal assertion at :549 requires return to C_DONE).
          if (!d_legal) begin
            // REJECT, do not FETCH. The four conditions in `legal_o` are guaranteed
            // by compiler (ecg_desc.sv "Validity"), so violation indicates CORRUPT BLOB
            // or BIT-ALIGNED DECODE MISMATCH. Continuing computation produces valid-looking
            // but INCORRECT results -- a symptom detached from root cause.
            // 
            //
            // Previously `d_legal` only fed an `$error` under `ifndef SYNTHESIS`
            // (line ~444), so on SILICON an invalid descriptor STILL executed.
            // External audit caught this.
            //
            // Abort entire beat rather than skipping a layer: skipping yields "near-correct"
            // results indistinguishable from valid execution.
            desc_illegal_o <= 1'b1;
            state_q        <= C_DONE;
          end else begin
          q_op       <= (!EN_MAXPOOL && (d_op == 5'(ECG_MAXPOOL))) ? 5'(ECG_GAP)
                      : (!EN_ADD     && (d_op == 5'(ECG_ADD)))     ? 5'(ECG_GAP)
                      : d_op;
          q_act      <= d_act;
          q_src0     <= d_src0;     q_src1    <= d_src1;
          q_dst      <= d_dst;      q_dst_off <= d_dst_off;
          q_cin      <= d_cin;      q_cout    <= d_cout;
          q_len_in   <= d_len_in;   q_len_out <= d_len_out;
          q_k        <= d_k;        q_stride  <= d_stride;
          q_pad      <= d_pad;
          // Feature switches take effect EXCLUSIVELY here: force fields to constant.
          q_dw       <= EN_DW ? d_dw : 1'b0;
          q_w_base   <= d_w_base;   q_rq_base <= d_rq_base;
          q_rq_n     <= EN_PERCH ? d_rq_n
                       : ((d_rq_n > 9'd1) ? 9'd1 : d_rq_n);
          q_last     <= d_last;
          q_is_mac   <= d_is_mac;
          // DESTINATION buffer base, written to base table now: current layer writes to it,
          // and subsequent layers reading it will use this exact base.
          ab_base_we  <= 1'b1;
          ab_base_sel <= d_dst;
          ab_base     <= ABITS'(d_buf_base);
          state_q     <= C_LAUNCH;
          end
        end

        C_LAUNCH: begin
          if (q_is_mac) sq_start <= 1'b1;
          else          sv_start <= 1'b1;
          state_q <= C_RUN;
        end

        C_RUN: if (q_is_mac ? sq_done : sv_done) state_q <= C_NEXT;

        C_NEXT: begin
          // Advance bias base AFTER layer completes: during execution it must remain
          // the base for current layer.
          if (q_is_mac) bias_base_q <= bias_base_q + 10'({1'b0, q_cout});
          if (single_i || q_last || (layer_q + 6'd1 >= n_layers_i)) begin
            state_q <= C_DONE;
          end else begin
            layer_q <= layer_q + 6'd1;
            state_q <= C_FETCH;
          end
        end

        C_DONE: begin
          done_o  <= 1'b1;
          state_q <= C_IDLE;
        end

        default: state_q <= C_IDLE;
      endcase
    end
  end

`ifndef SYNTHESIS
  always_ff @(posedge clk_i or negedge rst_ni) begin
    // This `$error` BLOCKS tests intentionally loading an invalid descriptor, so REJECTION
    // at C_FETCH cannot be checked by directed tests unless disabled with `+ecg_cho_desc_sai`.
    // NOT disabled by default: an invalid descriptor during REAL execution must stop simulation.
    // 
    if (rst_ni && (state_q == C_FETCH) && desc_valid_i && !d_legal
        && !$test$plusargs("ecg_cho_desc_sai"))
      $error("ecg_coproc: descriptor lop %0d khong hop le", layer_q);

    // DESC-01, BIAS REGION. The other 4 regions (src/dst/weight/scale) are
    // WIDENED to 32 bits BEFORE multiplying and comparing with capacity in
    // `ecg_desc.legal_o`. The BIAS region is not.
    // 
    // OVERFLOW LIES IN ACCUMULATOR, not in address addition. Initially placed
    // assertion on `bias_base_q + sq_boff` (:336) and it NEVER fired --
    // because `bias_base_q` is a 10-BIT REGISTER, truncating at :458 before
    // line :336 ever evaluates. Confirmed: a 20-layer model with cout 64 (sum 1280)
    // ran completely while assertion stayed silent. Initial check had WRONG EXPRESSION.
    // 
    // `legal_o` bounds `cout` of EACH LAYER (via `dst_fit`) but NOT cumulative
    // cout across layers, so a per-layer valid model can overflow 1023 and silently
    // TRUNCATE: an OUT-OF-BOUNDS index wraps into an IN-BOUNDS index. `ecg_mmio.b_oob`
    // is blind to this -- it detects indices exceeding table, not wrapped within.
    // 
    // 
    // Assertion lives in `ifndef SYNTHESIS`, adding zero logic: area unchanged.
    // It guards DESCRIPTOR GENERATION PATH (bad model caught in simulation), NOT
    // hardware -- on silicon truncation remains silent. Hardware guard (`bias_fit` in
    // `legal_o`) is a separate architectural decision requiring an ADR (see DESC-01).
    // 
    if (rst_ni && (state_q == C_NEXT) && q_is_mac
        && ((32'(bias_base_q) + 32'({23'b0, q_cout})) > 32'd1023))
      $error("ecg_coproc: bo cong don bias %0d + cout %0d = %0d TRAN 10 bit -- gia tri cat con %0d, mot chi so NGOAI bang thanh mot chi so TRONG bang",
             bias_base_q, q_cout,
             32'(bias_base_q) + 32'({23'b0, q_cout}),
             (32'(bias_base_q) + 32'({23'b0, q_cout})) & 32'd1023);

    // STORE or CONCAT layer must not be dispatched: CONCAT has no record
    // (ADR-0014 §2.1) and STORE is a control instruction, not a layer.
    if (rst_ni && (state_q == C_FETCH) && desc_valid_i
        && !(d_is_mac || d_is_pool || d_is_gap || d_is_add))
      $error("ecg_coproc: op %0d o lop %0d khong co sequencer nao nhan",
             d_op, layer_q);
    if (rst_ni && (state_q != C_IDLE) && pre_we_i)
      $error("ecg_coproc: nap dau vao trong luc dang chay");
    // Two sequencers share read and write ports via MUX, not arbiter;
    // valid only if both never run concurrently.
    if (rst_ni && sq_busy && sv_busy)
      $error("ecg_coproc: ca hai sequencer cung chay -- mux khong con dung");

    // CROSS-CHECK between inference paths. Sequencers do not read decode signals
    // directly -- they deduce from q_op / q_rq_n / q_act. Two redundant deduction
    // paths can diverge, so cross-verify here rather than leaving signals unused.
    // 
    if (rst_ni && (state_q == C_FETCH) && desc_valid_i) begin
      // STORE is a control instruction, not a layer in descriptor table.
      if (d_is_store)
        $error("ecg_coproc: STORE xuat hien nhu mot lop o %0d", layer_q);
      // src1 is meaningful only for ADD (and CONCAT, which has no descriptor). Elsewhere
      // it must be ECG_SRC_NONE = 13, not 0: 0 is a REAL buffer (buffer index 0),
      // so compiler uses dedicated sentinel for unused -- initial assert compared to 0 and failed.
      // 
      if (!d_uses_src1 && (d_src1 != ECG_SRC_NONE))
        $error("ecg_coproc: lop %0d khong dung src1 nhung src1 = %0d",
               layer_q, d_src1);
      // Per-channel requires rq_n == cout -- but ONLY for multiplying layers
      // and GAP. `per_ch` is rq_n > 1, and ADD has rq_n = 2 (one scale per operand),
      // satisfying "> 1" without being per-channel. Initial assert caught m2 residual layer.
      // 
      if (d_per_ch && (d_is_mac || d_is_gap) && (d_rq_n != d_cout))
        $error("ecg_coproc: lop %0d per-channel nhung rq_n = %0d != cout = %0d",
               layer_q, d_rq_n, d_cout);
      // Decoded ReLU must match act field read by sequencer.
      if (d_relu != (d_act == 2'(ECG_ACT_RELU)))
        $error("ecg_coproc: lop %0d relu = %0d nhung act = %0d",
               layer_q, d_relu, d_act);
      // Record writing to non-zero channel offset is CONCAT branch, requiring cat_part
      // flag -- otherwise memory allocator would have treated it as separate buffer.
      if ((d_dst_off != 10'd0) && !d_cat_part)
        $error("ecg_coproc: lop %0d co dst_off = %0d nhung cat_part = 0",
               layer_q, d_dst_off);
    end
  end

  // MODULE-SCOPE REJECTION CHECK (cannot live inside `always` block per IEEE 1800
  // 16.14.6, as Verilator enforces): invalid descriptor MUST steer state machine
  // to C_DONE, never C_LAUNCH.
  //
  // SCOPE LIMITATION: concurrent assertion checked in EVERY simulation run, BUT
  // only triggered when condition occurs; currently no directed test injects invalid
  // descriptor. Hence it remains an UNEXERCISED assertion, not counted toward coverage.
  // `+ecg_cho_desc_sai` above enables directed tests to exercise this path.
  // 
  // `lint_off SYNCASYNCNET` applies SOLELY around this assertion: FSM uses
  // ASYNCHRONOUS `rst_ni` (`always_ff @(... or negedge rst_ni)`), while
  // `disable iff (!rst_ni)` evaluates synchronously with clk. Verilator reports this.
  // As this is an assertion rather than synthesizable logic, difference creates no
  // second reset tree -- but scoping rule disables warning strictly locally with rationale.
  // 
  /* verilator lint_off SYNCASYNCNET */
  assert property (@(posedge clk_i) disable iff (!rst_ni)
    ((state_q == C_FETCH) && desc_valid_i && !d_legal) |=> (state_q == C_DONE))
    else $error("ecg_coproc: descriptor sai ma may KHONG ve C_DONE");
  /* verilator lint_on SYNCASYNCNET */

`endif

endmodule

`endif  // ECG_COPROC_SV

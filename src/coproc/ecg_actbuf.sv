// Activation buffer: 5 kB of int8, thirteen logical buffers plus two input
// ports, one physical port pair, and the boundary zero injection of ADR-0013.
//
// Two design decisions live here and both come from a threshold, not taste.
//
// N7 (<= 24 kB on chip). The store is 5120 B and nothing else. Byte offsets for
// the thirteen logical buffers are assigned by the compiler with liveness, so
// two buffers that are never live together share bytes; a static per-buffer
// range would need 5,056 B for Inception against a 4,096 B budget. The base of
// each logical buffer arrives in the layer descriptor and is held in a small
// register file here, not in SRAM, so it costs flops rather than budget.
//
// N9 (>= 90 % logic shared across the four families). The audit passes by 0.1
// points, and the thinnest item is the 65 gates that only ResNet needs: a second
// read for the ADD skip path. Adding a second physical read port would move
// those gates into the memory itself and put N9 below threshold. So this module
// exposes two read *requests* and time-multiplexes them onto one port: request A
// wins on even grants, request B on odd. ADD and the multi-branch CONCAT
// therefore issue at half rate, which is affordable because between them they
// account for four layers out of fifty, and the cycle budget has 4.6x headroom.
//
// Zero injection: a read marked out of bounds returns zero without touching the
// memory. That is what makes a model switch cost 98 cycles instead of 610, and
// the invariant it rests on -- every in-bounds address was written earlier in the
// same inference -- is asserted below rather than assumed.

`ifndef ECG_ACTBUF_SV
`define ECG_ACTBUF_SV

module ecg_actbuf
  import ecg_pkg::*;
#(
    parameter int unsigned BYTES = ECG_ACT_BYTES,
    localparam int unsigned ADDR_W = $clog2(BYTES)
) (
    input  logic                     clk_i,
    input  logic                     rst_ni,

    // Base-address register file, written when a layer is launched.
    input  logic                     base_we_i,
    input  logic [3:0]               base_sel_i,
    input  logic [ADDR_W-1:0]        base_i,

    // Read: single read request.
    input  logic                     ra_req_i,
    input  logic [3:0]               ra_buf_i,
    input  logic [ADDR_W-1:0]        ra_off_i,
    input  logic                     ra_oob_i,     // ADR-0013 zero injection
    output logic                     ra_gnt_o,

    // One int8 per granted read, one cycle later.
    output logic                     rdata_valid_o,
    output logic signed [7:0]        rdata_o,

    input  logic                     we_i,
    input  logic [3:0]               w_buf_i,
    input  logic [ADDR_W-1:0]        w_off_i,
    input  logic signed [7:0]        wdata_i,
    // Single cycle pulse when an inference beat starts (SIMULATION only).
    // See written/wepoch block below and ADR-0013 §3.
    input  logic                     infer_start_i
);

  // ---------------------------------------------------------------- bases
  // Packed rather than unpacked so the reset is one assignment: Verilator
  // rejects a delayed assignment to an unpacked array inside a for loop.
  // ECG_N_BASE = 16, not ECG_N_BUFFER = 13: two input ports (ECG_SRC_IN = 14,
  // ECG_SRC_RR = 15) reside in same physical buffer so need separate bases. With
  // 13 bases, base_of returns 0 for both and both read from byte 0.
  logic [ECG_N_BASE-1:0][ADDR_W-1:0] base_q;

  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
      base_q <= '0;
    end else if (base_we_i) begin
      base_q[base_sel_i] <= base_i;
    end
  end

  function automatic logic [ADDR_W-1:0] base_of(logic [3:0] sel);
    return base_q[sel];
  endfunction

  // ---------------------------------------------------------------- read
  // Single request, no arbitration: incoming request is granted immediately.
  assign ra_gnt_o = ra_req_i;

  logic [ADDR_W-1:0] raddr;
  logic              roob;
  always_comb begin
    raddr = base_of(ra_buf_i) + ra_off_i;
    roob  = ra_oob_i;
  end

  // A zero-injected read must not reach the memory: the whole point of ADR-0013
  // is that the boundary region is never fetched, so the address it would have
  // used is meaningless and may be out of range.
  logic mem_re;
  assign mem_re = ra_req_i && !roob;

  logic signed [7:0] mem_rdata;

  ecg_sram_1r1w #(.WIDTH(8), .DEPTH(BYTES)) u_mem (
      .clk_i,
      .re_i    (mem_re),
      .raddr_i (raddr),
      .rdata_o (mem_rdata),
      .we_i    (we_i),
      .waddr_i (base_of(w_buf_i) + w_off_i),
      .wdata_i (wdata_i)
  );

  logic zero_q, valid_q;

  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
      valid_q <= 1'b0;
      zero_q  <= 1'b0;
    end else begin
      valid_q <= ra_req_i;
      zero_q  <= roob;
    end
  end

  assign rdata_valid_o = valid_q;
  assign rdata_o       = zero_q ? 8'sd0 : mem_rdata;

`ifndef SYNTHESIS
  // Written-before-read, the invariant ADR-0013 §3 rests on. A read of an
  // in-bounds byte that no write has touched in this inference means activations
  // from the previously loaded model are being consumed, which biases the result
  // slightly, appears only after a model switch, and depends on which model ran
  // before -- so it is invisible to a single-model tolerance test.
  //
  // SCOPE, clarifying previous overstatements. written is only cleared on RESET,
  // so after FIRST beat it no longer proves "written in THIS BEAT" -- exactly
  // what the comment claimed. An external audit caught this.
  //
  // WHY NOT clear the entire bitmap when a beat starts: MMIO loader port
  // SHARES this write port, and firmware loads 256 samples + 3 RR features BEFORE
  // issuing start. Clearing on start would wipe bits of ALREADY-written data, and
  // trigger FALSE error reports on layer 0. That was a fix answering the wrong
  // question.
  //
  // Clear PER BUFFER just as that buffer is about to be written by a layer:
  // ecg_coproc asserts w_clr_i along with base write at C_FETCH, where base_sel_i = d_dst.
  // Input buffer and RR buffer are never d_dst of a layer, so their bits are
  // preserved as required.
  // written = ever written. wepoch = written in WHICH BEAT. Two tiers, two
  // distinct questions:
  //   !written[b]                  -> read byte NEVER written
  //   written[b] & wepoch != epoch -> read byte written in PREVIOUS beat
  // The second is what single-tier bitmap CANNOT report, and what actually happens
  // after a model switch.
  //
  // INPUT buffer (ECG_SRC_IN) and RR buffer (ECG_SRC_RR) are EXEMPT from beat checks,
  // which is mandatory, not arbitrary: MMIO loader port shares this write port,
  // and firmware loads 256 samples + 3 RR features BEFORE issuing
  // start. Thus they carry PREVIOUS beat epoch by design. Tier-one check
  // (written) remains active.
  logic [BYTES-1:0] written;
  logic [7:0]       wepoch [BYTES];
  logic [7:0]       epoch_q;
  logic             epoch_exempt;
  assign epoch_exempt = (ra_buf_i == ECG_SRC_IN) || (ra_buf_i == ECG_SRC_RR);

  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
      written <= '0;
      epoch_q <= 8'd0;
    end else begin
      if (infer_start_i) epoch_q <= epoch_q + 8'd1;
      if (we_i) begin
        written[base_of(w_buf_i) + w_off_i] <= 1'b1;
        wepoch[base_of(w_buf_i) + w_off_i]  <= epoch_q;
      end
      if (mem_re && !written[raddr]) begin
        $error("ecg_actbuf: read of unwritten byte %0d (ADR-0013 invariant)",
               raddr);
      end else if (mem_re && !epoch_exempt && (wepoch[raddr] != epoch_q)) begin
        // SystemVerilog does not concatenate adjacent strings like C -- must be single string.
        $error("ecg_actbuf: byte %0d doc o nhip %0d ma ghi o nhip %0d: hoat do cua mot nhip TRUOC (ADR-0013 muc 3)",
               raddr, epoch_q, wepoch[raddr]);
      end
    end
  end
`endif

`ifdef FORMAL
  logic f_started_q = 1'b0;
  always_ff @(posedge clk_i) f_started_q <= 1'b1;
  always_comb if (!f_started_q) assume (!rst_ni);

  logic f_req_q;
  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) f_req_q <= 1'b0;
    else f_req_q <= ra_req_i;
  end


  // Assumption: compiler guarantees base + offset within buffer. make isa
  // verifies this on real traces, and ecg_desc blocks out-of-bounds buf_base.
  always_comb begin
    assume ({1'b0, base_of(ra_buf_i)} + {1'b0, ra_off_i} < (ADDR_W+1)'(BYTES));
    assume ({1'b0, base_of(w_buf_i)} + {1'b0, w_off_i} < (ADDR_W+1)'(BYTES));
  end

  always_ff @(posedge clk_i) begin
    if (rst_ni && f_started_q) begin
      // 1. A ZERO-injected read never touches memory. This is module-level provable
      //    half of ADR-0013: boundary region is not fetched, so address is
      //    meaningless and may be out of bounds. The other half -- in-bound
      //    byte was written by prior layer -- requires full sequencer in proof
      //    scope and belongs to P6.
      assert (!(ra_req_i && ra_oob_i) || !mem_re);

      // 2. A zero-injected read returns EXACTLY 0, not residual data in
      //    read path.
      assert (!(rdata_valid_o && zero_q) || (rdata_o == 8'sd0));

      // 3. rdata_valid_o equals ra_req_i delayed by one cycle: read path does
      //    not spontaneously generate results or drop any result.
      assert (rdata_valid_o == f_req_q);

      // 4. Any request is granted IMMEDIATELY. Both sequencers rely on this
      //    to issue a read every cycle; if untrue, T_infer
      //    deviates silently from cycle model.
      assert (ra_gnt_o == ra_req_i);

      // 5. Read address is within memory when actually reading.
      assert (!mem_re || ({1'b0, raddr} < (ADDR_W+1)'(BYTES)));
    end
  end

`endif

endmodule

`endif  // ECG_ACTBUF_SV

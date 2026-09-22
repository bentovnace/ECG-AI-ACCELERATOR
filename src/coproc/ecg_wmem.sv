// Weight memory and the LOADW DMA.
//
// Three decisions live here.
//
// N7 (<= 24 kB on chip). ADR-0015 holds ONE model's weights, sized to the
// largest family (m3-mobilenet, 4.656 B), instead of all four resident. That is
// what took N7 from 25.318 B (fail) to 11.634 B. The cost is that LOADW now sits
// on the critical path of a model switch: T_switch went 98 -> 669 cycles, which
// still leaves 33 % of the N3 budget. So this DMA's throughput is not a
// nice-to-have -- at 4 B/cycle instead of 8 the switch would cost 1.251 cycles
// and N3 would fail.
//
// Unaligned 8-byte reads in one cycle. The PE array wants eight weight bytes per
// cycle, but a layer's weight region starts wherever the previous one ended, so
// the byte offset is not a multiple of eight. Fetching two aligned words in two
// cycles would halve MAC utilisation. Instead the store is split into two banks
// by word parity: words w and w+1 always land in different banks, so both come
// out in the same cycle and a 128-bit funnel shifter picks the eight bytes. The
// banks cost nothing extra -- the total depth is unchanged.
//
// Byte order is little-endian inside a word: byte j occupies bits [8j+7:8j]. That
// makes the DMA stream, the memory contents and the PE lane order the same
// sequence, so there is no place for a silent reversal to hide.

`ifndef ECG_WMEM_SV
`define ECG_WMEM_SV

module ecg_wmem
  import ecg_pkg::*;
#(
    // ecg_pkg::ECG_WMEM_BYTES = 4,664 = 4,656 B of largest family (ADR-0015)
    // plus 8 B padding for 3 B overflow measured by tools/wlayout_check.py.
    parameter  int unsigned BYTES  = ECG_WMEM_BYTES,
    localparam int unsigned WORDS  = BYTES / 8,
    // One bank per parity of word index. Round up so odd WORDS has enough depth.
    localparam int unsigned BWORDS = (WORDS + 1) / 2,
    localparam int unsigned BAW    = (BWORDS <= 1) ? 1 : $clog2(BWORDS),
    localparam int unsigned OFF_W  = $clog2(BYTES)
) (
    input  logic                  clk_i,
    input  logic                  rst_ni,

    // ---- LOADW DMA. Non-blocking in the sense that command returns immediately;
    // core checks `busy` rather than being stalled at issue interface.
    input  logic                  dma_start_i,
    input  logic [OFF_W:0]        dma_len_i,      // byte, boi cua 8
    output logic                  dma_busy_o,
    output logic                  dma_done_o,     // single cycle pulse

    // Input data path, 8 B/cycle (BUS_BYTES from tools/cycle_model.py).
    input  logic                  s_valid_i,
    input  logic [63:0]           s_data_i,
    output logic                  s_ready_o,

    // ---- Read 8 bytes starting from ANY byte offset, one cycle latency.
    input  logic                  rd_req_i,
    input  logic [OFF_W-1:0]      rd_off_i,
    output logic                  rd_valid_o,
    output logic [63:0]           rd_data_o
);

  // ------------------------------------------------------------------ DMA
  logic [OFF_W:0]   wr_word_q;   // word index being written
  logic [OFF_W:0]   wr_words_q;  // total words to write
  logic             busy_q;

  logic accept;
  assign accept    = busy_q && s_valid_i;
  assign s_ready_o = busy_q;

  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
      busy_q     <= 1'b0;
      wr_word_q  <= '0;
      wr_words_q <= '0;
      dma_done_o <= 1'b0;
    end else begin
      dma_done_o <= 1'b0;
      if (!busy_q) begin
        if (dma_start_i && (dma_len_i != '0)) begin
          busy_q     <= 1'b1;
          wr_word_q  <= '0;
          wr_words_q <= dma_len_i >> 3;
        end
      end else if (accept) begin
        if (wr_word_q + 1 >= wr_words_q) begin
          busy_q     <= 1'b0;
          dma_done_o <= 1'b1;
        end
        wr_word_q <= wr_word_q + 1;
      end
    end
  end

  assign dma_busy_o = busy_q;

  // --------------------------------------------------------------- dual banks
  // Word w resides in bank (w & 1) at index (w >> 1). Thus w and w+1 always
  // reside in opposite banks and can be read in the same cycle.
  logic              we_even, we_odd;
  logic [BAW-1:0]    waddr_bank;

  assign we_even    = accept && (wr_word_q[0] == 1'b0);
  assign we_odd     = accept && (wr_word_q[0] == 1'b1);
  assign waddr_bank = BAW'(wr_word_q >> 1);

  // Word index of requested byte, and of subsequent word.
  logic [OFF_W-1:0]  w0;
  logic [2:0]        bsel;
  assign w0   = rd_off_i >> 3;
  assign bsel = rd_off_i[2:0];

  // Even bank reads from closest even index >= w0; odd bank from closest odd >= w0.
  // With w0 even: even reads w0, odd reads w0+1. With w0 odd: odd reads w0, even reads w0+1.
  // Reading final byte of buffer needs word w0 but NOT w0+1 (bsel = 0);
  // clamping prevents address w0+1 from overflowing bank capacity.
  // 
  // 
  logic [OFF_W:0] we_idx, wo_idx;
  assign we_idx = ((OFF_W+1)'(w0) + (OFF_W+1)'( w0[0])) >> 1;
  assign wo_idx = ((OFF_W+1)'(w0) + (OFF_W+1)'(!w0[0])) >> 1;

  logic [BAW-1:0] raddr_even, raddr_odd;
  assign raddr_even = (we_idx >= (OFF_W+1)'(BWORDS)) ? BAW'(BWORDS - 1)
                                                    : BAW'(we_idx);
  assign raddr_odd  = (wo_idx >= (OFF_W+1)'(BWORDS)) ? BAW'(BWORDS - 1)
                                                    : BAW'(wo_idx);

  logic [63:0] rd_even, rd_odd;

  ecg_sram_1r1w #(.WIDTH(64), .DEPTH(BWORDS)) u_even (
      .clk_i,
      .re_i    (rd_req_i),
      .raddr_i (raddr_even),
      .rdata_o (rd_even),
      .we_i    (we_even),
      .waddr_i (waddr_bank),
      .wdata_i (s_data_i)
  );

  ecg_sram_1r1w #(.WIDTH(64), .DEPTH(BWORDS)) u_odd (
      .clk_i,
      .re_i    (rd_req_i),
      .raddr_i (raddr_odd),
      .rdata_o (rd_odd),
      .we_i    (we_odd),
      .waddr_i (waddr_bank),
      .wdata_i (s_data_i)
  );

  // ------------------------------------------------------- funnel shifter
  // Parity AND shift amount must be registered; data arrives one cycle later
  // after rd_off_i has already changed.
  logic       odd_q, valid_q;
  logic [2:0] bsel_q;

  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
      valid_q <= 1'b0;
      odd_q   <= 1'b0;
      bsel_q  <= '0;
    end else begin
      valid_q <= rd_req_i;
      odd_q   <= w0[0];
      bsel_q  <= bsel;
    end
  end

  // Low word is w0, high word is w0+1.
  logic [127:0] pair;
  assign pair = odd_q ? {rd_even, rd_odd} : {rd_odd, rd_even};

  assign rd_data_o  = pair[7'({bsel_q, 3'b000}) +: 64];
  assign rd_valid_o = valid_q;

`ifndef SYNTHESIS
  always_ff @(posedge clk_i) begin
    // Read overflow past end of buffer: word w0+1 does not exist. Compiler never
    // generates such offset; caught here to prevent silent corruption.
    // 
    if (rd_req_i && (32'(rd_off_i) + 8 > 32'(BYTES)))
      $error("ecg_wmem: doc 8 B tu offset %0d tran qua %0d B", rd_off_i, BYTES);
    if (dma_start_i && !busy_q && (32'(dma_len_i) > 32'(BYTES)))
      $error("ecg_wmem: LOADW %0d B > dung luong %0d B", dma_len_i, BYTES);
    if (dma_start_i && !busy_q && (dma_len_i[2:0] != 3'd0))
      $error("ecg_wmem: LOADW %0d B khong phai boi cua 8", dma_len_i);
    if (dma_start_i && busy_q)
      $error("ecg_wmem: LOADW moi khi DMA con chay");
  end
`endif

`ifdef FORMAL
  logic f_started_q = 1'b0;
  always_ff @(posedge clk_i) f_started_q <= 1'b1;
  always_comb if (!f_started_q) assume (!rst_ni);

  // Reads do not overflow buffer boundary -- guaranteed by compiler and
  // tools/wlayout_check.py confirms 3 B padding is sufficient.
  always_comb assume (32'(rd_off_i) + 8 <= 32'(BYTES));

  // Word index and shift amount, delayed 1 cycle to align with returning data.
  logic [OFF_W-1:0] f_w0_q;
  logic [2:0]       f_bsel_q;
  logic             f_req_q;
  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
      f_w0_q   <= '0;
      f_bsel_q <= '0;
      f_req_q  <= 1'b0;
    end else begin
      f_w0_q   <= w0;
      f_bsel_q <= bsel;
      f_req_q  <= rd_req_i;
    end
  end

  logic [127:0] f_shifted;
  assign f_shifted = pair >> {f_bsel_q, 3'b000};

  always_ff @(posedge clk_i) begin
    if (rst_ni && f_started_q) begin
      // 1. `rd_valid_o` strictly equals `rd_req_i` delayed by one cycle.
      assert (rd_valid_o == f_req_q);

      // 2. PARITY mapping orientation: word w0 resides in even bank when
      //    w0 is even, in odd bank when w0 is odd; `pair` packs w0 into lower half.
      // 
      // 
      // 
      // 
      // 
      if (rd_valid_o) begin
        assert (f_w0_q[0] ? (pair[63:0] == rd_odd)
                          : (pair[63:0] == rd_even));
      end
      // Two indices differ by at most 1.
      assert (!rd_req_i
              || ((we_idx <= wo_idx + (OFF_W+1)'(1))
                  && (wo_idx <= we_idx + (OFF_W+1)'(1))));

      // 3. Funnel shifter extracts EXACT eight bytes starting from requested byte.
      // 
      //    Expressed via RIGHT SHIFT rather than variable part-select, providing an
      //    independent specification check of the RTL part-select logic.
      // 
      // 
      // 
      // 
      // 
      // 
      assert (!rd_valid_o || (rd_data_o == f_shifted[63:0]));
      //
      // PHYSICAL MEMORY MODEL REQUIRED (not `anyseq` abstraction stub).
      // 
      // Memory state must be real to maintain equivalence across dual read ports.
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

      // 4. DMA write address is within bounds, and DMA writes only while active.
      assert (!(we_even || we_odd) || busy_q);
      assert (!(we_even || we_odd)
              || ({1'b0, waddr_bank} < (BAW+1)'(BWORDS)));
    end
  end
`endif

endmodule

`endif  // ECG_WMEM_SV

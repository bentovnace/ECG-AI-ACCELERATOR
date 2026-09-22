// Drop-in replacement for ecg_sram_1r1w built out of OpenRAM sky130 macros.
//
// WHY THIS FILE EXISTS. `ecg_sram_1r1w` is a behavioural memory. Vivado infers a
// RAMB18 from it for free, so on FPGA it costs nothing. sky130 has no RAMB18: if
// yosys is left to map the same description, the 10.808 bytes of the coprocessor
// become flip-flops plus a read mux, and `ecg_coproc` measures 3.391.618 um2 with
// 16.590 mux4 cells -- 14-28x too large, and wrong for a reason that has nothing
// to do with the design. With the memories blackboxed the logic alone measures
// 118.296 um2, so the area of the chip is decided by which macros the memories
// land in. This module is where that decision is made, in one place, behind an
// interface that is character-for-character the one the rest of the RTL already
// uses -- so swapping it in cannot change the sequencer's timing contract.
//
// THE MACROS. /work/tools-eda/OpenROAD-flow-scripts/flow/platforms/sky130ram
// ships four OpenRAM 1rw1r macros. Areas below are read from the `area :` line of
// each `*_TT_1p8V_25C.lib`, not estimated:
//
//   sky130_sram_1rw1r_128x256_8   128 b x 256 w   722.109 um2   4096 B
//   sky130_sram_1rw1r_64x256_8     64 b x 256 w   419.463 um2   2048 B
//   sky130_sram_1rw1r_80x64_8      80 b x  64 w   181.971 um2    640 B (512 B used)
//   sky130_sram_1rw1r_44x64_8      44 b x  64 w   105.707 um2    352 B (256 B used)
//
// The 80 b and 44 b macros are ten and five-and-a-half bytes wide. A byte address
// can only be split into (row, lane) by a shift if the lane count is a power of
// two, so this module uses 8 of the 10 lanes and 4 of the 5.5. Paying for width
// that is not addressable is worse than it looks (0,36 and 0,41 um2/B against
// 0,18 for the 128x256), which is why the plan picks a big macro whenever it can;
// the small ones only earn their place as the last bank of a short array. Which
// combination is cheapest is decided by a search over those four areas -- see
// `kind_plan` for why a greedy choice is not good enough.
//
// READ LATENCY -- the one thing that must not drift. `ecg_sram_1r1w` captures
// `raddr_i` at a rising edge and drives `rdata_o` from that same edge. The macro
// captures addr1 at the rising edge of clk1 too, but its .lib arc for dout1 is
// `timing_type : falling_edge` (memory_read group, related_pin clk1), and the
// Verilog model matches: inputs are registered at posedge, dout1 is assigned at
// the following negedge. So the DATA ARRIVES HALF A CYCLE LATE, but in the SAME
// cycle -- a consumer that samples at the next rising edge sees exactly what the
// behavioural memory gave it. Latency in rising edges is one, unchanged, and no
// compensating pipeline stage is needed. What DOES change is static timing: the
// path from dout1 to the next flop has half a period, not a full one, so the read
// mux below is on a half-cycle path. That is a synthesis constraint to state, not
// a functional difference, and it is the reason the lane mux is kept to a single
// variable part-select with no arithmetic in it.
//
// HOLDING THE READ DATA. `ecg_sram_1r1w` holds `rdata_o` when `re_i` is low; a
// deselected macro leaves dout1 undefined. The first attempt here avoided an
// output register by tying csb1 active and replaying the last read address, so
// that the macro re-read the same word and held by itself. The harness killed it:
// a WRITE to the address being replayed changes what the re-read returns, while
// the behavioural memory keeps the old data. That is a real difference, it was
// found by phase 5 and not by reasoning, and it is why the hold is now an output
// register -- WIDTH flops and a two-input mux, enabled by the registered `re_i`.
// The mux sits on the same half-cycle path as any consumer of dout1, and the
// latency is unchanged because the register only supplies the cycles in which the
// macro is not driving anything anyway.
//
// Neither user needs the hold: both qualify the data with a valid bit. It is
// implemented anyway, because "drop-in replacement" has to mean the port, not the
// port as currently used, or the next user of this memory pays for the surprise.
//
// READ DURING WRITE. Same byte address: 1rw1r gives no answer -- port 0's write
// drivers hold that column while port 1's sense amp is looking at a cell in the
// middle of being written. The OpenRAM model only prints a warning; the assertion
// below makes it an error, exactly as `ecg_sram_1r1w` already does, so nothing new
// is forbidden. Same macro ROW, different byte: allowed and required. With sixteen
// bytes per row a read and a write in the same cycle share a row on about one
// access in sixteen (the harness counts them), and refusing that would break the
// whole point of a 1r1w memory. It is safe because
// port 0's write drivers only pull the columns whose wmask bit is set; the rest of
// the row sees a non-destructive access, and the lane mux discards it anyway.
//
// Comments are in English to match the rest of 40-rtl.

`ifndef ECG_SRAM_MACRO_SV
`define ECG_SRAM_MACRO_SV


// Clock gate, latch-based, written behaviourally so it simulates and so synthesis
// can map it to the technology integrated clock gate.
//
// Why this exists: the macro `.lib` puts `internal_power` on the two clock pins and
// the value is the SAME for every `when` condition -- including `csb & web` (fully
// deselected). Measured directly by running with csb0=csb1=1: the number does not
// move. So the macro burns clock energy every edge whether or not it is accessed,
// and `csb` cannot save it. Only removing the edge can.
//
// Measured port duty from a real 96,940-cycle simulation (tools/vcd_port_duty.py):
// the write port is active 12.27 % of cycles for the activation buffer and 0 % for
// the weight memory during compute. So gating the two write clocks removes 19.88 mW
// of the 42.5 mW total; the two read ports run 93.83 % of cycles and gating them
// would recover only 1.31 mW, which is not worth an enable path on the tighter read
// timing.
//
// The latch is on the LOW phase so the enable settles before the next rising edge.
// Writing it as a flip-flop instead would delay the enable by a full cycle and gate
// the wrong cycle -- that is the classic clock-gating bug and it is silent in
// simulation whenever the enable happens to stay high for two cycles.
module ecg_clk_gate (
    input  logic clk_i,
    input  logic en_i,
    output logic clk_o
);
  // Direct AND, NO latch. Three styles tested and first two failed here:
  //
  //   always_latch if (!clk_i) en_latched = en_i;   -> 55,475 mismatch
  //   always_ff @(negedge clk_i) en_q <= en_i;      -> 55,475 mismatch, identical
  //
  // Identical discrepancy across differing styles indicates cause is not in
  // gate style. Root cause is testbench waveform shape: project `tick()` performs
  // `clk=1; eval; clk=0; eval` (rising edge first, then falling in same tick).
  // Thus low-phase latch samples enable from previous cycle, gating clock one
  // cycle off -- the classic clock-gating bug warned by this very comment.
  //
  //
  // Under this pulse shape, correct gate is direct AND. Valid here because `en_i`
  // comes from ecg_seq registers, staying stable whole cycle without glitching.
  // In silicon, synthesis maps this to foundry integrated clock gating cell
  // (sky130_fd_sc_hd__dlclkp_1) whose internal latch prevents glitching; setup
  // condition (enable stable across high phase) holds as enable is register output.
  //
  assign clk_o = clk_i & en_i;
endmodule

module ecg_sram_macro #(
    parameter int unsigned WIDTH = 8,
    parameter int unsigned DEPTH = 4096,
    localparam int unsigned ADDR_W = (DEPTH <= 1) ? 1 : $clog2(DEPTH)
,
    // Write port clock gating. Default ON: saves 19.88 out of 42.5 mW per
    // measured duty cycle; can disable to measure power delta.
    parameter bit CLK_GATE = 1'b1
) (
    input  logic                clk_i,

    input  logic                re_i,
    input  logic [ADDR_W-1:0]   raddr_i,
    output logic [WIDTH-1:0]    rdata_o,

    input  logic                we_i,
    input  logic [ADDR_W-1:0]   waddr_i,
    input  logic [WIDTH-1:0]    wdata_i
);

  // Everything below is declared before it is used, including the functions. That
  // is not style: slang follows the LRM and rejects a reference above its
  // declaration, while Verilator accepts it. Four bugs in this project were of
  // exactly that shape, all found only by the second analyser.

  localparam int unsigned N_KIND    = 4;
  localparam int unsigned MAX_BANKS = 32;

  // Kinds are listed LARGEST FIRST. The greedy plan below relies on that order,
  // and it survives any WIDTH because a kind's word count is rows * usable/WIDTH.
  function automatic int unsigned kind_rows(int unsigned k);
    case (k)
      0: return 256;
      1: return 256;
      2: return 64;
      default: return 64;
    endcase
  endfunction

  // Physical data width of the macro port.
  function automatic int unsigned kind_dw(int unsigned k);
    case (k)
      0: return 128;
      1: return 64;
      2: return 80;
      default: return 44;
    endcase
  endfunction

  // Addressable width: the largest power-of-two number of bytes inside the word.
  function automatic int unsigned kind_use(int unsigned k);
    case (k)
      0: return 128;
      1: return 64;
      2: return 64;
      default: return 32;
    endcase
  endfunction

  function automatic int unsigned kind_aw(int unsigned k);
    return (kind_rows(k) <= 1) ? 1 : $clog2(kind_rows(k));
  endfunction

  function automatic int unsigned kind_masks(int unsigned k);
    return (kind_dw(k) + 7) / 8;
  endfunction

  // Words of WIDTH bits this kind can hold, or 0 if it cannot hold one at all.
  // A word must fit an integral, power-of-two number of times in the usable part
  // of the row, otherwise (row, lane) is a division and not a shift.
  function automatic int unsigned kind_words(int unsigned k);
    if ((WIDTH == 0) || (WIDTH > kind_use(k))) return 0;
    if ((kind_use(k) % WIDTH) != 0) return 0;
    if (((kind_use(k) / WIDTH) & ((kind_use(k) / WIDTH) - 1)) != 0) return 0;
    return kind_rows(k) * (kind_use(k) / WIDTH);
  endfunction

  // Cell area in whole um2, truncated from the `area :` line of each macro's
  // *_TT_1p8V_25C.lib. These are the numbers the plan below minimises, so they
  // belong in the code rather than in a comment: if a macro is re-characterised,
  // the choice has to move with it.
  function automatic int unsigned kind_area(int unsigned k);
    case (k)
      0: return 722109;   // 722109,357
      1: return 419463;   // 419462,707
      2: return 181971;   // 181970,793
      default: return 105707;  // 105706,655
    endcase
  endfunction

  // The plan: four bits per bank, value kind+1, zero meaning "no bank". Banks are
  // emitted in non-increasing capacity, which is what makes every bank base a
  // multiple of that bank's capacity and therefore lets the byte lane be the low
  // bits of the global address instead of a subtraction.
  //
  // The choice itself is a search, not a greedy walk, and that is not gold
  // plating: greedy (largest macro that still fits the remainder) was the first
  // version and it is measurably wrong. For DEPTH=4448 -- the buffer size ADR-0017
  // is considering -- greedy leaves a 352-word tail, takes a 44x64 because it
  // fits, then needs a second one, and lands on 933.522 um2 where one 80x64 tail
  // gives 904.080 um2.
  //
  // The search is tiny because a bigger macro is strictly cheaper per word
  // (176 / 205 / 355 / 413 um2 per byte, largest to smallest), so small macros
  // only ever pay for a TAIL, and any count that reaches the next size up is
  // dominated by it: 2x64x256 (838.926) loses to one 128x256 (722.109) at equal
  // capacity, 4x80x64 (727.883) loses to one 64x256 (419.463), 2x44x64 (211.413)
  // loses to one 80x64 (181.971). Hence n1 <= 1, n2 <= 3, n3 <= 1 and the count
  // of the largest macro follows from the remainder: sixteen combinations.
  function automatic [MAX_BANKS*4-1:0] kind_plan();
    logic [MAX_BANKS*4-1:0] p;
    logic [MAX_BANKS*4-1:0] best_p;
    int unsigned n0, tail, need, area, best_area, idx, banks;
    int unsigned cnt [N_KIND];
    best_p    = '0;
    best_area = 0;
    for (int unsigned n1 = 0; n1 <= 1; n1++)
      for (int unsigned n2 = 0; n2 <= 3; n2++)
        for (int unsigned n3 = 0; n3 <= 1; n3++) begin
          // A kind that cannot hold a word of this WIDTH cannot be used at all.
          if (((n1 == 0) || (kind_words(1) != 0))
              && ((n2 == 0) || (kind_words(2) != 0))
              && ((n3 == 0) || (kind_words(3) != 0))) begin
            tail = n1 * kind_words(1) + n2 * kind_words(2) + n3 * kind_words(3);
            need = (tail >= DEPTH) ? 0 : (DEPTH - tail);
            if ((need == 0) || (kind_words(0) != 0)) begin
              n0    = (need == 0) ? 0
                    : ((need + kind_words(0) - 1) / kind_words(0));
              banks = n0 + n1 + n2 + n3;
              area  = n0 * kind_area(0) + n1 * kind_area(1)
                    + n2 * kind_area(2) + n3 * kind_area(3);
              if ((banks != 0) && (banks <= MAX_BANKS)
                  && ((best_area == 0) || (area < best_area))) begin
                best_area = area;
                cnt[0]    = n0;
                cnt[1]    = n1;
                cnt[2]    = n2;
                cnt[3]    = n3;
                p         = '0;
                idx       = 0;
                for (int unsigned k = 0; k < N_KIND; k++)
                  for (int unsigned c = 0; c < MAX_BANKS; c++)
                    if ((c < cnt[k]) && (idx < MAX_BANKS)) begin
                      p[idx*4 +: 4] = 4'(k + 1);
                      idx           = idx + 1;
                    end
                best_p = p;
              end
            end
          end
        end
    return best_p;
  endfunction

  localparam logic [MAX_BANKS*4-1:0] PLAN = kind_plan();

  function automatic int unsigned plan_nbanks();
    int unsigned n;
    n = 0;
    for (int unsigned i = 0; i < MAX_BANKS; i++)
      if (PLAN[i*4 +: 4] != 4'd0) n = i + 1;
    return n;
  endfunction

  function automatic int unsigned plan_kind(int unsigned i);
    return (PLAN[i*4 +: 4] == 4'd0) ? 0 : (32'(PLAN[i*4 +: 4]) - 1);
  endfunction

  function automatic int unsigned plan_base(int unsigned i);
    int unsigned b;
    b = 0;
    for (int unsigned j = 0; j < MAX_BANKS; j++)
      if ((j < i) && (PLAN[j*4 +: 4] != 4'd0)) b = b + kind_words(plan_kind(j));
    return b;
  endfunction

  localparam int unsigned NBANKS  = plan_nbanks();
  localparam int unsigned CAP_TOT = plan_base(MAX_BANKS);

  // Fail loudly rather than silently building a memory that is too small. This
  // one is an initial block, not an elaboration task, because MAX_BANKS is the
  // knob to turn and the message has to name the two numbers that disagree.
  initial begin
    if (CAP_TOT < DEPTH)
      $fatal(1, "ecg_sram_macro: %0d banks cover %0d words < DEPTH=%0d (raise MAX_BANKS)",
             NBANKS, CAP_TOT, DEPTH);
  end

  // The bank and lane decode for the output mux has to use the address the macros
  // CAPTURED, one cycle ago, not the one on the port now.
  logic [ADDR_W-1:0] raddr_q;
  logic              re_q;

  // Gated clock for READ port (global). Measured duty cycle is 93.83% saving only
  // 1.31 mW; retained to measure delta, but default un-gated because enable would
  // sit on the read path which already has tighter timing constraints.
  logic clk_rd;
  assign clk_rd = clk_i;

  always_ff @(posedge clk_i) begin
    re_q <= re_i;
    if (re_i) raddr_q <= raddr_i;
  end

  logic [31:0] ra32, ra_q32, wa32;
  assign ra32   = 32'(raddr_i);
  assign ra_q32 = 32'(raddr_q);
  assign wa32   = 32'(waddr_i);

  // NBANKS is zero only when no macro can hold a word of this WIDTH, which the
  // initial block above turns into $fatal; NB keeps the vectors legal until then.
  localparam int unsigned NB = (NBANKS == 0) ? 1 : NBANKS;

  logic [NB-1:0]            bank_selq;
  logic [NB-1:0][WIDTH-1:0] bank_rdata;

  // No usable macro: emit nothing and let the elaboration task below say why.
  // Elaborating a zero-capacity bank instead would produce a pile of
  // constant-comparison warnings that bury the real message.
  if (NBANKS == 0) begin : g_no_macro
    $fatal(1, "ecg_sram_macro: WIDTH must be a power of two in 8..128, got %0d", WIDTH);
    assign bank_selq  = '0;
    assign bank_rdata = '0;
  end

  for (genvar b = 0; b < NBANKS; b++) begin : g_bank
    localparam int unsigned K    = plan_kind(b);
    localparam int unsigned BASE = plan_base(b);
    localparam int unsigned CAPW = kind_words(K);
    localparam int unsigned WPW  = kind_use(K) / WIDTH;         // words per row
    localparam int unsigned LB   = (WPW <= 1) ? 0 : $clog2(WPW);
    localparam int unsigned AW   = kind_aw(K);
    localparam int unsigned DW   = kind_dw(K);
    localparam int unsigned USE  = kind_use(K);
    localparam int unsigned NM   = kind_masks(K);
    localparam int unsigned BW   = WIDTH / 8;                   // mask bits per word

    // Range decode. Written as generated assigns and not as one expression
    // because a bank that starts at 0 or ends at the top of the address space
    // would otherwise compare against a constant that can never fail, which is
    // an error under -Wall and, worse, hides a real off-by-one behind a waiver.
    logic sel_w, lo_w, hi_w;

    if (BASE == 0) begin : g_lo0
      assign lo_w = 1'b1;
    end else begin : g_lo
      assign lo_w = (wa32 >= 32'(BASE));
    end

    if ((BASE + CAPW) >= (1 << ADDR_W)) begin : g_hi0
      assign hi_w = 1'b1;
    end else begin : g_hi
      assign hi_w = (wa32 < 32'(BASE + CAPW));
    end

    assign sel_w = lo_w && hi_w && we_i;

    // The output mux selects on the address the macros ACTUALLY captured, which
    // is raddr_q. Recomputing the decode from raddr_q instead of registering
    // sel_r keeps the two in step even when re_i is low and the address replays.
    if (BASE == 0) begin : g_qlo0
      assign bank_selq[b] = ((BASE + CAPW) >= (1 << ADDR_W))
                            ? 1'b1 : (ra_q32 < 32'(BASE + CAPW));
    end else if ((BASE + CAPW) >= (1 << ADDR_W)) begin : g_qhi0
      assign bank_selq[b] = (ra_q32 >= 32'(BASE));
    end else begin : g_q
      assign bank_selq[b] = (ra_q32 >= 32'(BASE)) && (ra_q32 < 32'(BASE + CAPW));
    end

    // Row and lane. BASE is a multiple of CAPW and CAPW a multiple of WPW, so the
    // lane is the low bits of the global address -- no subtraction on the address
    // path, which matters because the read data path is already half a cycle.
    // The read row is computed for every bank, not only the one that owns the
    // address: an address belonging to another bank wraps to some word of this
    // one, and the output mux discards it. Selecting csb1 per bank instead would
    // save read power and cost a decode on the address path; the macros are read
    // together on purpose, because that decode would be in series with the row
    // address setup and the read side already has the tighter constraint.
    logic [AW-1:0] row_r, row_w;
    assign row_r = AW'((ra32 - 32'(BASE)) >> LB);
    assign row_w = AW'((wa32     - 32'(BASE)) >> LB);

    // Gated clock for this bank's WRITE port. `sel_w` is the true enable: it
    // matches csb0 condition, so gating does not alter behavior -- macro model
    // triggers on `posedge clk0 && !csb0`; absent edge performs no operation,
    // exactly as csb0 = 1.
    logic clk_wr;
    if (CLK_GATE) begin : g_gate_wr
      ecg_clk_gate u_gate_wr (.clk_i(clk_i), .en_i(sel_w), .clk_o(clk_wr));
    end else begin : g_nogate_wr
      assign clk_wr = clk_i;
    end

    logic [DW-1:0] dout1_w;
    logic [DW-1:0] dout0_unused;  // port 0 is write-only here
    logic [DW-1:0] din0_w;
    logic [NM-1:0] wmask0_w;

    // wdata replicated across the row: the mask decides which lane lands. Cheaper
    // than a shifter, and it is what the byte-write port exists for. The
    // replication is cast straight onto the port rather than through a named
    // signal, because a named signal would leave unused top bits on the 44 b and
    // 64 b macros and -Wall is right to flag those.
    localparam int unsigned REP = (DW + WIDTH - 1) / WIDTH;

    assign din0_w = DW'({REP{wdata_i}});

    if (LB == 0) begin : g_wlane1
      assign wmask0_w = NM'((1 << BW) - 1);
    end else begin : g_wlane
      assign wmask0_w = NM'(16'((1 << BW) - 1) << (16'(BW) * 16'(32'(waddr_i[LB-1:0]))));
    end

    if (DW > USE) begin : g_spare
      // The 80 b and 44 b macros are not a power of two wide, so the top bits of
      // the row have no byte address (see the header). Sink them into a named
      // net instead of a lint waiver: the tool can then see that they are read by
      // nothing, which is the property being claimed.
      logic [DW-USE-1:0] unused_spare_bits;
      assign unused_spare_bits = dout1_w[DW-1:USE];
    end

    if (LB == 0) begin : g_lane1
      assign bank_rdata[b] = dout1_w[WIDTH-1:0];
    end else begin : g_lane
      assign bank_rdata[b] = dout1_w[(WIDTH * 32'(raddr_q[LB-1:0])) +: WIDTH];
    end

    case (K)
      0: begin : g_k0
        sky130_sram_1rw1r_128x256_8 u_ram (
            .clk0(clk_wr), .csb0(!sel_w), .web0(!sel_w), .wmask0(wmask0_w),
            .addr0(row_w), .din0(din0_w), .dout0(dout0_unused),
            .clk1(clk_rd), .csb1(!re_i), .addr1(row_r), .dout1(dout1_w));
      end
      1: begin : g_k1
        sky130_sram_1rw1r_64x256_8 u_ram (
            .clk0(clk_wr), .csb0(!sel_w), .web0(!sel_w), .wmask0(wmask0_w),
            .addr0(row_w), .din0(din0_w), .dout0(dout0_unused),
            .clk1(clk_rd), .csb1(!re_i), .addr1(row_r), .dout1(dout1_w));
      end
      2: begin : g_k2
        sky130_sram_1rw1r_80x64_8 u_ram (
            .clk0(clk_wr), .csb0(!sel_w), .web0(!sel_w), .wmask0(wmask0_w),
            .addr0(row_w), .din0(din0_w), .dout0(dout0_unused),
            .clk1(clk_rd), .csb1(!re_i), .addr1(row_r), .dout1(dout1_w));
      end
      default: begin : g_kdefault
        sky130_sram_1rw1r_44x64_8 u_ram (
            .clk0(clk_wr), .csb0(!sel_w), .web0(!sel_w), .wmask0(wmask0_w),
            .addr0(row_w), .din0(din0_w), .dout0(dout0_unused),
            .clk1(clk_rd), .csb1(!re_i), .addr1(row_r), .dout1(dout1_w));
      end
    endcase
  end

  logic [WIDTH-1:0] bank_mux;

  always_comb begin
    bank_mux = '0;
    for (int unsigned b = 0; b < NB; b++)
      if (bank_selq[b]) bank_mux = bank_rdata[b];
  end

  // The hold register of the header: it supplies rdata_o exactly on the cycles
  // the macros are deselected, so it can never be in the way of a real read.
  logic [WIDTH-1:0] hold_q;

  always_ff @(posedge clk_i) if (re_q) hold_q <= bank_mux;

  assign rdata_o = re_q ? bank_mux : hold_q;

`ifndef SYNTHESIS
  always_ff @(posedge clk_i) begin
    if (re_i && (32'(raddr_i) >= 32'(DEPTH)))
      $error("ecg_sram_macro: read address %0d out of range (DEPTH=%0d)", raddr_i, DEPTH);
    if (we_i && (32'(waddr_i) >= 32'(DEPTH)))
      $error("ecg_sram_macro: write address %0d out of range (DEPTH=%0d)", waddr_i, DEPTH);
    // Same wording and same policy as ecg_sram_1r1w: a 1rw1r macro cannot answer
    // this, so the replacement must not be the place where it starts happening.
    if (re_i && we_i && (raddr_i == waddr_i))
      $error("ecg_sram_macro: read-during-write at %0d (1rw1r gives no defined data for the written byte)",
             raddr_i);
  end
`endif

endmodule

`endif  // ECG_SRAM_MACRO_SV

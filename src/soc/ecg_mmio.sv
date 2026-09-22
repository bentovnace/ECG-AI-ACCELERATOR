// Memory-mapped peripheral for ecg_coproc: maps 34 raw coprocessor ports
// into an 8 KB address window on the CV32E40X data OBI bus.
//
// ARCHITECTURAL CONTEXT. Provides register and buffer access enabling
// end-to-end RISC-V software execution.
//
//   ecg_coproc has 34 ports. ecg_cvxif drives five inputs (start_i, single_i,
//   layer_i, dma_start_i, dma_len_i) and monitors three outputs (busy_o, done_o,
//   dma_busy_o). Remaining ports interface with memory and control registers.
//
//
//
//
//
//
// Subsystem features:
//
//   1. DESCRIPTOR TABLE. Combinational read table matching ecg_coproc contract
//      (desc_valid_i and desc_word_i asserted in same cycle as desc_req_o).
//
//
//
//
//   2. SCALE AND BIAS TABLES. s_off_o/b_off_o combinational read lookup.
//      Two independent tables for distinct index widths (s_off 14b, b_off 10b).
//
//
//
//   3. WEIGHT STREAMING. 64-bit word FIFO feeding coprocessor DMA.
//
//
//   4. SAMPLE INGESTION. Preload port to activation buffer active only when idle.
//
//
//
//   5. RESULT COLLECTION. Captures final layer writes (wr_last_layer_o) into FIFO.
//
//
//
// ERROR STATUS (ERRSTAT sticky flags):
//
//   * Out-of-bounds indices: desc_idx >= N_LAYER, s_off >= N_SCALE, b_off >= N_BIAS.
//   * Weight FIFO overflow: firmware writes faster than coprocessor drains.
//   * Result FIFO overflow: model generates more outputs than FIFO capacity.
//
//
//
// Sticky flags indicate execution validity without silent data corruption.
//
//
// ADDRESS MAP (offset in 8 KB window, 32-bit registers):
//
//   0x000  CTRL     W  bit0 start, bit1 single, bit2 dma_start, bit3 clr done,
//                      bit4 clr dma_done, bit5 clr ERRSTAT, bit6 ctrl_sel
//   0x004  STATUS   R  bit0 busy, bit1 done, bit2 dma_busy, bit3 dma_done,
//                      bit4 wf_empty, bit5 wf_full, bit6 rf_empty, bit7 rf_full
//   0x008  LAYER    RW [5:0]
//   0x00C  NLAYERS  RW [5:0]
//   0x010  IN_LEN   RW [9:0]
//   0x014  DMA_LEN  RW [WBITS:0]
//   0x018  RESCNT   R  number of results pending in FIFO
//   0x01C  RESRD    R  POP result: {last, buf[3:0], off[ABITS-1:0], data[7:0]}
//   0x020  WFIFO_L  W  lower 32 bits of 64-bit weight word
//   0x024  WFIFO_H  W  upper 32 bits AND PUSH into FIFO
//   0x028  PRE      W  single sample preload: {buf[3:0], off[ABITS-1:0], data[7:0]}
//   0x02C  ERRSTAT  R  bit0 desc oob, bit1 s_off oob, bit2 b_off oob, bit3 wf full,
//                      bit4 rf full, bit5 preload while busy, bit6 partial write,
//                      bit7 bus table oob, bit8 illegal desc, bit9 PRE4 overflow
//   0x030  PRE_ADDR W  set preload address pointer: {buf[3:0], off[ABITS-1:0]}
//   0x038  UART_RX  R  POP byte: {frame_err, has_byte, data[7:0]} (bit9/bit8)
//   0x03C  UART_TX  RW write: transmit byte; read: bit0 tx_ready
//   0x034  PRE4     W  four int8 samples packed in word, auto-increments address by 4.
//                      Byte 0 is first sample. Reduces write cycles from 259 to 65.
//   0x400  DESC     RW descriptor table: N_LAYER records x 16 B (4 words per record)
//   0x800  SCALE    RW requant scale table: ONE entry per WORD (offset = entry*4)
//   0x1000 BIAS     RW signed 9-bit bias table: ONE entry per WORD (offset = entry*4)
//
// ONE ENTRY PER WORD: avoids sub-word packing alignment bugs with word-addressed OBI bus.
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
module ecg_mmio
  import ecg_pkg::*;
#(
    // Table sizing according to measured requirements.
    //
    // Four model co-residency sizing:
    //   descriptor: 50 records (800 B)
    //   scale: 256 entries (512 B)
    //   bias: 562 entries (1124 B)
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
`ifdef ECG_MMIO_1RES
    parameter int unsigned N_LAYER = 14,
    parameter int unsigned N_SCALE = 133,
    parameter int unsigned N_BIAS  = 221,
`else
    parameter int unsigned N_LAYER = ECG_N_LAYER_TOTAL,   // 50 total across 4 models
    parameter int unsigned N_SCALE = ECG_N_REQUANT_SCALE, // 256 total across 4 models
    parameter int unsigned N_BIAS  = 562,                 // total across 4 models
`endif
    parameter int unsigned WFIFO_DEPTH = 8,
    parameter int unsigned RFIFO_DEPTH = 32,
    parameter int unsigned ABITS = $clog2(ECG_ACT_BYTES),
    parameter int unsigned WBITS = $clog2(ECG_WMEM_BYTES)
) (
    input  logic        clk_i,
    input  logic        rst_ni,

    // ---- BUS interface: OBI-lite slave (subset of CV32E40X data bus) --------
    input  logic        req_i,
    output logic        gnt_o,
    // addr_i has 32 bits from OBI bus; lower [12:0] decoded here.
    // Upper bits [31:13] decoded by parent module (ecg_soc).
    //
    /* verilator lint_off UNUSEDSIGNAL */
    input  logic [31:0] addr_i,
    /* verilator lint_on UNUSEDSIGNAL */
    input  logic        we_i,
    input  logic [3:0]  be_i,
    input  logic [31:0] wdata_i,
    output logic        rvalid_o,
    output logic [31:0] rdata_o,

    // ---- Coprocessor interface: 34 ports of ecg_coproc ----------------------
    // Control source select: 0 = custom instructions via CV-X-IF, 1 = MMIO.
    //
    //
    //
    //
    //
    //
    //
    //
    output logic        ctrl_sel_o,
    output logic        cp_start_o,
    output logic        cp_single_o,
    output logic [5:0]  cp_layer_o,
    output logic [5:0]  cp_n_layers_o,
    output logic [9:0]  cp_in_len_o,
    // 1-cycle pulse from ecg_coproc upon rejecting an illegal descriptor.
    // Latched into ERRSTAT bit 8 for software visibility.
    //
    input  logic                  cp_desc_illegal_i,
    input  logic        cp_busy_i,
    input  logic        cp_done_i,

    input  logic        desc_req_i,
    input  logic [5:0]  desc_idx_i,
    output logic        desc_valid_o,
    output logic [ECG_DESC_BYTES*8-1:0] desc_word_o,

    input  logic [13:0] s_off_i,
    input  logic [9:0]  b_off_i,
    output logic [ECG_REQUANT_MULT_BITS-1:0]  s_mult_o,
    output logic [ECG_REQUANT_SHIFT_BITS-1:0] s_shift_o,
    output logic signed [8:0] s_bias_o,

    // ---- UART interface: external IO channel --------------------------------
    // Khoi thanh ghi khong chua bo UART; no chi la CUA SO cho firmware. Bo UART
    // nam o ecg_soc de chan `rx_i`/`tx_o` ra duoc top.
    input  logic [7:0]  u_rx_data_i,
    input  logic        u_rx_valid_i,
    input  logic        u_rx_frame_err_i,
    output logic [7:0]  u_tx_data_o,
    output logic        u_tx_valid_o,
    input  logic        u_tx_ready_i,

    output logic        dma_start_o,
    output logic [WBITS:0] dma_len_o,
    input  logic        dma_busy_i,
    input  logic        dma_done_i,
    output logic        ws_valid_o,
    output logic [63:0] ws_data_o,
    input  logic        ws_ready_i,

    output logic        pre_we_o,
    output logic [3:0]  pre_buf_o,
    output logic [ABITS-1:0] pre_off_o,
    output logic signed [7:0] pre_data_o,

    input  logic        wr_i,
    input  logic [3:0]  wr_buf_i,
    input  logic [ABITS-1:0] wr_off_i,
    input  logic signed [7:0] wr_data_i,
    input  logic        wr_last_layer_i
);

  // ---- Address decoding ---------------------------------------------------
  // 8 KB window: addr[12:0] decoded. Word-aligned accesses ignore [1:0].
  // mot tu 32 bit (loi phat lenh lw/sw cho cac thanh ghi nay).
  localparam int unsigned DESC_W = ECG_DESC_BYTES * 8;   // 128
  localparam int unsigned RES_W  = 1 + 4 + ABITS + 8;    // last+buf+off+data
  localparam int unsigned WFCW  = $clog2(WFIFO_DEPTH) + 1;
  localparam int unsigned RFCW  = $clog2(RFIFO_DEPTH) + 1;

  // Lower [12:0] decoded in this module. Upper [31:13] decoded by parent SoC.
  //
  //
  //
  logic [12:0] a;
  assign a = addr_i[12:0];


  logic sel_reg, sel_desc, sel_scale, sel_bias;
  assign sel_reg   = (a[12:8] == 5'h00);                    // 0x000-0x0FF
  assign sel_desc  = (a[12] == 1'b0) && (a[11:10] == 2'b01); // 0x400-0x7FF
  assign sel_scale = (a[12] == 1'b0) && (a[11:10] == 2'b10); // 0x800-0xBFF
  assign sel_bias  = (a[12] == 1'b1);                        // 0x1000-0x1FFF

  // OBI-lite: accept transaction every cycle, return data next cycle.
  // gnt_o tied to 1'b1 without conditional backpressure stalls.
  //
  assign gnt_o = 1'b1;

  logic        rd_q;
  logic [31:0] rdata_q;
  assign rvalid_o = rd_q;
  assign rdata_o  = rdata_q;

  // ---- Configuration registers --------------------------------------------
  logic        single_q, ctrl_sel_q;
  logic [5:0]  layer_q, n_layers_q;
  logic [9:0]  in_len_q;
  logic [WBITS:0] dma_len_q;
  logic        start_q, dma_start_pulse_q;
  logic        done_sticky_q, dma_done_sticky_q;
  logic [11:0] err_q;  // bit7 = out-of-bounds bus index (`tbl_oob`)

  // ITEM APB-08: newly occurring error must not be masked by clear command.
  // Preserve newly set error bits when clearing existing ERRSTAT bits.
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
  logic desc_oob, s_oob, b_oob;
  logic rf_empty, rf_full;
  logic rf_push;

  logic [11:0] err_curr_cycle;
  always_comb begin
    err_curr_cycle    = '0;
    err_curr_cycle[0] = desc_oob;
    err_curr_cycle[1] = s_oob;
    err_curr_cycle[2] = b_oob;
    err_curr_cycle[4] = rf_push && rf_full;
    err_curr_cycle[8] = cp_desc_illegal_i;
  end

  // Full word write validation: partial writes flag error.
  //
  //
  logic be_day;
  assign be_day = (be_i == 4'hF);
                        // bit8  = coprocessor rejected illegal descriptor
                        // bit9  = PRE4 scatter overflow (write while busy)
                        // bit10 = UART RX overflow (unserviced byte overwritten)
                        // bit11 = UART TX overflow (write while transmitter busy)
  //
  //
  //

  assign ctrl_sel_o    = ctrl_sel_q;
  assign cp_start_o    = start_q;
  assign cp_single_o   = single_q;
  assign cp_layer_o    = layer_q;
  assign cp_n_layers_o = n_layers_q;
  assign cp_in_len_o   = in_len_q;
  assign dma_start_o   = dma_start_pulse_q;
  assign dma_len_o     = dma_len_q;

  // ---- Three tables: COMBINATIONAL READ -----------------------------------
  // Combinational read returns data in same cycle without read latency.
  //
  logic [DESC_W-1:0] desc_mem [N_LAYER];
  logic [15:0]       scale_mem [N_SCALE];
  // Bias stores signed 9-bit values directly without truncation.
  //
  logic [8:0]        bias_mem  [N_BIAS];

  // Out-of-bounds guards for descriptor, scale, and bias accesses.
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
  // Bus-side out-of-bounds check for memory-mapped access.
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
  //
  //
  logic tbl_oob;
  assign tbl_oob = (sel_desc  && (32'({26'b0, a[9:4]})  >= 32'(N_LAYER)))
                || (sel_scale && (32'({24'b0, a[9:2]})  >= 32'(N_SCALE)))
                || (sel_bias  && (32'({22'b0, a[11:2]}) >= 32'(N_BIAS)));

  if (N_LAYER < 64) begin : g_desc_chk
    assign desc_oob = desc_req_i && (desc_idx_i >= 6'(N_LAYER));
  end else begin : g_desc_full
    assign desc_oob = 1'b0;
  end
  if (N_SCALE < 16384) begin : g_scale_chk
    assign s_oob = (s_off_i >= 14'(N_SCALE));
  end else begin : g_scale_full
    assign s_oob = 1'b0;
  end
  if (N_BIAS < 1024) begin : g_bias_chk
    assign b_oob = (b_off_i >= 10'(N_BIAS));
  end else begin : g_bias_full
    assign b_oob = 1'b0;
  end

  assign desc_valid_o = desc_req_i;
  assign desc_word_o  = desc_oob ? '0 : desc_mem[desc_idx_i];

  // Scale entry packs multiplier in [10:0] and shift in [15:11].
  logic [15:0] sc_word;
  assign sc_word   = s_oob ? '0 : scale_mem[s_off_i[$clog2(N_SCALE)-1:0]];
  assign s_mult_o  = sc_word[ECG_REQUANT_MULT_BITS-1:0];
  assign s_shift_o = sc_word[10 + ECG_REQUANT_SHIFT_BITS : 11];
  // Signed 9-bit bias preserved as signed values.
  //
  assign s_bias_o  = b_oob ? '0 : $signed(bias_mem[b_off_i[$clog2(N_BIAS)-1:0]]);

  // ---- Weight FIFO --------------------------------------------------------
  logic [63:0] wf_mem [WFIFO_DEPTH];
  logic [WFCW-1:0] wf_cnt_q;
  logic [$clog2(WFIFO_DEPTH)-1:0] wf_rd_q, wf_wr_q;
  logic wf_empty, wf_full;
  assign wf_empty = (wf_cnt_q == 0);
  assign wf_full  = (wf_cnt_q == WFCW'(WFIFO_DEPTH));
  assign ws_valid_o = !wf_empty;
  assign ws_data_o  = wf_mem[wf_rd_q];

  logic [31:0] wf_lo_q;   // lower half held until upper half is written

  // Explicit enqueue and dequeue flags for unified counter update.
  //
  logic wf_enqueue, wf_dequeue;
  assign wf_dequeue = ws_valid_o && ws_ready_i;
  assign wf_enqueue = req_i && we_i && sel_reg && (a[7:0] == 8'h24) && !wf_full;

  // Result FIFO enqueue and dequeue flags.
  //
  //
  //
  //
  //
  //
  //
  //
  logic rf_enqueue, rf_dequeue;
  assign rf_enqueue = rf_push && !rf_full;
  assign rf_dequeue = req_i && !we_i && sel_reg && (a[7:0] == 8'h1C) && !rf_empty;

  // ---- Result FIFO --------------------------------------------------------
  logic [RES_W-1:0] rf_mem [RFIFO_DEPTH];
  logic [RFCW-1:0] rf_cnt_q;
  logic [$clog2(RFIFO_DEPTH)-1:0] rf_rd_q, rf_wr_q;
  assign rf_empty = (rf_cnt_q == 0);
  assign rf_full  = (rf_cnt_q == RFCW'(RFIFO_DEPTH));

  // Capture only final layer writes (wr_last_layer_i) to prevent FIFO overflow.
  //
  assign rf_push = wr_i && wr_last_layer_i;

  // ---- Preload port: one pulse per write to 0x028 -------------------------
  logic        pre_we_q;
  logic [3:0]  pre_buf_q;
  logic [ABITS-1:0] pre_off_q;
  // ---- Pointer and scatter engine for packed preload (PRE_ADDR / PRE4) -----
  // Ingests 4 packed int8 samples per 32-bit word, auto-incrementing address.
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
  // ---- UART: holding registers and status flags ---------------------------
  // Direct register buffering without extra FIFO stages.
  //
  //
  //
  //
  logic [7:0] u_rx_q;
  logic       u_rx_full_q;
  logic       u_frame_q;      // sticky flag, cleared via CTRL bit 5
  logic [7:0] u_tx_q;
  logic       u_tx_valid_q;
  assign u_tx_data_o  = u_tx_q;
  assign u_tx_valid_o = u_tx_valid_q;

  logic [3:0]       p4_buf_q;
  logic [ABITS-1:0] p4_off_q;
  logic [31:0]      p4_data_q;
  logic [2:0]       p4_cnt_q;
  logic             p4_busy;
  assign p4_busy = (p4_cnt_q != 3'd0);
  logic signed [7:0] pre_data_q;
  assign pre_we_o   = pre_we_q;
  assign pre_buf_o  = pre_buf_q;
  assign pre_off_o  = pre_off_q;
  assign pre_data_o = pre_data_q;

  // ---- Sequential logic ---------------------------------------------------
  // OBI protocol: every accepted transaction generates an rvalid response.
  //
  //
  //
  logic wr_cyc, rd_cyc;
  assign wr_cyc = req_i && we_i;
  assign rd_cyc = req_i && !we_i;

  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
      single_q <= 1'b0; ctrl_sel_q <= 1'b0; layer_q <= '0; n_layers_q <= '0; in_len_q <= '0;
      dma_len_q <= '0; start_q <= 1'b0; dma_start_pulse_q <= 1'b0;
      done_sticky_q <= 1'b0; dma_done_sticky_q <= 1'b0; err_q <= '0;
      wf_cnt_q <= '0; wf_rd_q <= '0; wf_wr_q <= '0; wf_lo_q <= '0;
      rf_cnt_q <= '0; rf_rd_q <= '0; rf_wr_q <= '0;
      pre_we_q <= 1'b0; pre_buf_q <= '0; pre_off_q <= '0; pre_data_q <= '0;
      p4_buf_q <= '0; p4_off_q <= '0; p4_data_q <= '0; p4_cnt_q <= '0;
      u_rx_q <= '0; u_rx_full_q <= 1'b0; u_frame_q <= 1'b0;
      u_tx_q <= '0; u_tx_valid_q <= 1'b0;
      rd_q <= 1'b0; rdata_q <= '0;
    end else begin
      // Single cycle pulses
      start_q           <= 1'b0;
      dma_start_pulse_q <= 1'b0;
      pre_we_q          <= 1'b0;
      rd_q              <= 1'b0;
      u_tx_valid_q      <= 1'b0;

      // Coprocessor sticky flags
      if (cp_done_i)  done_sticky_q     <= 1'b1;
      if (dma_done_i) dma_done_sticky_q <= 1'b1;

      // Out-of-bounds error flags
      if (desc_oob) err_q[0] <= 1'b1;
      if (s_oob)    err_q[1] <= 1'b1;
      if (b_oob)    err_q[2] <= 1'b1;
      if (cp_desc_illegal_i) err_q[8] <= 1'b1;

      // ---- Weight FIFO: read pointer update ----
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
      if (ws_valid_o && ws_ready_i) begin
        wf_rd_q <= wf_rd_q + 1'b1;
      end

      // ---- Weight FIFO counter update: 4 explicit cases ----
      //
      //
      case ({wf_enqueue, wf_dequeue})
        2'b10: wf_cnt_q <= wf_cnt_q + 1'b1;   // push only
        2'b01: wf_cnt_q <= wf_cnt_q - 1'b1;   // pop only
        default: ;                            // both or neither -> hold
      endcase

      // ---- Result FIFO counter update: 4 explicit cases ----
      case ({rf_enqueue, rf_dequeue})
        2'b10: rf_cnt_q <= rf_cnt_q + 1'b1;   // push only
        2'b01: rf_cnt_q <= rf_cnt_q - 1'b1;   // pop only
        default: ;                            // both or neither -> hold
      endcase

      // ---- Result FIFO: push on final layer write ----
      if (rf_push) begin
        if (rf_full) begin
          err_q[4] <= 1'b1;   // overflow flag
        end else begin
          rf_mem[rf_wr_q] <= {wr_last_layer_i, wr_buf_i, wr_off_i, wr_data_i};
          rf_wr_q  <= rf_wr_q + 1'b1;
          // `rf_cnt_q` KHONG gan o day -- xem khoi bon ca duy nhat o duoi.
        end
      end

      // ---- Bus WRITE handling ----
      if (wr_cyc) begin
        // Full word write verification: partial writes flagged.
        //
        //
        //
        //
        //
        //
        //
        if (be_i != 4'hF) err_q[6] <= 1'b1;
        if (sel_reg && be_day) begin
          case (a[7:0])
            8'h00: begin  // CTRL
              start_q           <= wdata_i[0];
              single_q          <= wdata_i[1];
              dma_start_pulse_q <= wdata_i[2];
              if (wdata_i[3]) done_sticky_q     <= 1'b0;
              if (wdata_i[4]) dma_done_sticky_q <= 1'b0;
              // ITEM APB-08: clear previous errors while retaining active cycle errors
              if (wdata_i[5]) err_q             <= err_curr_cycle;
              ctrl_sel_q <= wdata_i[6];
            end
            8'h08: layer_q    <= wdata_i[5:0];
            8'h0C: n_layers_q <= wdata_i[5:0];
            8'h10: in_len_q   <= wdata_i[9:0];
            8'h14: dma_len_q  <= wdata_i[WBITS:0];
            8'h20: wf_lo_q    <= wdata_i;
            8'h24: begin  // push 64-bit word
              if (wf_full) begin
                err_q[3] <= 1'b1;
              end else begin
                wf_mem[wf_wr_q] <= {wdata_i, wf_lo_q};
                wf_wr_q  <= wf_wr_q + 1'b1;
              end
            end
            8'h28: begin  // preload single sample
              // Preload shares buffer write port; forbidden while busy.
              //
              //
              if (cp_busy_i) begin
                err_q[5] <= 1'b1;
              end else begin
                pre_we_q   <= 1'b1;
                pre_data_q <= $signed(wdata_i[7:0]);
                pre_off_q  <= wdata_i[8+ABITS-1:8];
                pre_buf_q  <= wdata_i[8+ABITS+3:8+ABITS];
              end
            end
            8'h30: begin  // PRE_ADDR: set pointer {buf, off}
              if (cp_busy_i) begin
                err_q[5] <= 1'b1;
              end else begin
                p4_off_q <= wdata_i[ABITS-1:0];
                p4_buf_q <= wdata_i[ABITS+3:ABITS];
              end
            end
            8'h34: begin  // PRE4: packed 4 int8 samples, auto-increment
              if (cp_busy_i) begin
                err_q[5] <= 1'b1;
              end else if (p4_busy) begin
                // Scatter engine busy overflow check.
                //
                //
                //
                err_q[9] <= 1'b1;
              end else begin
                p4_data_q <= wdata_i;
                p4_cnt_q  <= 3'd4;
              end
            end
            8'h3C: begin  // UART_TX: transmit byte
              if (u_tx_ready_i) begin
                u_tx_q       <= wdata_i[7:0];
                u_tx_valid_q <= 1'b1;
              end else begin
                err_q[11] <= 1'b1;   // TX overflow: write while busy
              end
            end
            default: ;  // unmapped address: ignore without setting error
                        //
                        //
          endcase
        end else if (tbl_oob && be_day) begin
          // Out-of-bounds bus write: reject and set error flag.
          //
          //
          err_q[7] <= 1'b1;
        // ITEM EXEC-07: table writes not blocked by cp_busy_i.
        //
        //
        end else if (sel_desc && be_day) begin
          // 16 B record = 4 words. a[3:2] selects word.
          desc_mem[a[9:4]][{a[3:2], 5'b0} +: 32] <= wdata_i;
        end else if (sel_scale && be_day) begin
          // Two entries per 32-bit word.
          scale_mem[a[9:2]] <= wdata_i[15:0];
        end else if (sel_bias && be_day) begin
          bias_mem[a[11:2]] <= wdata_i[8:0];
        end
      end

      // rvalid generation for read and write cycles.
      if (wr_cyc) rd_q <= 1'b1;

      // ---- Bus READ handling: return data next cycle ----
      if (rd_cyc) begin
        rd_q <= 1'b1;
        if (sel_reg) begin
          case (a[7:0])
            8'h04: rdata_q <= {23'b0, p4_busy, rf_full, rf_empty, wf_full, wf_empty,
                               dma_done_sticky_q, dma_busy_i, done_sticky_q, cp_busy_i};
            8'h08: rdata_q <= {26'b0, layer_q};
            8'h0C: rdata_q <= {26'b0, n_layers_q};
            8'h10: rdata_q <= {22'b0, in_len_q};
            8'h14: rdata_q <= {{(31-WBITS){1'b0}}, dma_len_q};
            8'h18: rdata_q <= {{(32-RFCW){1'b0}}, rf_cnt_q};
            8'h1C: begin  // POP result
              if (rf_empty) begin
                rdata_q <= 32'hFFFF_FFFF;   // empty return value
              end else begin
                rdata_q <= {{(32-RES_W){1'b0}}, rf_mem[rf_rd_q]};
                rf_rd_q  <= rf_rd_q + 1'b1;
                // `rf_cnt_q` KHONG gan o day -- xem khoi bon ca duy nhat o duoi.
              end
            end
            8'h2C: rdata_q <= {20'b0, err_q};
            8'h38: begin  // UART_RX: POP byte
              rdata_q <= {22'b0, u_frame_q, u_rx_full_q, u_rx_q};
              u_rx_full_q <= 1'b0;   // read pops buffer
            end
            8'h3C: rdata_q <= {31'b0, u_tx_ready_i};
            8'h00: rdata_q <= {25'b0, ctrl_sel_q, 6'b0};  // readback ctrl_sel
            default: rdata_q <= 32'h0;
          endcase
        end else if (tbl_oob) begin
          // Return distinct signature on out-of-bounds read.
          //
          //
          rdata_q  <= 32'hDEAD_BEEF;
          err_q[7] <= 1'b1;
        end else if (sel_desc) begin
          rdata_q <= desc_mem[a[9:4]][{a[3:2], 5'b0} +: 32];
        end else if (sel_scale) begin
          rdata_q <= {16'b0, scale_mem[a[9:2]]};
        end else if (sel_bias) begin
          rdata_q <= {23'b0, bias_mem[a[11:2]]};
        end else begin
          rdata_q <= 32'h0;
        end
      end

      // ---- UART: sample incoming byte ----
      // Handled after read decode so concurrent read does not drop byte.
      //
      //
      if (u_rx_frame_err_i) u_frame_q <= 1'b1;
      if (u_rx_valid_i) begin
        if (u_rx_full_q) begin
          err_q[10] <= 1'b1;      // RX overflow: unread byte overwritten
        end else begin
          u_rx_q      <= u_rx_data_i;
          u_rx_full_q <= 1'b1;
        end
      end

      // ---- PRE4 scatter engine: 1 sample per cycle ----
      // Evaluated after write decode: active scatter maintains sequence priority.
      //
      //
      //
      //
      if (p4_cnt_q != 3'd0) begin
        pre_we_q   <= 1'b1;
        pre_data_q <= $signed(p4_data_q[7:0]);
        pre_off_q  <= p4_off_q;
        pre_buf_q  <= p4_buf_q;
        p4_data_q  <= {8'b0, p4_data_q[31:8]};
        p4_off_q   <= p4_off_q + 1'b1;
        p4_cnt_q   <= p4_cnt_q - 3'd1;
      end
    end
  end

`ifndef SYNTHESIS
  // ITEM APB-06 (Alignment verification): word-aligned accesses enforce a[1:0] == 0.
  //
  //
  //
  //
  //
  // Asynchronous reset matching module convention.
  //
  //
  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (rst_ni && req_i && (addr_i[1:0] != 2'b00))
      $error("ecg_mmio: dia chi LECH CAN 0x%08h (a[1:0] = %02b) -- bus nay theo TU va hai bit thap bi bo qua im lang", addr_i, addr_i[1:0]);
  end
`endif

endmodule

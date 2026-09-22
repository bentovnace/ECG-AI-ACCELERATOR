// UART 8-N-1: input channel for an EXTERNAL SIGNAL SOURCE (simulated PC or sensor).
//
// RATIONALE: Requirement demands connecting the SoC to a physical biomedical
// sensor (or emulated signals from a PC). Prior to this block, all design inputs
// were loaded via static hex files with `$readmemh` during simulation -- no
// runtime streaming input path existed. A bitstream on board would have no input.
//
// UART SELECTION: Chosen over SPI/I2C because every PC provides USB-serial,
// and Arty Z7-20 features an onboard USB-UART bridge. Physical sensors (such as
// ADS1292) use SPI, but SPI requires a clocked master block -- a separate IP that
// cannot interface with a PC directly. UART satisfies both branches of the requirement.
//
// MID-BIT SAMPLING: Samples at center of bit, not at edges. Receiver waits 1.5
// bit periods after start bit falling edge, then samples MID-BIT. Sampling at
// edges allows 1% clock drift to accumulate across 10 bits into 10%, causing bit
// errors silently since 8-N-1 has no parity.
//
//
// FRAME CHECKING: Receiver checks that stop bit is 1. If not, signals framing
// error (`rx_frame_err_o`) rather than outputting a corrupt byte -- clock drift
// or noise produces garbage bytes, and silent corruption is fatal to sampling.
//
//
module ecg_uart #(
    // Clock cycles per BIT. Default: 100 MHz / 115200 = 868. Parameterized so
    // simulations can use a small value to avoid 868 cycles per bit -- a test
    // running at actual hardware rate would be too slow in CI.
    //
    parameter int unsigned CK_MOI_BIT = 868
) (
    input  logic       clk_i,
    input  logic       rst_ni,

    // ---- external pins -----------------------------------------------------
    input  logic       rx_i,
    output logic       tx_o,

    // ---- internal: RECEIVE -------------------------------------------------
    output logic [7:0] rx_data_o,
    output logic       rx_valid_o,      // single-cycle pulse
    output logic       rx_frame_err_o,  // single-cycle pulse: stop bit != 1

    // ---- internal: TRANSMIT ------------------------------------------------
    input  logic [7:0] tx_data_i,
    input  logic       tx_valid_i,      // assert when tx_ready_o is high
    output logic       tx_ready_o
);

  localparam int unsigned HALF_BIT = CK_MOI_BIT / 2;
  localparam int unsigned CNT_W = $clog2(CK_MOI_BIT + 1);

  // ======================================================== RECEIVE
  typedef enum logic [1:0] { R_IDLE, R_START, R_DATA, R_STOP } r_e;
  r_e          r_state_q;
  logic [CNT_W-1:0] r_cnt_q;
  logic [2:0]  r_bit_q;
  logic [7:0]  r_sr_q;
  logic        rx_q, rx_q2;

  assign rx_data_o = r_sr_q;

  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
      r_state_q <= R_IDLE; r_cnt_q <= '0; r_bit_q <= '0; r_sr_q <= '0;
      rx_valid_o <= 1'b0; rx_frame_err_o <= 1'b0;
      rx_q <= 1'b1; rx_q2 <= 1'b1;
    end else begin
      rx_valid_o     <= 1'b0;
      rx_frame_err_o <= 1'b0;
      // Two-stage synchronizer on external pin to prevent metastability
      // from external asynchronous pin into internal state machine.
      //
      rx_q  <= rx_i;
      rx_q2 <= rx_q;

      unique case (r_state_q)
        R_IDLE: if (!rx_q2) begin           // falling edge = start bit
                  r_state_q <= R_START;
                  r_cnt_q   <= '0;
                end
        R_START: begin
          r_cnt_q <= r_cnt_q + 1'b1;
          if (r_cnt_q == CNT_W'(HALF_BIT)) begin
            // Mid-bit of start bit. If line reverted high, it was noise;
            // return to IDLE rather than accepting corrupted byte.
            if (rx_q2) begin
              r_state_q <= R_IDLE;
            end else begin
              r_state_q <= R_DATA;
              r_cnt_q   <= '0;
              r_bit_q <= '0;
            end
          end
        end
        R_DATA: begin
          r_cnt_q <= r_cnt_q + 1'b1;
          if (r_cnt_q == CNT_W'(CK_MOI_BIT - 1)) begin
            r_cnt_q <= '0;
            r_sr_q  <= {rx_q2, r_sr_q[7:1]};   // LSB first, standard 8-N-1
            r_bit_q <= r_bit_q + 1'b1;
            if (r_bit_q == 3'd7) r_state_q <= R_STOP;
          end
        end
        R_STOP: begin
          r_cnt_q <= r_cnt_q + 1'b1;
          if (r_cnt_q == CNT_W'(CK_MOI_BIT - 1)) begin
            r_cnt_q <= '0;
            r_state_q <= R_IDLE;
            if (rx_q2) rx_valid_o     <= 1'b1;   // stop = 1 -> valid byte
            else       rx_frame_err_o <= 1'b1;   // framing error
          end
        end
        default: r_state_q <= R_IDLE;
      endcase
    end
  end

  // ======================================================== TRANSMIT
  typedef enum logic [1:0] { T_IDLE, T_START, T_DATA, T_STOP } t_e;
  t_e          t_state_q;
  logic [CNT_W-1:0] t_cnt_q;
  logic [2:0]  t_bit_q;
  logic [7:0]  t_sr_q;

  assign tx_ready_o = (t_state_q == T_IDLE);

  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
      t_state_q <= T_IDLE; t_cnt_q <= '0; t_bit_q <= '0; t_sr_q <= '0; tx_o <= 1'b1;
    end else begin
      unique case (t_state_q)
        T_IDLE: begin
          tx_o <= 1'b1;                    // line idle high
          if (tx_valid_i) begin
            t_sr_q  <= tx_data_i;
            t_state_q <= T_START;
            t_cnt_q   <= '0;
          end
        end
        T_START: begin
          tx_o    <= 1'b0;
          t_cnt_q <= t_cnt_q + 1'b1;
          if (t_cnt_q == CNT_W'(CK_MOI_BIT - 1)) begin
            t_cnt_q <= '0; t_bit_q <= '0; t_state_q <= T_DATA;
          end
        end
        T_DATA: begin
          tx_o    <= t_sr_q[0];
          t_cnt_q <= t_cnt_q + 1'b1;
          if (t_cnt_q == CNT_W'(CK_MOI_BIT - 1)) begin
            t_cnt_q <= '0;
            t_sr_q  <= {1'b0, t_sr_q[7:1]};
            t_bit_q <= t_bit_q + 1'b1;
            if (t_bit_q == 3'd7) t_state_q <= T_STOP;
          end
        end
        T_STOP: begin
          tx_o    <= 1'b1;
          t_cnt_q <= t_cnt_q + 1'b1;
          if (t_cnt_q == CNT_W'(CK_MOI_BIT - 1)) begin
            t_cnt_q <= '0; t_state_q <= T_IDLE;
          end
        end
        default: t_state_q <= T_IDLE;
      endcase
    end
  end

`ifdef FORMAL
  // `rx_valid_o` and `rx_frame_err_o` are mutually exclusive.
  // A frame is either valid or erroneous, never both.
  assert property (@(posedge clk_i) disable iff (!rst_ni)
      !(rx_valid_o && rx_frame_err_o));
  // Both are single-cycle pulses.
  assert property (@(posedge clk_i) disable iff (!rst_ni) rx_valid_o |=> !rx_valid_o);
`endif

endmodule

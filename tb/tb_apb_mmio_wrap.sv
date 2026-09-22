// Wrapper connecting REAL APB SLAVE with REAL MMIO PERIPHERAL (W1-D / F07+F08).
//
// WHY WRAPPER IS NEEDED: `tb_ecg_apb` runs `ecg_apb_slave` STANDALONE and only
// verifies `be_o`. The question of F07/F08 concerns REAL REGISTER STATE -- whether
// an empty write strobe triggers `cp_start_o`. `be_o` alone does not answer this:
// it reflects intended byte strobes, not what peripheral logic actually EXECUTES.
// 
`ifndef TB_APB_MMIO_WRAP_SV
`define TB_APB_MMIO_WRAP_SV

module tb_apb_mmio_wrap
  import ecg_pkg::*;
(
    input  logic        clk_i,
    input  logic        rst_ni,
    // APB interface
    input  logic        psel_i,
    input  logic        penable_i,
    input  logic        pwrite_i,
    input  logic [31:0] paddr_i,
    input  logic [31:0] pwdata_i,
    input  logic [3:0]  pstrb_i,
    output logic        pready_o,
    output logic        pslverr_o,
    output logic [31:0] prdata_o,
    // Observable side effects of CTRL register
    output logic        cp_start_o,
    output logic        cp_single_o,
    output logic        dma_start_o,
    output logic        ctrl_sel_o,
    // ---- Direct CPU DATA port (master A of `ecg_mmio_mux`) ----------
    // This path bypasses APB bridge; firmware `sb`/`sh` to MMIO
    // is unmediated by bridge logic. `be_day` in `ecg_mmio` is the ONLY gate
    // at this stage, which this testbench validates.
    input  logic        cpu_req_i,
    input  logic        cpu_we_i,
    input  logic [3:0]  cpu_be_i,
    input  logic [31:0] cpu_addr_i,
    input  logic [31:0] cpu_wdata_i,
    output logic        cpu_gnt_o,
    output logic        cpu_rvalid_o,
    output logic [31:0] cpu_rdata_o,
    // OBI bus between bridge and peripheral for cross-check
    output logic [3:0]  be_o
);
  logic        req, gnt, we, rvalid;
  logic [31:0] addr, wdata, rdata;
    // OBI bus downstream of mux, driving peripheral directly
  logic        s_req, s_gnt, s_we, s_rvalid;
  logic [3:0]  s_be;
  logic [31:0] s_addr, s_wdata, s_rdata;

  ecg_apb_slave #(.ABITS(32)) u_apb (
      .clk_i, .rst_ni,
      .psel_i, .penable_i, .pwrite_i, .paddr_i, .pwdata_i, .pstrb_i,
      .pready_o, .pslverr_o, .prdata_o,
      .req_o (req), .gnt_i (gnt), .addr_o (addr), .we_o (we),
      .be_o, .wdata_o (wdata), .rvalid_i (rvalid), .rdata_i (rdata)
  );

  // REAL MUX instance matching `ecg_soc` hierarchy, ensuring test
  // verifies system integration directly.
  ecg_mmio_mux u_mux (
      .clk_i, .rst_ni,
      .a_req_i (cpu_req_i), .a_gnt_o (cpu_gnt_o), .a_addr_i (cpu_addr_i),
      .a_we_i (cpu_we_i), .a_be_i (cpu_be_i), .a_wdata_i (cpu_wdata_i),
      .a_rvalid_o (cpu_rvalid_o), .a_rdata_o (cpu_rdata_o),
      .b_req_i (req), .b_gnt_o (gnt), .b_addr_i (addr), .b_we_i (we),
      .b_be_i (be_o), .b_wdata_i (wdata), .b_rvalid_o (rvalid), .b_rdata_o (rdata),
      .s_req_o (s_req), .s_gnt_i (s_gnt), .s_addr_o (s_addr), .s_we_o (s_we),
      .s_be_o (s_be), .s_wdata_o (s_wdata), .s_rvalid_i (s_rvalid),
      .s_rdata_i (s_rdata)
  );

  ecg_mmio u_mmio (
      .clk_i, .rst_ni,
      .req_i (s_req), .gnt_o (s_gnt), .addr_i (s_addr), .we_i (s_we), .be_i (s_be),
      .wdata_i (s_wdata), .rvalid_o (s_rvalid), .rdata_o (s_rdata),
      .ctrl_sel_o, .cp_start_o, .cp_single_o, .cp_layer_o (), .cp_n_layers_o (),
      .cp_in_len_o (), .cp_desc_illegal_i (1'b0),
      .cp_busy_i (1'b0), .cp_done_i (1'b0),
      .desc_req_i (1'b0), .desc_idx_i ('0), .desc_valid_o (), .desc_word_o (),
      .s_off_i ('0), .b_off_i ('0), .s_mult_o (), .s_shift_o (), .s_bias_o (),
      .u_rx_data_i ('0), .u_rx_valid_i (1'b0), .u_rx_frame_err_i (1'b0),
      .u_tx_data_o (), .u_tx_valid_o (), .u_tx_ready_i (1'b1),
      .dma_start_o, .dma_len_o (), .dma_busy_i (1'b0), .dma_done_i (1'b0),
      .ws_valid_o (), .ws_data_o (), .ws_ready_i (1'b1),
      .pre_we_o (), .pre_buf_o (), .pre_off_o (), .pre_data_o (),
      .wr_i (1'b0), .wr_buf_i ('0), .wr_off_i ('0), .wr_data_i ('0),
      .wr_last_layer_i (1'b0)
  );
endmodule

`endif

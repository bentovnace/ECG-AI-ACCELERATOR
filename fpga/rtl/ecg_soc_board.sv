// Wrapper cho bo mach: thu ngan 283 bit cong cua `ecg_soc` xuong vua ngan sach chan.
//
// VI SAO CAN. `ecg_soc` phoi ra 283 bit cong (`mcycle_o` 64, `dbg_pc_o` 32,
// `dbg_data_o` 32, `dbg_instr_addr_o` 32, ca cum APB 104, ...). Part
// `xc7z020clg400-1` co ~125 chan PL. Khong the dat `ecg_soc` lam top cua mot
// lan implement that -- no khong phai chuyen kho, no khong the.
//
// Cac cong do ton tai co ly do (quan sat cho testbench va cho bring-up), va
// chung DUNG o do. Cai sai se la doi `ecg_soc.sv` de phuc vu mot rang buoc cua
// bo mach. Nen thu ngan o TANG NGOAI, trong lane FPGA, dung nhu `60-fpga/README`
// da dat ra: "RTL thiet ke phai giu doc lap cong cu".
//
// GIU CHO THIET KE KHONG BI TOI UU BAY. Buoc dau vao ve hang so se cho tong hop
// truyen hang so vao trong va xoa mat phan lon SoC -- bao cao tai nguyen khi do
// se la bao cao cua mot thiet ke KHAC. Hai lop bao ve, co y dung ca hai:
//   1. `DONT_TOUCH` tren the hien `ecg_soc` -- chan truyen hang so qua bien.
//   2. Moi cong ra rong deu bi RUT XOR ve mot bit va dua ra `led_o[3]`, nen
//      chung co mot noi tieu that chu khong chi mot thuoc tinh.
// Chi mot trong hai cung du ve nguyen tac; ca hai thi khong phu thuoc vao viec
// cong cu ton trong mot thuoc tinh.

module ecg_soc_board #(
    parameter string IMEM_HEX        = "",
    parameter string DMEM_HEX        = "",
    parameter bit    USE_MMCM        = 1,
    parameter bit    RST_ACTIVE_HIGH = 1   // Arty Z7 push buttons are active high (pressed = 1)
) (
    input  logic       clk_i,          // 125 MHz oscillator on pin H16
    input  logic       rst_ni,         // BTN0 on Arty Z7 (pin D19)
    input  logic       fetch_enable_i, // SW0 on Arty Z7 (pin M20)
    input  logic       uart_rx_i,      // Pmod JA pin 1 (pin Y18)
    output logic       uart_tx_o,      // Pmod JA pin 2 (pin Y19)
    output logic [3:0] led_o           // LD0..LD3 on Arty Z7 (R14, P14, N16, M14)
);

  // ---- MMCM: 125 MHz -> 41.6667 MHz (T = 24.0 ns) ------------------------
  logic clk_soc, clk_fb, mmcm_locked;

  if (USE_MMCM) begin : gen_mmcm
    MMCME2_BASE #(
        .BANDWIDTH("OPTIMIZED"),
        .CLKFBOUT_MULT_F(8.0),       // VCO = 125 MHz * 8 = 1000 MHz (800..1600 MHz)
        .CLKFBOUT_PHASE(0.0),
        .CLKIN1_PERIOD(8.0),         // 125 MHz = 8.000 ns
        .CLKOUT0_DIVIDE_F(24.0),     // 1000 MHz / 24 = 41.6667 MHz (24.000 ns)
        .CLKOUT0_DUTY_CYCLE(0.5),
        .CLKOUT0_PHASE(0.0),
        .DIVCLK_DIVIDE(1),
        .REF_JITTER1(0.010),
        .STARTUP_WAIT("FALSE")
    ) u_mmcm (
        .CLKOUT0  (clk_soc),
        .CLKOUT0B (),
        .CLKOUT1  (),
        .CLKOUT1B (),
        .CLKOUT2  (),
        .CLKOUT2B (),
        .CLKOUT3  (),
        .CLKOUT3B (),
        .CLKOUT4  (),
        .CLKOUT5  (),
        .CLKOUT6  (),
        .CLKFBOUT (clk_fb),
        .CLKFBOUTB(),
        .LOCKED   (mmcm_locked),
        .CLKIN1   (clk_i),
        .PWRDWN   (1'b0),
        .RST      (1'b0),
        .CLKFBIN  (clk_fb)
    );
  end else begin : gen_no_mmcm
    assign clk_soc     = clk_i;
    assign mmcm_locked = 1'b1;
  end

  // Reset conditioning: on Arty Z7, BTN0 is pressed=1, released=0.
  // When RST_ACTIVE_HIGH=1: pressing BTN0 asserts reset (rst_n_int = 0).
  // Also hold SoC in reset until MMCM frequency is locked and stable!
  logic rst_n_int;
  assign rst_n_int = (RST_ACTIVE_HIGH ? ~rst_ni : rst_ni) & mmcm_locked;

  logic        core_sleep, cp_busy, cp_done, xif_kill;
  logic [63:0] mcycle;
  logic [31:0] dbg_data, dbg_pc, dbg_instr_addr;
  logic        dbg_pc_valid;
  logic [31:0] apb_prdata;
  logic        apb_pready, apb_pslverr;

  (* DONT_TOUCH = "yes" *)
  ecg_soc #(
      .IMEM_HEX (IMEM_HEX),
      .DMEM_HEX (DMEM_HEX)
  ) u_soc (
      .clk_i            (clk_soc),
      .rst_ni           (rst_n_int),
      .fetch_enable_i   (fetch_enable_i),
      .core_sleep_o     (core_sleep),
      .cp_busy_o        (cp_busy),
      .cp_done_o        (cp_done),
      .xif_kill_seen_o  (xif_kill),
      .mcycle_o         (mcycle),
      .dbg_addr_i       (8'd0),
      .dbg_data_o       (dbg_data),
      .dbg_pc_valid_o   (dbg_pc_valid),
      .dbg_pc_o         (dbg_pc),
      .dbg_instr_addr_o (dbg_instr_addr),
      // Cum APB khong ra chan: khong co master APB tren duong bring-up nay.
      // Buoc ve khong va dua vao `DONT_TOUCH` de logic APB van duoc tong hop.
      .apb_psel_i       (1'b0),
      .apb_penable_i    (1'b0),
      .apb_pwrite_i     (1'b0),
      .apb_paddr_i      (32'd0),
      .apb_pwdata_i     (32'd0),
      .apb_pstrb_i      (4'd0),
      .apb_prdata_o     (apb_prdata),
      .apb_pready_o     (apb_pready),
      .apb_pslverr_o    (apb_pslverr),
      .uart_rx_i        (uart_rx_i),
      .uart_tx_o        (uart_tx_o)
  );

  // Ba den bao co nghia cho bring-up, den thu tu la noi tieu cho phan con lai.
  logic [3:0] led_q;
  always_ff @(posedge clk_soc or negedge rst_n_int) begin
    if (!rst_n_int) led_q <= 4'd0;
    else begin
      led_q[0] <= core_sleep;
      led_q[1] <= cp_busy;
      led_q[2] <= cp_done;
      led_q[3] <= xif_kill ^ (^mcycle) ^ (^dbg_data) ^ dbg_pc_valid
                ^ (^dbg_pc) ^ (^dbg_instr_addr)
                ^ (^apb_prdata) ^ apb_pready ^ apb_pslverr;
    end
  end
  assign led_o = led_q;

endmodule

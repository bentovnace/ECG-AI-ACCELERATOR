// `ecg_sram_1r1w` implemented using physical sky130 SRAM macros (OpenRAM),
// used for ASIC flow when ECG_SRAM=macro.
//
// Why this file exists separately without modifying ecg_sram_macro.sv:
//   * `ecg_sram_macro` is a module with a different name. Rest of RTL references
//     `ecg_sram_1r1w`, so a wrapper matching that module name is required.
//   * Loaded IN PLACE OF 40-rtl/asic/ecg_sram_1r1w_bb.sv. Both files define
//     the same module name and must not be loaded simultaneously.
//
//
//
// Blackbox stubs below prevent yosys from mapping behavioural macro models into flip-flops.
// Ports and widths must match platforms/sky130ram/<macro>/<macro>.v.
// Slang validates port connectivity, guarding against divergence.
//
//
`ifndef ECG_SRAM_1R1W_MACRO_SV
`define ECG_SRAM_1R1W_MACRO_SV

(* blackbox *)
module sky130_sram_1rw1r_128x256_8 (
    input  logic         clk0,
    input  logic         csb0,
    input  logic         web0,
    input  logic [15:0]  wmask0,
    input  logic [7:0]   addr0,
    input  logic [127:0] din0,
    output logic [127:0] dout0,
    input  logic         clk1,
    input  logic         csb1,
    input  logic [7:0]   addr1,
    output logic [127:0] dout1
);
endmodule

(* blackbox *)
module sky130_sram_1rw1r_64x256_8 (
    input  logic        clk0,
    input  logic        csb0,
    input  logic        web0,
    input  logic [7:0]  wmask0,
    input  logic [7:0]  addr0,
    input  logic [63:0] din0,
    output logic [63:0] dout0,
    input  logic        clk1,
    input  logic        csb1,
    input  logic [7:0]  addr1,
    output logic [63:0] dout1
);
endmodule

(* blackbox *)
module sky130_sram_1rw1r_80x64_8 (
    input  logic        clk0,
    input  logic        csb0,
    input  logic        web0,
    input  logic [9:0]  wmask0,
    input  logic [5:0]  addr0,
    input  logic [79:0] din0,
    output logic [79:0] dout0,
    input  logic        clk1,
    input  logic        csb1,
    input  logic [5:0]  addr1,
    output logic [79:0] dout1
);
endmodule

(* blackbox *)
module sky130_sram_1rw1r_44x64_8 (
    input  logic        clk0,
    input  logic        csb0,
    input  logic        web0,
    input  logic [5:0]  wmask0,
    input  logic [5:0]  addr0,
    input  logic [43:0] din0,
    output logic [43:0] dout0,
    input  logic        clk1,
    input  logic        csb1,
    input  logic [5:0]  addr1,
    output logic [43:0] dout1
);
endmodule

module ecg_sram_1r1w #(
    parameter int unsigned WIDTH = 8,
    parameter int unsigned DEPTH = 4096,
    localparam int unsigned ADDR_W = (DEPTH <= 1) ? 1 : $clog2(DEPTH)
) (
    input  logic                clk_i,

    input  logic                re_i,
    input  logic [ADDR_W-1:0]   raddr_i,
    output logic [WIDTH-1:0]    rdata_o,

    input  logic                we_i,
    input  logic [ADDR_W-1:0]   waddr_i,
    input  logic [WIDTH-1:0]    wdata_i
);

  ecg_sram_macro #(.WIDTH(WIDTH), .DEPTH(DEPTH)) u_macro (
      .clk_i  (clk_i),
      .re_i   (re_i),
      .raddr_i(raddr_i),
      .rdata_o(rdata_o),
      .we_i   (we_i),
      .waddr_i(waddr_i),
      .wdata_i(wdata_i)
  );

endmodule

`endif  // ECG_SRAM_1R1W_MACRO_SV

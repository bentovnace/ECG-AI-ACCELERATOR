// BLACKBOX stub for ecg_sram_1r1w, used only in ASIC flow.
//
// On FPGA, Vivado infers ecg_sram_1r1w as RAMB18 for free. On ASIC there is no
// RAMB18: allowing yosys to map behavioural memory turns 10,808 bytes into
// flip-flops and massive read multiplexers. Measured: ecg_coproc yielded
// 3,391,618 um2 = 3.39 mm2 with 16,590 mux4 cells -- while same design on
// FPGA is 2,457 LUTs (~0.12-0.25 mm2). 3.39 mm2 is 14-28x too large because
// memory was synthesized as logic.
//
// `read_slang` flattens hierarchy, so yosys `blackbox` cannot be called after
// elaboration: submodule no longer exists to match names. Declaring a stub
// with (* blackbox *) and loading it IN PLACE OF real file maintains boundary.
//
//
// Result: hierarchy boundary stops at SRAM, logic area is measured separately,
// and SRAM macro area (OpenRAM/sky130ram) is added in downstream flow.
//
//
// Ports must match character-for-character with 40-rtl/src/common/ecg_sram_1r1w.sv.
// Mismatches trigger slang elaboration errors, guarding against divergence.
//
`ifndef ECG_SRAM_1R1W_BB_SV
`define ECG_SRAM_1R1W_BB_SV

(* blackbox *)
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
endmodule

`endif  // ECG_SRAM_1R1W_BB_SV

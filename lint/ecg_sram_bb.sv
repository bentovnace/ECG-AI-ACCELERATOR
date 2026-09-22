// Blackbox stubs for area measurement only. Never in a synthesis or simulation
// filelist.
//
// Without a target technology yosys maps the 4 kB activation store to 32,768
// flip-flops and reports ecg_actbuf at 70,856 cells, which measures the absence
// of a memory macro rather than the module's own logic. N9 is a statement about
// logic sharing between the four families, so the memory must be excluded from
// that count -- it is shared by all four unconditionally.

(* blackbox *)
module ecg_sram_1r1w #(
    parameter int unsigned WIDTH = 8,
    parameter int unsigned DEPTH = 4096,
    // A plain parameter, not a localparam: yosys refuses a blackbox that is
    // instantiated with parameters but declares none as overridable.
    parameter int unsigned ADDR_W = 12
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

(* blackbox *)
module ecg_sram #(
    parameter int unsigned WIDTH = 8,
    parameter int unsigned DEPTH = 256,
    parameter int unsigned ADDR_W = 8
) (
    input  logic                clk_i,
    input  logic                en_i,
    input  logic                we_i,
    input  logic [ADDR_W-1:0]   addr_i,
    input  logic [WIDTH-1:0]    wdata_i,
    output logic [WIDTH-1:0]    rdata_o
);
endmodule

// Single-port synchronous SRAM wrapper.
//
// Every memory in the coprocessor goes through this module. That is the point:
// N7 caps total on-chip memory at 24 kB and the design currently sits at
// 23.704 B, so 872 B of headroom. Having one place that instantiates storage
// means the budget can be audited by grepping for instances of this module
// rather than by reading every file.
//
// The write style below is the one Vivado infers as block RAM: synchronous
// read with the address registered, no reset on the data output, and no
// read-during-write bypass. Adding a bypass would force distributed RAM and
// blow the budget, so it is deliberately absent -- the sequencer never reads a
// word in the same cycle it writes it.

`ifndef ECG_SRAM_SV
`define ECG_SRAM_SV

module ecg_sram #(
    parameter int unsigned WIDTH = 8,
    parameter int unsigned DEPTH = 256,
    // Read latency is one cycle and the cycle model in P4.4 assumes exactly
    // that. Exposed as a localparam so a reader does not have to infer it.
    localparam int unsigned ADDR_W = (DEPTH <= 1) ? 1 : $clog2(DEPTH)
) (
    input  logic                clk_i,
    input  logic                en_i,      // clock enable for both read and write
    input  logic                we_i,
    input  logic [ADDR_W-1:0]   addr_i,
    input  logic [WIDTH-1:0]    wdata_i,
    output logic [WIDTH-1:0]    rdata_o
);

  logic [WIDTH-1:0] mem [DEPTH];

  always_ff @(posedge clk_i) begin
    if (en_i) begin
      if (we_i) mem[addr_i] <= wdata_i;
      // Read-first is not modelled: rdata_o after a write to the same address
      // is the old word. No consumer relies on either behaviour.
      else      rdata_o <= mem[addr_i];
    end
  end

`ifndef SYNTHESIS
  // An out-of-range address is a design error, not a runtime condition: the
  // address generators are counters bounded by descriptor fields, so if this
  // fires the descriptor and the RTL disagree, which is exactly the class of
  // bug N2 would otherwise catch far downstream.
  always_ff @(posedge clk_i) begin
    // Widened on purpose: with a power-of-two DEPTH, ADDR_W'(DEPTH) truncates
    // to zero and the comparison would always hold.
    if (en_i && (32'(addr_i) >= 32'(DEPTH))) begin
      $error("ecg_sram: address %0d out of range (DEPTH=%0d)", addr_i, DEPTH);
    end
  end
`endif

endmodule

`endif  // ECG_SRAM_SV

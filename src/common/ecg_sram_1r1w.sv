// Simple dual-port SRAM: one read port, one write port, synchronous.
//
// The activation buffer needs to read an operand and write a previous result in
// the same cycle, which a single-port memory cannot do. This is the "simple dual
// port" configuration that Vivado infers as block RAM at no extra cost over a
// single-port BRAM, so it does not change the N7 accounting.
//
// Read-during-write to the same address returns the OLD word. The sequencer
// never does that -- a layer reads only buffers an earlier layer finished
// writing -- and the assertion below fires if that ever stops being true,
// because relying on either behaviour would make the RTL depend on something the
// budget does not pay for.

`ifndef ECG_SRAM_1R1W_SV
`define ECG_SRAM_1R1W_SV

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

  logic [WIDTH-1:0] mem [DEPTH];

  always_ff @(posedge clk_i) begin
    if (we_i) mem[waddr_i] <= wdata_i;
    if (re_i) rdata_o <= mem[raddr_i];
  end

`ifndef SYNTHESIS
  always_ff @(posedge clk_i) begin
    // Widened on purpose: with a power-of-two DEPTH, ADDR_W'(DEPTH) truncates to
    // zero and the comparison would always hold.
    if (re_i && (32'(raddr_i) >= 32'(DEPTH)))
      $error("ecg_sram_1r1w: read address %0d out of range (DEPTH=%0d)",
             raddr_i, DEPTH);
    if (we_i && (32'(waddr_i) >= 32'(DEPTH)))
      $error("ecg_sram_1r1w: write address %0d out of range (DEPTH=%0d)",
             waddr_i, DEPTH);
    if (re_i && we_i && (raddr_i == waddr_i))
      $error("ecg_sram_1r1w: read-during-write at %0d (sequencer must not read a word it writes this cycle)", raddr_i);
  end
`endif

endmodule

`endif  // ECG_SRAM_1R1W_SV

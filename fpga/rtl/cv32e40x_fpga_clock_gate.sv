// FPGA implementation of the clock gate the CV32E40X sleep unit instantiates.
//
// The core ships only bhv/cv32e40x_sim_clock_gate.sv, whose header forbids both
// ASIC and FPGA synthesis: it is an always_latch behavioural model. Vivado stops
// with [Synth 8-439] module 'cv32e40x_clock_gate' not found unless a real one is
// supplied, which is why the ADR-0003 §6 core-only experiment had never run.
//
// BUFGCE is the 7-series equivalent: a global clock buffer with a clock enable,
// so the gated clock stays on the dedicated clock network instead of being routed
// as logic. Gating a clock with a LUT would work in simulation and then fail
// timing, or silently glitch, on the board.
//
// This file is FPGA-only and deliberately lives outside 40-rtl: the design RTL
// must stay technology independent. The ASIC flow supplies its own gate from the
// standard-cell library.

module cv32e40x_clock_gate #(
    // Present only to match the core's instantiation; the FPGA has one gate cell.
    parameter int unsigned LIB = 0
) (
    input  logic clk_i,
    input  logic en_i,
    input  logic scan_cg_en_i,
    output logic clk_o
);

  BUFGCE u_bufgce (
      .I (clk_i),
      .CE(en_i | scan_cg_en_i),
      .O (clk_o)
  );

endmodule

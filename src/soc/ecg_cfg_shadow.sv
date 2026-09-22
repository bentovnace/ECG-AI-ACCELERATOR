// Shadow-latch a configuration field per RUN.
//
// ITEM EXEC-04. The coprocessor reads configuration fields AFTER start cycle:
//   ecg_coproc.sv:396  `in_len_i`   -- in C_BASE, one cycle AFTER C_IDLE
//   ecg_coproc.sv:455  `n_layers_i` -- in C_NEXT, after a layer completes
//   ecg_coproc.sv:455  `single_i`   -- same state
// They arrive COMBINATIONALLY from register block, so host writes MID-RUN
// would alter behavior of the ACTIVE run. This block holds them stable.
//
// `layer_i` and `dma_len_i` are NOT shadowed: they are ALREADY latched by consumers
//   (`ecg_coproc.sv:381 layer_q <= ...` and `ecg_wmem.sv:83 wr_words_q <= ...`),
//   both on the exact start cycle. Re-shadowing would add redundant flops.
//
//
// WHY `held_o` PASSES THROUGH DIRECTLY IN START CYCLE: `ecg_coproc` samples config
//   IMMEDIATELY when `start_i` rises (C_IDLE). A purely registered output would update
//   one cycle LATER, providing the PREVIOUS run's value. Hence pass-through on start.
//
//
// WHY INSTANTIATED IN `ecg_soc` INSTEAD OF `ecg_mmio`: TWO control sources
//   (MMIO and CVXIF) exist and `ecg_soc.sv` selects between them (default CVXIF).
//   A latch inside `ecg_mmio` would only trigger on MMIO CTRL writes, not CVXIF.
//   `cp_start` is the COMMON trigger for both paths.
module ecg_cfg_shadow #(
    parameter int W = 1
) (
    input  logic         clk_i,
    input  logic         rst_ni,
    input  logic         start_i,   // start pulse for an execution run
    input  logic [W-1:0] live_i,    // combinational value from register block
    output logic [W-1:0] held_o     // value held for ACTIVE run
);
  logic [W-1:0] held_q;

  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni)      held_q <= '0;
    else if (start_i) held_q <= live_i;
  end

  assign held_o = start_i ? live_i : held_q;
endmodule

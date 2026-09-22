// Checks invariants of ecg_pkg: catches errors that documentation cannot verify.
//
// Invariants verified here are stated in documentation, but documentation is not executable.
// Widening a field without narrowing `resv` breaks the 16 B record size and shifts
// all table offsets silently.
//
// Run via: make lint
// Note: do not place linter tool names at the start of comment lines in this file.
//

module ecg_pkg_check;
  import ecg_pkg::*;

  // 16 B per record is the basis of all memory budgets. Following ADR-0014 §2.1
  // (CONCAT no longer has dedicated record) record count is 48, so 48 x 16 = 768 B --
  // measured via `make n7` from real blob. Changing record size alters the budget.
  //
  //
  initial begin
    if ($bits(ecg_layer_desc_t) != 8 * ECG_DESC_BYTES)
      $error("ecg_layer_desc_t %0d bits, must be %0d", $bits(ecg_layer_desc_t),
             8 * ECG_DESC_BYTES);
    if ($bits(ecg_model_desc_t) != 8 * ECG_DESC_BYTES)
      $error("ecg_model_desc_t %0d bits, must be %0d", $bits(ecg_model_desc_t),
             8 * ECG_DESC_BYTES);

    // Three special codes of src field must lie outside buffer index range.
    // Explicit typecast: 4-bit src codes compared with int ECG_N_BUFFER.
    //
    //
    if (int'(ECG_SRC_NONE) < ECG_N_BUFFER || int'(ECG_SRC_IN) < ECG_N_BUFFER
        || int'(ECG_SRC_RR) < ECG_N_BUFFER)
      $error("special src code overlaps buffer range 0..%0d", ECG_N_BUFFER - 1);

    // Peak buffers actually required (4) must fit allocated buffer range.
    if (ECG_PEAK_BUFFER_USED > ECG_N_BUFFER)
      $error("need %0d buffers, only %0d available", ECG_PEAK_BUFFER_USED, ECG_N_BUFFER);

    // Peak activation budget must fit 4 kB buffer from ADR-0002 §4.1.
    if (ECG_PEAK_ACT_BYTES > 4096)
      $error("peak activation %0d B exceeds 4 kB", ECG_PEAK_ACT_BYTES);

    // Exactly one instruction blocks core: store.barrier (isa.md §4.3).
    // If compute instructions blocked, non-blocking execution model would collapse.
    //
    if (!ecg_op_blocking(ECG_STORE, ECG_CLASS_CTRL))
      $error("store.barrier must block core");
    if (ecg_op_blocking(ECG_CONV1D, ECG_CLASS_COMPUTE)
        || ecg_op_blocking(ECG_LOADW, ECG_CLASS_COMPUTE))
      $error("compute instructions and LOADW must not block core");
  end
endmodule

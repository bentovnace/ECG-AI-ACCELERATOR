// Layer descriptor decode: 16 bytes in, control out.
//
// The unpack itself is one bit cast, because ecg_layer_desc_t is a packed struct
// whose field order is the compiler's field order and whose 13 reserved bits sit
// at the bottom. That is deliberate and it is the point of this module rather
// than a weakness: the ONE thing that can go wrong here is the struct in
// ecg_pkg.sv drifting from the field list in tools/compile_model.py, and that has
// already happened once -- pack_layer moved the reserved field from 14 bits to 13
// and unpack_layer kept shifting by 14, which put every field off by one bit and
// turned cin = 0 into groups = 0. A silent, total corruption from a one-bit edit.
//
// So the testbench does not check this module against a hand-written model. It
// checks it against the actual bytes tools/compile_model.py emits for the 54 real
// layers of the four models, field by field. The two implementations are in
// different languages and neither was derived from the other; if the struct order
// drifts again, that test is what fails.
//
// No products are computed here. cout x len_out and cin x k are what the
// sequencer needs, but a multiply in this module would infer a DSP48E1 the way
// out_pos x stride did in ecg_addrgen (checklist C3), and the nested counters
// that walk those products live in the sequencer anyway. This module hands over
// the operands and the booleans, nothing more.

`ifndef ECG_DESC_SV
`define ECG_DESC_SV

module ecg_desc
  import ecg_pkg::*;
(
    // 16 B theo thu tu byte LON TRUOC, dung nhu blob ghi ra dia.
    input  logic [ECG_DESC_BYTES*8-1:0] word_i,
    input  logic                        valid_i,

    // Tung truong mot, khong phai mot struct. Sequencer tieu thu tung truong,
    // va mot cong struct o bien module se buoc moi ben tieu thu tu lam lai phep
    // tinh bit -- dung thu module nay ton tai de tap trung vao mot cho.
    output logic [4:0]                  op_o,
    output logic [1:0]                  act_o,
    output logic [3:0]                  src0_o,
    output logic [3:0]                  src1_o,
    output logic [3:0]                  dst_o,
    output logic [9:0]                  dst_off_o,
    output logic [8:0]                  cin_o,
    output logic [8:0]                  cout_o,
    output logic [9:0]                  len_in_o,
    output logic [9:0]                  len_out_o,
    output logic [2:0]                  k_o,
    output logic [1:0]                  stride_o,
    output logic [2:0]                  pad_o,
    output logic                        dw_o,
    output logic [13:0]                 w_base_o,
    output logic [13:0]                 rq_base_o,
    output logic [8:0]                  rq_n_o,
    output logic                        last_o,
    output logic                        cat_part_o,
    output logic [12:0]                 buf_base_o,

    // ---- Giai ma cho sequencer.
    output logic                        is_mac_o,    // dung mang PE
    output logic                        is_pool_o,
    output logic                        is_gap_o,
    output logic                        is_add_o,
    output logic                        is_store_o,
    output logic                        uses_src1_o, // ADD skip, CONCAT nhanh 2
    output logic                        per_ch_o,    // rq_n > 1
    output logic                        relu_o,
    output logic                        legal_o
);

  // Mot phep gan bit. Xem chu thich dau file ve vi sao day la cho DE nhat de
  // sai va la ly do bo kiem thu doi chieu voi byte that cua bo bien dich.
  ecg_layer_desc_t d;
  assign d = ecg_layer_desc_t'(word_i);

  assign op_o       = d.op;
  assign act_o      = d.act;
  assign src0_o     = d.src0;
  assign src1_o     = d.src1;
  assign dst_o      = d.dst;
  assign dst_off_o  = d.dst_off;
  assign cin_o      = d.cin;
  assign cout_o     = d.cout;
  assign len_in_o   = d.len_in;
  assign len_out_o  = d.len_out;
  assign k_o        = d.k;
  assign stride_o   = d.stride;
  assign pad_o      = d.pad;
  assign dw_o       = d.dw;
  assign w_base_o   = d.w_base;
  assign rq_base_o  = d.rq_base;
  assign rq_n_o     = d.rq_n;
  assign last_o     = d.last;
  assign cat_part_o = d.cat_part;
  assign buf_base_o = d.buf_base;

  logic [4:0] op;
  assign op = d.op;

  assign is_mac_o   = (op == 5'(ECG_CONV1D)) || (op == 5'(ECG_DWCONV))
                   || (op == 5'(ECG_PWCONV)) || (op == 5'(ECG_FC));
  assign is_pool_o  = (op == 5'(ECG_MAXPOOL));
  assign is_gap_o   = (op == 5'(ECG_GAP));
  assign is_add_o   = (op == 5'(ECG_ADD));
  assign is_store_o = (op == 5'(ECG_STORE));

  // src1 co nghia o dung hai cho: duong skip cua ADD, va nhanh thu hai cua
  // CONCAT. Moi noi khac truong nay mang `ECG_SRC_NONE`.
  //
  // CHU THICH TRUOC O DAY GHI "moi noi khac truong nay la 0" VA DO LA SAI -- do
  // duoc tren 44 lop that: 42 lop KHONG dung src1 deu mang **13**, khong 0.
  // `compile_model.py:69` dat `PH_NONE = -3`, va -3 trong mot truong 4 bit la 13,
  // dung bang `ECG_SRC_NONE` o ecg_pkg.sv:144. Neu ai cuong che dieu kien NHU DA
  // VIET (`src1 == 0`) thi 42/44 lop that bi TU CHOI. Mot rang buoc phat bieu sai
  // nguy hon mot rang buoc khong duoc phat bieu: cai sau chi thieu, cai truoc MOI
  // nguoi di cai dat no.
  assign uses_src1_o = is_add_o || (op == 5'(ECG_CONCAT));

  assign per_ch_o = d.rq_n > 9'd1;
  assign relu_o   = d.act == 2'(ECG_ACT_RELU);

  // ---- Tinh hop le. Bon dieu kien nay deu la dieu bo bien dich BAO DAM, nen
  // mot vi pham khong phai du lieu xau ma la blob sai hoac giai ma lech bit --
  // dung thu can bat som chu khong de mang PE chay tren rac.
  // Cac dieu kien duoi day KHONG doan: chung do tu 44 lop that cua bon ho
  // (xem tools/rtl_ref/desc_ref.py). Ban dau o day kiem k in {1,2,3,5,7} cho
  // MOI phep, va no ban ngay tren lop GAP dau tien -- k chi co nghia voi cac
  // phep co nhan, con GAP va ADD dung k = 0.
  logic uses_k;
  assign uses_k = is_mac_o || is_pool_o;

  logic k_ok, stride_ok, rq_ok, op_ok;
  assign k_ok = uses_k ? ((d.k == 3'd1) || (d.k == 3'd2) || (d.k == 3'd3)
                       || (d.k == 3'd5) || (d.k == 3'd7))
                       : (d.k == 3'd0);
  assign stride_ok = (d.stride == 2'd1) || (d.stride == 2'd2);

  // rq_n theo phep, va day la cho chat nhat trong ca module:
  //   conv/fc/gap : 1 khi per-tensor, cout khi per-channel
  //   ADD         : DUNG 2 -- mot thang cho moi toan hang, vi hai duong vao
  //                 khac thang nhau va phai dua ve cung thang truoc khi cong
  //   MAXPOOL     : 0 -- phep so sanh khong requant
  // Mot gia tri khac la dau hieu lech bit, va chinh truong nay se sai neu
  // resv doi be rong -- do la loi da tung xay ra that.
  always_comb begin
    if (is_add_o)          rq_ok = (d.rq_n == 9'd2);
    else if (is_pool_o)    rq_ok = (d.rq_n == 9'd0);
    else if (is_mac_o || is_gap_o)
                           rq_ok = (d.rq_n == 9'd1) || (d.rq_n == d.cout);
    else                   rq_ok = 1'b1;
  end

  // F05: a whitelist, not a bound. `op < ECG_N_OPCODE` accepted five opcodes
  // with no execution path -- see ecg_op_is_layer in ecg_pkg.sv.
  // MUC DESC-07: WHITELIST, khong mot CAN. Xem chinh dong duoi va ecg_pkg.sv:113.
  assign op_ok = ecg_op_is_layer(op);

  // ---- BON DIEU KIEN PHAM VI, them 2026-09-04 (P0-05 cua danh gia ngoai).
  //
  // TRUOC BAN NAY `legal_o` chi mang bon dieu kien HINH DANG (op/k/stride/rq_n)
  // va KHONG mot dieu kien PHAM VI nao. Phep kiem nen bo dem CO ton tai nhung
  // no la mot `$error` trong `ifndef SYNTHESIS` (xem duoi) -- tuc no im lang
  // tren silicon va khong gate gi ca. `ecg_coproc.sv:401` TU CHOI descriptor
  // khi `!d_legal`, nen bon dieu kien duoi day la thu bien phep kiem do thanh
  // mot rao AN TOAN BO NHO chu khong mot phep kiem hinh dang.
  //
  // MOI NGUONG DUOI DAY DO TU 44 LOP THAT, khong doan (xem bang cuoi tep va
  // tools/rtl_ref/desc_ref.py). Hai cho suyt sai neu doan:
  //   * `buf_base + cout*len_out` cua 8 trong 44 lop bang DUNG 6144, tuc dung
  //     bien. Mot dieu kien `<` se TU CHOI tam lop that.
  //   * sum of `w_base + w_extent` for each family equals EXACT declared weight bytes
  //     (4572 / 4564 / 4656 / 4548), so formula below is not an estimation -- it
  //     reproduces exact compiler layout.
  //
  // UNMEASURED COST: three multipliers (`cout*len_out`, `cin*cout*k`, `cout*k`)
  // are new hardware on combinational path of `legal_o`. They run ONCE per layer
  // (~3000 cycles) and could be time-multiplexed, but in this form are parallel logic.
  // Area will be verified in ASIC synthesis.
  // 
  logic is_compute, dst_fit, w_fit, rq_fit, nz_ok, mac_fit;

  // Seven opcodes have compute datapaths; five opcodes (CONCAT, REQUANT, RELU,
  // LOADW, STORE) do not. `dst_off`/`cout` for a control instruction have no
  // geometric meaning, so bound checks apply solely to compute layers.
  assign is_compute = is_mac_o || is_pool_o || is_gap_o || is_add_o;

  // DESTINATION buffer must lie within activation memory. `<=` rather than `<`: 8 real layers
  // end EXACTLY at final byte.
  // `dst_off` MUST be included in sum (F04). It is a CHANNEL offset, not byte offset,
  // so byte offset is `dst_off * len_out` -- `ecg_seq.sv:116-119` records an earlier bug
  // using raw `dst_off`, where m4 CONCAT branch wrote from byte 10 instead of 640.
  // 
  //
  // Without it, an out-of-bounds CONCAT descriptor remains valid:
  //   buf_base=0, dst_off=91, cout=6, len_out=64
  //   old formula  ->  0 + 6*64        =   384  <= 6144  VALID
  //   actual range ->  0 + (91+6)*64   =  6208  >  6144  exceeds by 64 bytes
  //
  // RETAIN `<=`, do not change to `<`: 8 of 44 real layers end at EXACTLY 6144.
  // The new formula reproduces compiler layout: all 8 layers with `dst_off != 0`
  // end at EXACTLY 6144, rejecting 0/44 valid layers.
  // 
  // DESC-01: products widened to 32 bits before multiply. DESC-02: includes
  // `dst_off` (concat offset) and `len_out` (actual output size).
  // Note on DESC-01: BIAS region has no range check in `legal_o`.
  assign dst_fit = !is_compute
                || ((32'(d.buf_base)
                     + (32'(d.dst_off) + 32'(d.cout)) * 32'(d.len_out))
                    <= 32'(ECG_ACT_BYTES));

  // TERMS accumulated into ONE psum must fit 24-bit accumulator (F06).
  //
  // DERIVATION FROM SOURCES:
  //   `ECG_PSUM_BITS = 24` (ecg_pkg.sv:192) -> max signed = 2^23-1 = 8,388,607
  //   `act_i`/`wgt_i` are `logic signed [7:0]` (ecg_mac8.sv:45,51) -> HARDWARE range
  //   is [-128, +127], with line 62 specifying "-128 * -128 = 16,384".
  //   Max product is 16,384 and positive.
  //     8,388,607 / 16,384 = 511.99  ->  N <= 511
  //     512 x 16,384 = 8,388,608, overflows by EXACTLY ONE unit. Bound of 511
  //     is tight, not arbitrary safety rounding.
  //
  // WHY NOT 520: quantize.py clamps to ±127 so quantizer never emits -128, yielding
  // max product 16,129 and N <= 520. However NOTHING IN HARDWARE enforces ±127: datapath
  // accepts -128, and handcrafted blobs or altered quantizers could inject -128.
  // Thus bound is enforced according to hardware domain.
  //   (`ecg_mac8.sv:96` notes ±127 bound in comments).
  // 
  // 
  // 
  //
  // N = terms per psum. `ecg_addrgen.sv:24-25`: `tap 0..k-1` and `in_ch 0..cin-1`;
  // `in_ch` skipped when `dw = 1`. Hence N = dw ? k : cin*k.
  // In 44 real layers: max N is 210 (m4/6 CONV1D); bound of 511 rejects 0/44 layers with 2.4x margin.
  // 
  // DWCONV is constant true for N <= 511: N = k, where k is a 3-bit field (k <= 7 <= 511).
  // Writing `d.dw ? 32'(d.k) : ...` would generate hardware for a condition that is always true.
  // 
  // 
  // 
  assign mac_fit = !is_mac_o || d.dw
                || (32'(d.cin) * 32'(d.k) <= 32'(ECG_MAC_TERMS_MAX));

  // WEIGHT region must reside within wmem. DWCONV uses one input channel per
  // output channel, requiring cout*k instead of cin*cout*k.
  assign w_fit = !is_mac_o
              || ((32'(d.w_base) + (d.dw ? 32'(d.cout) * 32'(d.k)
                                         : 32'(d.cin) * 32'(d.cout) * 32'(d.k)))
                  <= 32'(ECG_WMEM_BYTES));

  // Requant scale table. Evaluated unconditionally: for MAXPOOL (rq_n = 0)
  // simplifies to `rq_base <= 256`, satisfied by all 44 real layers.
  assign rq_fit = (32'(d.rq_base) + 32'(d.rq_n)) <= 32'(ECG_N_REQUANT_SCALE);

  // Zero-sized layer does not error -- it is SILENTLY SKIPPED, returning
  // outputs from previous layer.
  // NUM-05: formal proof in `ecg_addrgen` has `assume (pad_i < k_i)` (line 208),
  // which previously was unconstrained in pipeline.
  // Across 38 real layers: 0 layers have pad >= k; check rejects 0 real layers.
  // 
  // 
  // 
  logic pad_ok;
  assign pad_ok = !uses_k || ({1'b0, d.pad} < {1'b0, d.k});

  assign nz_ok = !is_compute
              || ((d.cout != '0) && (d.len_out != '0)
                  && (d.cin != '0) && (d.len_in != '0));

  // DESC-10: `legal_o` constrains 8/8 fields -- kernel, stride, rq_n,
  // shape, src1 + dw, AND src0 + dst. Remaining field:
  //   act        validated at compiler level, as 2-bit field is fully populated.
  // 
  // 
  // Previous constraints were documented only in comments.
  // `src1` meaningful only for ADD and CONCAT; otherwise `ECG_SRC_NONE`.
  // `dw` meaningful only for DWCONV; `dw = 1` on CONV1D would widen `w_fit` and
  // `mac_fit` calculations erroneously.
  // 
  logic src1_ok, dw_ok;
  assign src1_ok = uses_src1_o || (d.src1 == ECG_SRC_NONE);
  assign dw_ok   = !d.dw || (op == 5'(ECG_DWCONV));

  // FINAL TWO FIELDS. Previously `src0` and `dst` passed through unvalidated.
  //
  // Mask selects ops where fields have defined meaning; avoids raw `is_compute`
  // which misses CONCAT (op 6) that reads `src0` and writes `dst`.
  // 
  //
  // Verified against 57 legal combinations: `src0` in {0,1,2,3,14,15} and `dst`
  // in {0,1,2,3,4}. Rejects 0 real layers.
  // 
  // 
  // 
  // 
  logic writes_buf, reads_src0, src0_ok, dst_ok;
  assign writes_buf = is_compute || (op == 5'(ECG_CONCAT));
  assign reads_src0 = writes_buf || is_store_o;
  // `src0` must be a READABLE source: 0..12 physical buffers, 14/15 input channels.
  // Only 13 (ECG_SRC_NONE) is invalid when src0 is read.
  assign src0_ok    = !reads_src0 || (d.src0 != ECG_SRC_NONE);
  // `dst` must be a WRITABLE buffer (< ECG_N_BUFFER, excluding 14 and 15 input ports).
  // 
  // 
  assign dst_ok     = !writes_buf || (32'(d.dst) < 32'(ECG_N_BUFFER));

  assign legal_o = op_ok && k_ok && stride_ok && rq_ok && pad_ok
                && src1_ok && dw_ok && src0_ok && dst_ok
                && dst_fit && w_fit && rq_fit && nz_ok && mac_fit;

`ifndef SYNTHESIS
  always_comb begin
    if (valid_i && !op_ok)
      $error("ecg_desc: op = %0d >= %0d opcode", op, ECG_N_OPCODE);
    if (valid_i && !k_ok)
      $error("ecg_desc: k = %0d sai voi op = %0d", d.k, op);
    if (valid_i && !stride_ok)
      $error("ecg_desc: stride = %0d khong thuoc {1,2}", d.stride);
    if (valid_i && !src0_ok)
      $error("ecg_desc: src0 = %0d la ECG_SRC_NONE tren op = %0d, ma op nay CO doc src0",
             d.src0, op);
    if (valid_i && !dst_ok)
      $error("ecg_desc: dst = %0d khong phai bo dem ghi duoc (can < %0d) tren op = %0d",
             d.dst, ECG_N_BUFFER, op);
    if (valid_i && !rq_ok)
      $error("ecg_desc: rq_n = %0d sai voi op = %0d (cout = %0d)",
             d.rq_n, op, d.cout);
    // Base must reside in buffer. An out-of-bounds base indicates bit misalignment --
    // lowest field of 16-byte struct, misaligning first.
    if (valid_i && (32'(d.buf_base) >= 32'(ECG_ACT_BYTES)))
      $error("ecg_desc: buf_base = %0d >= %0d B", d.buf_base, ECG_ACT_BYTES);
    if (valid_i && !dst_fit)
      $error("ecg_desc: vung dich %0d + %0d*%0d vuot bo dem %0d B",
             d.buf_base, d.cout, d.len_out, ECG_ACT_BYTES);  // xem dst_off o dst_fit
    if (valid_i && !w_fit)
      $error({"ecg_desc: vung trong so %0d + tam(cin=%0d,cout=%0d,k=%0d,",
              "dw=%0b) vuot wmem %0d B"},
             d.w_base, d.cin, d.cout, d.k, d.dw, ECG_WMEM_BYTES);
    if (valid_i && !rq_fit)
      $error("ecg_desc: rq_base = %0d + rq_n = %0d vuot bang thang %0d",
             d.rq_base, d.rq_n, ECG_N_REQUANT_SCALE);
    if (valid_i && !nz_ok)
      $error({"ecg_desc: lop tinh toan co kich thuoc 0 (cin=%0d cout=%0d ",
              "len_in=%0d len_out=%0d) -- se bi BO QUA IM LANG"},
             d.cin, d.cout, d.len_in, d.len_out);
  end
`endif

`ifdef FORMAL
  // Strongest invariant for this module: 20 unpacked fields reconstruct EXACT
  // original 128-bit word. Proves decode is non-overlapping and lossless.
  // 
  // 
  //
  // Golden vectors cross-check on finite samples; formal assertion proves for all 2^128 inputs.
  // 
  always_comb begin
    assert ({op_o, act_o, src0_o, src1_o, dst_o, dst_off_o, cin_o, cout_o,
             len_in_o, len_out_o, k_o, stride_o, pad_o, dw_o, w_base_o,
             rq_base_o, rq_n_o, last_o, cat_part_o, buf_base_o} == word_i);

    // `legal_o` never asserts if opcode exceeds maximum defined opcodes.
    // 
    assert (!legal_o || (op_o < 5'(ECG_N_OPCODE)));

    // EXACT PARTITIONING over 12 opcodes (not `<= 1`).
    //
    // `<= 1` allows 0, failing to ensure compute opcodes activate a path.
    // 
    // 
    // 
    // 
    //
    // Seven compute opcodes (CONV1D DWCONV PWCONV FC MAXPOOL GAP ADD) must activate
    // EXACTLY ONE path; five non-compute opcodes activate ZERO compute paths.
    // 
    if (op_o < 5'(ECG_N_OPCODE)) begin
      if ((op_o == 5'(ECG_CONV1D)) || (op_o == 5'(ECG_DWCONV))
       || (op_o == 5'(ECG_PWCONV)) || (op_o == 5'(ECG_FC))
       || (op_o == 5'(ECG_MAXPOOL)) || (op_o == 5'(ECG_GAP))
       || (op_o == 5'(ECG_ADD)))
        assert ($countones({is_mac_o, is_pool_o, is_gap_o, is_add_o}) == 1);
      else
        assert ($countones({is_mac_o, is_pool_o, is_gap_o, is_add_o}) == 0);
    end

    // `legal_o` must imply every range constraint. Stated individually
    // so counterexamples identify which specific condition failed.
    assert (!legal_o || dst_fit);
    assert (!legal_o || w_fit);
    assert (!legal_o || rq_fit);
    assert (!legal_o || nz_ok);
  end
`endif

endmodule

`endif  // ECG_DESC_SV

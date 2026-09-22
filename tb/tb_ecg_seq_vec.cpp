// Verilator harness for ecg_seq_vec (MAXPOOL, GAP, ADD).
//
// Vectors come from tools/rtl_ref/seq_vec_ref.py: per-channel numpy, requant by
// int_ref.requant, write offsets from the spec formula. Nothing there models the
// sequencer's loop nest.
//
// The harness models one activation read port and one scale table. Two things it
// must get right, because getting either wrong would blame the RTL:
//
//   * ADD reads its two operands on consecutive cycles from DIFFERENT buffers
//     (src0 then src1), so the harness serves whichever buffer a_buf_o names.
//   * The scale index moves for ADD (rq_base + operand) and is fixed for GAP.
//     Serving a single scale passes every GAP layer and fails only ADD.
//
// Three properties the vector set is built to catch: an all-negative pooling
// window at a padded position (zero injection would return 0, skipping the tap
// returns the true max), ADD with ReLU (which must apply after the sum, not to
// each operand), and ADD's two distinct scales.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <vector>

#include "Vecg_seq_vec.h"
#include "verilated.h"
#include "ecg_cov.h"

namespace {

constexpr int MAXPOOL = 3, GAP = 4, ADD = 5;

Vecg_seq_vec *dut = nullptr;

void tick() {
  dut->clk_i = 0;
  dut->eval();
  dut->clk_i = 1;
  dut->eval();
}

struct Case {
  int op, act, ch, len_in, len_out, k, stride, pad, dst_off;
  std::vector<int> x, y;
  std::vector<std::pair<int, int>> sc;
  std::vector<std::pair<int, int>> writes;
};

bool rdvec(FILE *fp, std::vector<int> &v) {
  int n = 0;
  if (std::fscanf(fp, "%d", &n) != 1) return false;
  v.resize(n);
  for (int i = 0; i < n; ++i) {
    if (std::fscanf(fp, "%d", &v[i]) != 1) return false;
  }
  return true;
}

const char *mname(int m) {
  return m == MAXPOOL ? "MAXPOOL" : (m == GAP ? "GAP" : "ADD");
}

}  // namespace

double sc_time_stamp() { return 0; }

// Moi `return 2` trong duong PHAN TICH phai NOI RA cho nao hong. Truoc do chung
// im lang: `fscanf` that bai roi `return 2` khong in gi, nen mot tep vector bi
// CAT CUT lam `make sim-seq_vec` do voi KHONG MOT DONG dau ra va nguoi doc phai di
// doc ma nguon. Chot bao ve dung truong hop TU BAO MINH ("khong mo duoc", co
// thong bao) va bo truong hop IM LANG.
#define DOC_HONG(what) do {                                                    \
    std::printf("tb_ecg_seq_vec: doc HONG tep vector %s\n"                         \
                "  dang doc: %s  (tb_ecg_seq_vec.cpp:%d)\n"                       \
                "  Tep bi CAT CUT hay sai dinh dang. Sinh lai:\n"              \
                "    python3 tools/rtl_ref/seq_vec_ref.py\n",                              \
                path, (what), __LINE__);                                       \
    return 2; } while (0)

int main(int argc, char **argv) {
  Verilated::commandArgs(argc, argv);

  const char *path = (argc > 1) ? argv[1] : "40-rtl/tb/vectors/seq_vec.txt";
  FILE *fp = std::fopen(path, "r");
  if (!fp) {
    std::printf("tb_ecg_seq_vec: khong mo duoc %s (chay `python3 "
                "tools/rtl_ref/seq_vec_ref.py`)\n", path);
    return 2;
  }
  // ── MUC ORACLE-01: SO TENSOR BAT BUOC DOI CHIEU ──────────────────────────
  // `ncase` doc TU CHINH TEP VECTOR, tuc so luot doi chieu va du lieu doi chieu
  // den TU MOT NGUON. Mot tep khai 48 ca va mang 48 ca se di qua IM LANG: vong
  // lap chay du 48 lan, moi lan khop, va bang ket qua doc nhu mot lan chay day
  // du. Thieu mot tensor khong lam do cai gi.
  //
  // Bon con so duoi day la NGUON THU HAI, nam trong ma phep thu chu khong trong
  // du lieu phep thu. Chung den tu `seq_vec_ref.py` (no in "49 lop (19 MAXPOOL,
  // 16 GAP, 14 ADD)"), va neu bo sinh vector doi thi CHUNG PHAI DUOC SUA TAY --
  // do la chu y: mot thay doi ve pham vi doi chieu phai di qua mot nguoi.
  constexpr int N_CA_MONG_DOI   = 49;
  constexpr int N_POOL_MONG_DOI = 19;
  constexpr int N_GAP_MONG_DOI  = 16;
  constexpr int N_ADD_MONG_DOI  = 14;

  int ncase = 0;
  if (std::fscanf(fp, "%d", &ncase) != 1) DOC_HONG("std::fscanf(fp, '%d', &ncase) != 1");
  if (ncase != N_CA_MONG_DOI) {
    std::printf("tb_ecg_seq_vec: FAIL -- tep vector khai %d ca, phep thu doi "
                "DUNG %d. Mot tensor thieu (hay them) khong duoc di qua im "
                "lang: so luot doi chieu va du lieu doi chieu phai den tu HAI "
                "nguon.\n", ncase, N_CA_MONG_DOI);
    return 1;
  }

  dut = new Vecg_seq_vec;
  dut->rst_ni = 0;
  dut->start_i = 0;
  for (int i = 0; i < 3; ++i) tick();
  dut->rst_ni = 1;
  tick();

  int errors = 0, n_write = 0;
  int n_pool = 0, n_gap = 0, n_add = 0, n_relu = 0;

  for (int ci = 0; ci < ncase; ++ci) {
    Case c;
    if (std::fscanf(fp, "%d %d %d %d %d %d %d %d %d", &c.op, &c.act, &c.ch,
                    &c.len_in, &c.len_out, &c.k, &c.stride, &c.pad,
                    &c.dst_off) != 9) {
      DOC_HONG("header Case: op/act/ch/len_in/len_out/k/stride/pad/dst_off");
    }
    if (!rdvec(fp, c.x) || !rdvec(fp, c.y)) DOC_HONG("!rdvec(fp, c.x) || !rdvec(fp, c.y)");
    int n = 0;
    if (std::fscanf(fp, "%d", &n) != 1) DOC_HONG("std::fscanf(fp, '%d', &n) != 1");
    c.sc.resize(n);
    for (int i = 0; i < n; ++i) {
      if (std::fscanf(fp, "%d %d", &c.sc[i].first, &c.sc[i].second) != 2) {
        DOC_HONG("cap scale c.sc[i]");
      }
    }
    if (std::fscanf(fp, "%d", &n) != 1) DOC_HONG("std::fscanf(fp, '%d', &n) != 1");
    c.writes.resize(n);
    for (int i = 0; i < n; ++i) {
      if (std::fscanf(fp, "%d %d", &c.writes[i].first,
                      &c.writes[i].second) != 2) {
        DOC_HONG("cap write c.writes[i]");
      }
    }

    if (c.op == MAXPOOL) ++n_pool;
    else if (c.op == GAP) ++n_gap;
    else ++n_add;
    if (c.act) ++n_relu;

    dut->op_i = static_cast<uint8_t>(c.op);
    dut->act_i = static_cast<uint8_t>(c.act);
    dut->src0_i = 0;
    dut->src1_i = 1;
    dut->dst_i = 2;
    dut->dst_off_i = static_cast<uint16_t>(c.dst_off);
    dut->cout_i = static_cast<uint16_t>(c.ch);
    dut->len_in_i = static_cast<uint16_t>(c.len_in);
    dut->len_out_i = static_cast<uint16_t>(c.len_out);
    dut->k_i = static_cast<uint8_t>(c.k);
    dut->stride_i = static_cast<uint8_t>(c.stride);
    dut->pad_i = static_cast<uint8_t>(c.pad);
    dut->rq_base_i = 0;

    dut->start_i = 1;
    dut->a_gnt_i = 1;
    dut->a_valid_i = 0;
    tick();
    dut->start_i = 0;

    std::map<int, int> got;
    bool pend = false;
    int pend_buf = 0, pend_off = 0;
    long cycles = 0;
    const long cap = 400000;

    while (cycles < cap) {
      if (pend) {
        dut->a_valid_i = 1;
        const std::vector<int> &src = (pend_buf == 1) ? c.y : c.x;
        dut->a_data_i = (pend_off >= 0 &&
                         pend_off < static_cast<int>(src.size()))
                            ? static_cast<int8_t>(src[pend_off]) : 0;
      } else {
        dut->a_valid_i = 0;
        dut->a_data_i = 0;
      }

      dut->eval();
      {
        const int so = dut->s_off_o;
        const int si = (so >= 0 && so < static_cast<int>(c.sc.size())) ? so : 0;
        if (!c.sc.empty()) {
          dut->s_mult_i = static_cast<uint16_t>(c.sc[si].first);
          dut->s_shift_i = static_cast<uint8_t>(c.sc[si].second);
        } else {
          dut->s_mult_i = 0;
          dut->s_shift_i = 0;
        }
      }
      dut->eval();

      const bool req = dut->a_req_o != 0 && dut->a_gnt_i != 0;
      const int buf = dut->a_buf_o;
      const int off = dut->a_off_o;

      tick();

      if (dut->wr_o) {
        got[static_cast<int>(dut->wr_off_o)] =
            static_cast<int>(static_cast<int8_t>(dut->wr_data_o));
        ++n_write;
      }

      pend = req;
      pend_buf = (buf == 1) ? 1 : 0;
      pend_off = off;

      ++cycles;
      if (dut->done_o) break;
    }

    if (cycles >= cap) {
      std::printf("ca %d: khong ket thuc trong %ld chu ky\n", ci, cap);
      ++errors;
      continue;
    }

    std::map<int, int> want;
    for (const auto &wv : c.writes) want[wv.first] = wv.second;
    if (want != got) {
      if (++errors <= 6) {
        std::printf("ca %d %s (act=%d ch=%d lin=%d lout=%d k=%d s=%d pad=%d): "
                    "%zu phep ghi, ky vong %zu\n", ci, mname(c.op), c.act, c.ch,
                    c.len_in, c.len_out, c.k, c.stride, c.pad, got.size(),
                    want.size());
        int shown = 0;
        for (const auto &wv : want) {
          const auto it = got.find(wv.first);
          if (it == got.end()) {
            if (shown++ < 6)
              std::printf("   off=%3d ky vong %4d, KHONG CO\n", wv.first,
                          wv.second);
          } else if (it->second != wv.second) {
            if (shown++ < 6)
              std::printf("   off=%3d ky vong %4d, nhan %4d\n", wv.first,
                          wv.second, it->second);
          }
        }
      }
    }

    dut->rst_ni = 0;
    tick();
    dut->rst_ni = 1;
    tick();
  }
  std::fclose(fp);

  dut->final();
  // ── MUC COMP-04: GAP per-channel co CHAY DUOC khong ──────────────────────
  // `ecg_desc.sv:146-147` nhan `rq_n == cout` cho GAP la HOP LE, tuc mo ta cho
  // phep mot GAP per-channel. Cau hoi cua muc la: chay no voi thang KHAC NHAU
  // giua cac kenh. Nhung `s_off_o` cua ecg_seq_vec (:188) la
  //     rq_base_q + (is_add ? s2_opnd_q : 0)
  // -- voi GAP thi so hang thu hai la 0 VO DIEU KIEN, nen chi so thang KHONG
  // DI theo kenh. Quet duoi day DO dieu do thay vi doc lai dong ma.
  //
  // Do bang: dem so chi so thang PHAN BIET ma may hoi trong mot luot. GAP voi
  // cout = 2 ma chi hoi MOT chi so thi khong the co hai thang khac nhau, va
  // moi vector "GAP per-channel" nao viet ra cung se dung mot thang cho ca hai
  // kenh -- im lang.
  //
  // DOI CHUNG la ADD: no dung CUNG duong `s_off_o` va PHAI hoi hai chi so. Neu
  // ADD cung chi hoi mot thi phep do nay hong, khong phai thiet ke thieu.
  {
    auto quet_thang = [&](int op, int ch, int len_in) {
      dut->rst_ni = 0; for (int i = 0; i < 3; ++i) tick(); dut->rst_ni = 1;
      dut->op_i = static_cast<uint8_t>(op);
      dut->act_i = 0; dut->src0_i = 0; dut->src1_i = 1; dut->dst_i = 2;
      dut->dst_off_i = 0;
      dut->cout_i = static_cast<uint16_t>(ch);
      dut->len_in_i = static_cast<uint16_t>(len_in);
      dut->len_out_i = (op == GAP) ? 1 : static_cast<uint16_t>(len_in);
      dut->k_i = 0; dut->stride_i = 1; dut->pad_i = 0; dut->rq_base_i = 0;
      dut->s_mult_i = 1; dut->s_shift_i = 0;
      dut->a_gnt_i = 1; dut->a_valid_i = 0;
      dut->start_i = 1; tick(); dut->start_i = 0;
      std::set<int> chi_so;
      bool pend = false;
      for (long c2 = 0; c2 < 20000; ++c2) {
        dut->a_valid_i = pend ? 1 : 0;
        dut->a_data_i = pend ? 1 : 0;
        dut->eval();
        chi_so.insert(static_cast<int>(dut->s_off_o));
        const bool req = dut->a_req_o != 0 && dut->a_gnt_i != 0;
        tick();
        pend = req;
        if (dut->done_o) break;
      }
      return chi_so;
    };
    const std::set<int> g = quet_thang(GAP, 2, 4);
    const std::set<int> a = quet_thang(ADD, 2, 4);
    std::printf("\n=== COMP-04: chi so thang PHAN BIET trong mot luot ===\n");
    std::printf("   GAP cout=2 : %zu chi so\n", g.size());
    std::printf("   ADD cout=2 : %zu chi so  (doi chung)\n", a.size());
    if (a.size() < 2) {
      std::printf("  CHOT COMP-04: DOI CHUNG HONG -- ADD cung chi hoi %zu chi "
                  "so, ma ADD thi CHAC CHAN dung hai thang. Phep do nay khong "
                  "nhin thay chi so thang di chuyen, nen no khong ket luan "
                  "duoc gi ve GAP.\n", a.size());
      ++errors;
    } else if (g.size() >= 2) {
      std::printf("   GAP CO hoi nhieu chi so -- per-channel chay duoc.\n");
    } else {
      std::printf("   PHAN DINH COMP-04: GAP cout=2 chi hoi MOT chi so thang, "
                  "trong khi ADD hoi %zu. `ecg_desc` NHAN `rq_n == cout` cho "
                  "GAP la hop le, nhung `ecg_seq_vec.sv:188` khong cho chi so "
                  "di theo kenh -- mot GAP per-channel se dung MOT thang cho "
                  "moi kenh, IM LANG. Duong hop le nhung khong thuc thi.\n",
                  a.size());
    }
  }

  ECG_COV_WRITE();
  delete dut;

  std::printf("tb_ecg_seq_vec: %d MAXPOOL, %d GAP, %d ADD (%d co ReLU), "
              "%d phep ghi\n", n_pool, n_gap, n_add, n_relu, n_write);
  if (n_pool == 0 || n_gap == 0 || n_add == 0 || n_relu == 0) {
    std::printf("tb_ecg_seq_vec: FAIL (bo vector khong phu het)\n");
    return 1;
  }
  // MUC ORACLE-01: khong chi "co it nhat mot cai moi loai" ma DUNG BAO NHIEU
  // cai moi loai. Phep kiem cu di qua ke ca khi 18/19 lop MAXPOOL bien mat.
  if (n_pool != N_POOL_MONG_DOI || n_gap != N_GAP_MONG_DOI ||
      n_add != N_ADD_MONG_DOI) {
    std::printf("tb_ecg_seq_vec: FAIL -- doi chieu %d MAXPOOL / %d GAP / %d ADD, "
                "phep thu doi %d / %d / %d\n", n_pool, n_gap, n_add,
                N_POOL_MONG_DOI, N_GAP_MONG_DOI, N_ADD_MONG_DOI);
    return 1;
  }
  if (errors == 0) {
    std::printf("tb_ecg_seq_vec: PASS (ba phep dung tung bit)\n");
    return 0;
  }
  std::printf("tb_ecg_seq_vec: FAIL (%d lop sai)\n", errors);
  return 1;
}

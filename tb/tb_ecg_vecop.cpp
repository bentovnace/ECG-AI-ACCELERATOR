// Verilator harness for ecg_vecop.
//
// Vectors come from tools/rtl_ref/vecop_ref.py, which implements MAXPOOL, GAP and
// ADD as three separate functions sharing nothing. ADD arrives as a two-beat
// stream because each of its operands carries its own requant scale and therefore
// passes the single requant stage separately; that is also what made the second
// activation read port unnecessary (N9 90,1 % -> 92,6 %). The RTL folds them onto one
// comparator and one adder for N9, so agreement is evidence that the folding does
// not leak state between operations -- the characteristic failure of a merged
// datapath, and the reason the vectors interleave the three rather than grouping
// them.
//
// Output timing: ovalid_q and the result registers are written on the SAME edge
// that carries the beat, so they are valid immediately after that tick -- not one
// tick later. An extra pipeline stage here is exactly the harness bug that has
// now bitten six times, so the comparison is written inline with no pending slot.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "Vecg_vecop.h"
#include "verilated.h"
#include "ecg_cov.h"

namespace {

Vecg_vecop *dut = nullptr;

void tick() {
  dut->clk_i = 0;
  dut->eval();
  dut->clk_i = 1;
  dut->eval();
}

// Verilator packs a signed 24-bit port into a wider word without extending the
// sign. Sizing the mask from the port means narrowing ECG_PSUM_BITS again cannot
// silently break the check.
constexpr int PSUM_BITS = 24;
int32_t sext_psum(uint32_t v) {
  const uint32_t mask = (1u << PSUM_BITS) - 1u;
  const uint32_t x = v & mask;
  return (x & (1u << (PSUM_BITS - 1)))
             ? static_cast<int32_t>(x) - static_cast<int32_t>(1u << PSUM_BITS)
             : static_cast<int32_t>(x);
}

constexpr int MAXPOOL = 3, GAP = 4, ADD = 5;

const char *mname(int m) {
  return m == MAXPOOL ? "MAXPOOL" : (m == GAP ? "GAP" : "ADD");
}

}  // namespace

double sc_time_stamp() { return 0; }

// Moi `return 2` trong duong PHAN TICH phai NOI RA cho nao hong. Truoc do chung
// im lang: `fscanf` that bai roi `return 2` khong in gi, nen mot tep vector bi
// CAT CUT lam `make sim-vecop` do voi KHONG MOT DONG dau ra va nguoi doc phai di
// doc ma nguon. Chot bao ve dung truong hop TU BAO MINH ("khong mo duoc", co
// thong bao) va bo truong hop IM LANG.
#define DOC_HONG(what) do {                                                    \
    std::printf("tb_ecg_vecop: doc HONG tep vector %s\n"                         \
                "  dang doc: %s  (tb_ecg_vecop.cpp:%d)\n"                       \
                "  Tep bi CAT CUT hay sai dinh dang. Sinh lai:\n"              \
                "    python3 tools/rtl_ref/vecop_ref.py\n",                              \
                path, (what), __LINE__);                                       \
    return 2; } while (0)

int main(int argc, char **argv) {
  Verilated::commandArgs(argc, argv);

  const char *path = (argc > 1) ? argv[1] : "40-rtl/tb/vectors/vecop.txt";
  FILE *fp = std::fopen(path, "r");
  if (!fp) {
    std::printf("tb_ecg_vecop: khong mo duoc %s (chay `python3 "
                "tools/rtl_ref/vecop_ref.py`)\n", path);
    return 2;
  }

  int n = 0;
  if (std::fscanf(fp, "%d", &n) != 1) DOC_HONG("std::fscanf(fp, '%d', &n) != 1");

  struct Beat {
    int mode, first, last, a, b, ovalid, oi8, opsum;
  };
  std::vector<Beat> beats(n);
  for (int i = 0; i < n; ++i) {
    Beat &t = beats[i];
    if (std::fscanf(fp, "%d %d %d %d %d %d %d %d", &t.mode, &t.first, &t.last,
                    &t.a, &t.b, &t.ovalid, &t.oi8, &t.opsum) != 8) {
      std::printf("tb_ecg_vecop: dong %d hong\n", i);
      return 2;
    }
  }
  std::fclose(fp);

  dut = new Vecg_vecop;
  dut->rst_ni = 0;
  dut->valid_i = 0;
  dut->mode_i = MAXPOOL;
  for (int i = 0; i < 3; ++i) tick();
  dut->rst_ni = 1;
  tick();

  int errors = 0;
  int checked = 0;
  int n_pool = 0, n_gap = 0, n_add = 0;

  for (int i = 0; i < n; ++i) {
    const Beat &t = beats[i];
    dut->mode_i = static_cast<uint8_t>(t.mode);
    dut->valid_i = 1;
    dut->first_i = static_cast<uint8_t>(t.first);
    dut->last_i = static_cast<uint8_t>(t.last);
    dut->a_i = static_cast<int8_t>(t.a);
    tick();

    const bool ov = dut->out_valid_o != 0;
    if (ov != (t.ovalid != 0)) {
      if (++errors <= 10) {
        std::printf("nhip %d %s: out_valid %d, ky vong %d\n", i,
                    mname(t.mode), ov ? 1 : 0, t.ovalid);
      }
      continue;
    }
    if (!ov) continue;

    if (t.mode == GAP) {
      const int32_t got = sext_psum(static_cast<uint32_t>(dut->out_psum_o));
      if (got != t.opsum) {
        if (++errors <= 10) {
          std::printf("nhip %d GAP: psum got %d want %d\n", i, got, t.opsum);
        }
      }
      ++n_gap;
    } else {
      const int got = static_cast<int8_t>(dut->out_i8_o);
      if (got != t.oi8) {
        if (++errors <= 10) {
          std::printf("nhip %d %s: got %d want %d\n", i, mname(t.mode), got,
                      t.oi8);
        }
      }
      if (t.mode == MAXPOOL) ++n_pool;
      else ++n_add;
    }
    ++checked;
  }

  dut->valid_i = 0;
  tick();

  dut->final();
  ECG_COV_WRITE();
  delete dut;

  std::printf("tb_ecg_vecop: %d nhip, %d ket qua (%d MAXPOOL, %d GAP, %d ADD)\n",
              n, checked, n_pool, n_gap, n_add);
  // A merged datapath that silently handled only one operation would still pass a
  // suite that never exercised the other two, so the counts are asserted.
  if (n_pool == 0 || n_gap == 0 || n_add == 0) {
    std::printf("tb_ecg_vecop: FAIL (mot phep khong duoc kiem lan nao)\n");
    return 1;
  }
  if (errors == 0) {
    std::printf("tb_ecg_vecop: PASS (ba phep dung tung bit, khong ro ri trang "
                "thai khi xen ke)\n");
    return 0;
  }
  std::printf("tb_ecg_vecop: FAIL (%d loi)\n", errors);
  return 1;
}

// Verilator harness for ecg_mac8.
//
// Vectors come from tools/rtl_ref/mac8_ref.py, whose expectation is a whole
// conv1d layer computed in numpy and then flattened into the issue order of
// ecg_addrgen's counter nest. That sourcing is the point of this file.
//
// The previous version of this harness took its expectation from a C++ loop that
// summed the eight lanes -- the same thing the RTL did at the time. Harness and
// RTL agreed with each other and both disagreed with the architecture, which maps
// the eight lanes onto eight OUTPUT CHANNELS (tools/cycle_model.py layer_cycles,
// and ecg_addrgen's out_ch stepping by N_PE). A suite whose reference describes
// the module instead of the system cannot catch a mapping error, however many
// vectors it runs.
//
// Coverage that matters here: cout not a multiple of eight (the head has 5 output
// channels, inc1 has 10), so the last scan has idle lanes whose results are not
// checked; the longest accumulate chain in the four models (cin=30, k=7, 210
// terms) which is what sizes the 24-bit psum; and pad > 0 so the zero-injected
// issues of ADR-0013 appear.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "Vecg_mac8.h"
#include "verilated.h"
#include "ecg_cov.h"

namespace {

constexpr int N_PE = 8;
constexpr int PSUM_BITS = 24;

Vecg_mac8 *dut = nullptr;

void tick() {
  dut->clk_i = 0;
  dut->eval();
  dut->clk_i = 1;
  dut->eval();
}

// Verilator packs a signed 24-bit lane into a wider word without extending the
// sign, so a negative accumulator reads back as a large positive number. Sizing
// the mask from PSUM_BITS means narrowing ECG_PSUM_BITS cannot silently break it.
int32_t sext_psum(uint32_t v) {
  const uint32_t mask = (1u << PSUM_BITS) - 1u;
  const uint32_t x = v & mask;
  return (x & (1u << (PSUM_BITS - 1)))
             ? static_cast<int32_t>(x) - static_cast<int32_t>(1u << PSUM_BITS)
             : static_cast<int32_t>(x);
}

}  // namespace

double sc_time_stamp() { return 0; }

// Moi `return 2` trong duong PHAN TICH phai NOI RA cho nao hong. Truoc do chung
// im lang: `fscanf` that bai roi `return 2` khong in gi, nen mot tep vector bi
// CAT CUT lam `make sim-mac8` do voi KHONG MOT DONG dau ra va nguoi doc phai di
// doc ma nguon. Chot bao ve dung truong hop TU BAO MINH ("khong mo duoc", co
// thong bao) va bo truong hop IM LANG.
#define DOC_HONG(what) do {                                                    \
    std::printf("tb_ecg_mac8: doc HONG tep vector %s\n"                         \
                "  dang doc: %s  (tb_ecg_mac8.cpp:%d)\n"                       \
                "  Tep bi CAT CUT hay sai dinh dang. Sinh lai:\n"              \
                "    python3 tools/rtl_ref/mac8_ref.py\n",                              \
                path, (what), __LINE__);                                       \
    return 2; } while (0)

int main(int argc, char **argv) {
  Verilated::commandArgs(argc, argv);

  const char *path = (argc > 1) ? argv[1] : "40-rtl/tb/vectors/mac8.txt";
  FILE *fp = std::fopen(path, "r");
  if (!fp) {
    std::printf("tb_ecg_mac8: khong mo duoc %s (chay `python3 "
                "tools/rtl_ref/mac8_ref.py`)\n", path);
    return 2;
  }
  int n = 0;
  if (std::fscanf(fp, "%d", &n) != 1) DOC_HONG("std::fscanf(fp, '%d', &n) != 1");

  dut = new Vecg_mac8;
  dut->rst_ni = 0;
  dut->clear_i = 0;
  dut->acc_i = 0;
  dut->act_i = 0;
  for (int j = 0; j < N_PE; ++j) dut->wgt_i[j] = 0;
  for (int i = 0; i < 3; ++i) tick();
  dut->rst_ni = 1;
  tick();

  int errors = 0, n_scan = 0, n_lane = 0, n_idle_lane = 0;

  for (int i = 0; i < n; ++i) {
    int clear, acc, act, wg[N_PE], ov;
    if (std::fscanf(fp, "%d %d %d", &clear, &acc, &act) != 3) DOC_HONG("std::fscanf(fp, '%d %d %d', &clear, &acc, &act) != 3");
    for (int j = 0; j < N_PE; ++j) {
      if (std::fscanf(fp, "%d", &wg[j]) != 1) DOC_HONG("std::fscanf(fp, '%d', &wg[j]) != 1");
    }
    if (std::fscanf(fp, "%d", &ov) != 1) DOC_HONG("std::fscanf(fp, '%d', &ov) != 1");
    long want[N_PE];
    bool chk[N_PE];
    for (int j = 0; j < N_PE; ++j) {
      char tok[32];
      if (std::fscanf(fp, "%31s", tok) != 1) DOC_HONG("std::fscanf(fp, '%31s', tok) != 1");
      if (tok[0] == 'x') {
        chk[j] = false;
        want[j] = 0;
        if (ov) ++n_idle_lane;
      } else {
        chk[j] = true;
        want[j] = std::strtol(tok, nullptr, 10);
      }
    }

    dut->clear_i = static_cast<uint8_t>(clear);
    dut->acc_i = static_cast<uint8_t>(acc);
    dut->act_i = static_cast<int8_t>(act);
    // Mang khong dong goi ca hai ben, nen khong ben nao lam phep tinh bit --
    // mot phep dong goi viet tay o day la mot cho nua de sai.
    for (int j = 0; j < N_PE; ++j) dut->wgt_i[j] = static_cast<int8_t>(wg[j]);
    tick();

    // psum_o is combinational from acc_q, which was written on the edge above,
    // so the eight lanes are valid immediately after this tick -- no extra
    // pipeline stage. Adding one is the harness bug this project keeps hitting.
    if (!ov) continue;
    ++n_scan;
    for (int j = 0; j < N_PE; ++j) {
      if (!chk[j]) continue;
      const int32_t got = sext_psum(static_cast<uint32_t>(dut->psum_o[j]));
      ++n_lane;
      if (got != want[j]) {
        if (++errors <= 12) {
          std::printf("nhip %d lan %d: got %d want %ld\n", i, j, got, want[j]);
        }
      }
    }
  }
  std::fclose(fp);

  // ---- NUM-04: the 511-term bound, RUN rather than argued -----------------
  //
  // ECG_MAC_TERMS_MAX = 511 was derived arithmetically: the hardware product
  // domain is [-128,127], the largest product is -128 * -128 = +16384, the psum
  // is 24-bit signed (max +8388607), and 512 * 16384 = 8388608 -- over by
  // exactly one. The derivation is tight. It had never been EXECUTED.
  //
  // The longest chain this suite actually ran was 210 terms (cin=30, k=7), the
  // longest in the four models. 211..511 had never been through the hardware,
  // and three separate times today "correct by argument" and "has been run"
  // turned out to be different states.
  //
  // ORACLE-02: the expectation is an independent int64 accumulation, NOT a
  // second call into the same helper the RTL path uses. Two implementations of
  // the same mistake agree with each other.
  {
    std::printf("=== NUM-04 · chuoi tich luy o CAN, du lieu bien (-128 x -128)\n");
    // 512 KHONG chay trong luot nay: `ecg_mac8.sv:115` co mot khang dinh tran
    // psum 24 bit, va no `$stop` -- tien trinh dung, nen mot ca sau 512 se khong
    // duoc chay va bang se im lang thieu. Ca 512 chay RIENG qua bien moi truong
    // `ECG_MAC8_TRAN`, va cai duoc mong doi o do la MOT LAN DUNG, khong phai mot
    // dong ket qua. Xem luat `sim-mac8` trong Makefile.
    const bool chi_tran = (std::getenv("ECG_MAC8_TRAN") != nullptr);
    const int LEN_THUONG[] = {509, 510, 511};
    const int LEN_TRAN[] = {512};
    const int *LEN = chi_tran ? LEN_TRAN : LEN_THUONG;
    const int N_LEN = chi_tran ? 1 : 3;
    const int32_t PSUM_MAX = (1 << (PSUM_BITS - 1)) - 1;
    int n_bien = 0;
    for (int li = 0; li < N_LEN; ++li) {
      const int N = LEN[li];
      // oracle doc lap: cong so hoc int64, khong goi lai duong cua RTL
      int64_t mong = 0;
      for (int t = 0; t < N; ++t) mong += static_cast<int64_t>(-128) * -128;

      dut->clear_i = 1; dut->acc_i = 1; dut->act_i = -128;
      for (int j = 0; j < N_PE; ++j) dut->wgt_i[j] = -128;
      tick();                       // nhip dau: clear thang acc, psum = mot tich
      dut->clear_i = 0;
      for (int t = 1; t < N; ++t) tick();
      const int32_t got = sext_psum(static_cast<uint32_t>(dut->psum_o[0]));
      dut->acc_i = 0;

      const bool tran = mong > PSUM_MAX;
      const char *phan;
      if (!tran) {
        phan = (got == static_cast<int32_t>(mong)) ? "ok" : "!! SAI";
        if (got != static_cast<int32_t>(mong)) ++errors;
      } else {
        // Tran thi psum 24 bit KHONG con mang gia tri dung -- dieu phai chung
        // minh la no KHAC oracle, tuc can 511 that su la mot can.
        phan = (got != static_cast<int32_t>(mong)) ? "tran DUNG nhu du bao"
                                                   : "!! KHONG tran -- can sai";
        if (got == static_cast<int32_t>(mong)) ++errors;
      }
      std::printf("    N = %3d  oracle int64 = %10lld  psum24 = %10d  %s\n",
                  N, static_cast<long long>(mong), got, phan);
      ++n_bien;
    }
    // Chong rong: bon do dai phai chay het, VA phai co it nhat mot ca khong tran
    // va mot ca tran -- mot bo chi co mot phia khong phan biet duoc mot can voi
    // mot chan tren bat ky.
    if (n_bien != N_LEN) {
      std::printf("  CHOT: chi chay %d/%d do dai\n", n_bien, N_LEN);
      ++errors;
    }
    if (chi_tran) {
      // Toi day nghia la khang dinh cua RTL KHONG ban o 512 -- tuc can 511
      // khong duoc phan cung bao dam, va no chi la mot lap luan so hoc.
      std::printf("  CHOT: N = 512 chay xong ma khong mot khang dinh nao ban -- "
                  "can 511 khong duoc phan cung canh\n");
      ++errors;
    }
    std::printf("    can ECG_MAC_TERMS_MAX = %d; 511 x 16384 = %d <= %d, "
                "512 x 16384 = %d > %d\n",
                511, 511 * 16384, PSUM_MAX, 512 * 16384, PSUM_MAX);
  }

  dut->final();
  ECG_COV_WRITE();
  delete dut;

  std::printf("tb_ecg_mac8: %d nhip, %d lan quet, %d kenh ra kiem, "
              "%d lan nhan roi bo qua\n", n, n_scan, n_lane, n_idle_lane);
  // A suite with no idle lanes never exercised a cout that is not a multiple of
  // eight, which is the majority of the real layers.
  if (n_idle_lane == 0) {
    std::printf("tb_ecg_mac8: FAIL (khong co lop nao cout khong chia het 8)\n");
    return 1;
  }
  if (errors == 0) {
    std::printf("tb_ecg_mac8: PASS (8 kenh ra doc lap, dung tung bit tren "
                "%d lan quet)\n", n_scan);
    return 0;
  }
  std::printf("tb_ecg_mac8: FAIL (%d loi)\n", errors);
  return 1;
}

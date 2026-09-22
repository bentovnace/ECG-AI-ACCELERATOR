// Verilator harness for ecg_coproc -- this is the N2 test.
//
// Vectors come from tools/rtl_ref/coproc_ref.py, whose expectation is
// compile_model.run_blob: the interpreter that runs FROM THE BLOB, on real
// quantised DS2 beats. That is the integer path frozen in P4, and it is what N2
// means.
//
// The vector file is organised in FAMILY BLOCKS: descriptors, weights, scales and
// bias once per family, then N beats. This harness mirrors that -- reset and
// weight DMA once per family, then the beats run BACK TO BACK WITH NO RESET.
// That ordering is not just faster, it is a stronger test:
//
//   * It is what the real system does. Weights are resident (ADR-0015) and beats
//     arrive continuously; resetting between beats tested a mode that never runs.
//   * It checks the ADR-0013 invariant ACROSS beats. If a layer reads a byte that
//     the PREVIOUS beat wrote rather than this one, a per-beat-reset harness sees
//     nothing (the stale byte happens to be zero after reset) but this one sees
//     beat 1 pass and beat 2 onward fail -- so the beat index of the first
//     failure is itself the diagnosis.
//
// The harness plays the part of everything outside the coprocessor: the
// descriptor table, the requant scale table, the bias table, and the weight DMA
// source. Three details in there would blame the RTL if got wrong:
//
//   * Preloading the input goes through buffer id 14 with ABSOLUTE byte offsets
//     0..in_len+2. Before the first `start_i` the base register file is all zero,
//     so base_of(14) and base_of(15) are both 0 -- writing RR through buffer 15
//     at offset 0 would land on top of the ECG window. At start the coprocessor
//     programs base(14)=0 and base(15)=in_len, so the RR reads find their bytes
//     exactly where this preload put them. base(14) stays 0 afterwards, which is
//     why the preload for beat 2 and later is still correct with no reset.
//   * The weight DMA moves whole 64-bit words, so a weight region whose size is
//     not a multiple of eight is zero-padded up. ecg_wmem asserts both the
//     multiple-of-8 rule and the capacity, so a wrong length fails loudly.
//   * The bias table is FLAT for the whole model and b_off_o already carries the
//     per-layer base the coprocessor accumulates. Adding a base here too would
//     double it, and the result would look like a bias-scale bug.
//
// Comparison is by last write per address, not by the multiset of writes: a
// buffer byte is written more than once per inference because buffers are reused,
// and the value that matters is the last one. `got` is therefore cleared per
// beat, while the DUT state deliberately is not.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "Vecg_coproc.h"
#include "verilated.h"
#include "ecg_cov.h"

namespace {

Vecg_coproc *dut = nullptr;

void tick() {
  dut->clk_i = 0;
  dut->eval();
  dut->clk_i = 1;
  dut->eval();
}

struct Beat {
  std::vector<int> xq, rq;
  std::map<std::pair<int, int>, std::pair<int, int>> want;  // -> (gia tri, lop)
};

struct Fam {
  char stem[32];
  int n_layers, in_len;
  std::vector<std::string> desc;   // hex, 32 nibbles each
  std::vector<int> w, bias;
  std::vector<std::pair<int, int>> sc;
  std::vector<Beat> beats;
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

// The 128-bit descriptor port is an array of 32-bit words, word 0 the low bits.
void set_desc(const std::string &hex) {
  for (int i = 0; i < 4; ++i) {
    char buf[9];
    std::memcpy(buf, hex.c_str() + i * 8, 8);
    buf[8] = '\0';
    dut->desc_word_i[3 - i] =
        static_cast<uint32_t>(std::strtoul(buf, nullptr, 16));
  }
}


// ---------------------------------------------------------------- ba buoc goc
// Tach ra thanh ham vi phep kiem CHUYEN MO HINH can goi chung theo mot thu tu
// khac. Truoc day ba buoc nay nam long trong mot vong lap va do chinh la ly do
// khoang trong G-1 ton tai: khong the chay bon ho noi nhau ma khong reset.

void reset_dut() {
  dut->rst_ni = 0;
  dut->start_i = 0;
  dut->pre_we_i = 0;
  dut->dma_start_i = 0;
  dut->ws_valid_i = 0;
  dut->desc_valid_i = 0;
  for (int i = 0; i < 3; ++i) tick();
  dut->rst_ni = 1;
  tick();
}

// Nap trong so cua mot ho. Tra ve false neu DMA con chay sau khi nap het.
bool dma_weights(const Fam &f) {
  dut->n_layers_i = static_cast<uint8_t>(f.n_layers);
  dut->in_len_i = static_cast<uint16_t>(f.in_len);

  const size_t nword = (f.w.size() + 7) / 8;
  dut->dma_start_i = 1;
  dut->dma_len_i = static_cast<uint32_t>(nword * 8);
  tick();
  dut->dma_start_i = 0;
  for (size_t k = 0; k < nword; ++k) {
    uint64_t word = 0;
    for (int j = 0; j < 8; ++j) {
      const size_t idx = k * 8 + j;
      const uint8_t byte = (idx < f.w.size())
                               ? static_cast<uint8_t>(static_cast<int8_t>(f.w[idx]))
                               : 0;
      word |= static_cast<uint64_t>(byte) << (8 * j);
    }
    dut->ws_valid_i = 1;
    dut->ws_data_i = word;
    dut->eval();
    const bool moved = dut->ws_ready_o != 0;
    tick();
    if (!moved) --k;   // back pressure: retry the same word
  }
  dut->ws_valid_i = 0;
  tick();
  return dut->dma_busy_o == 0;
}

// Chay MOT nhip. Tra ve so dia chi sai; *cycles nhan so chu ky; *first_bad nhan
// lop dau tien phan ky (-1 neu dung het). `got` duoc xoa moi nhip, con trang thai
// DUT thi CO Y khong.
int run_beat(const Fam &f, const Beat &b, long *cycles, int *first_bad,
             std::map<std::pair<int, int>, int> *got_out) {
  for (size_t i = 0; i < b.xq.size(); ++i) {
    dut->pre_we_i = 1;
    dut->pre_buf_i = 14;
    dut->pre_off_i = static_cast<uint16_t>(i);
    dut->pre_data_i = static_cast<int8_t>(b.xq[i]);
    tick();
  }
  for (size_t i = 0; i < b.rq.size(); ++i) {
    dut->pre_we_i = 1;
    dut->pre_buf_i = 14;
    dut->pre_off_i = static_cast<uint16_t>(f.in_len + i);
    dut->pre_data_i = static_cast<int8_t>(b.rq[i]);
    tick();
  }
  dut->pre_we_i = 0;
  tick();

  dut->start_i = 1;
  tick();
  dut->start_i = 0;

  std::map<std::pair<int, int>, int> got;
  long cyc = 0;
  const long cap = 4000000;
  while (cyc < cap) {
    if (dut->desc_req_o) {
      const int idx = dut->desc_idx_o;
      if (idx >= 0 && idx < f.n_layers) set_desc(f.desc[idx]);
      dut->desc_valid_i = 1;
    } else {
      dut->desc_valid_i = 0;
    }
    dut->eval();
    {
      const int so = dut->s_off_o;
      const int bo = dut->b_off_o;
      if (!f.sc.empty()) {
        const int si = (so >= 0 && so < static_cast<int>(f.sc.size())) ? so : 0;
        dut->s_mult_i = static_cast<uint16_t>(f.sc[si].first);
        dut->s_shift_i = static_cast<uint8_t>(f.sc[si].second);
      }
      // 9 bit co dau, KHONG ep ve int8: mot muc bias thuc te la -170 va ep ve
      // int8 lam no thanh +86, tuc ca mot kenh ra sai.
      dut->s_bias_i = static_cast<int16_t>(
          (bo >= 0 && bo < static_cast<int>(f.bias.size())) ? f.bias[bo] : 0);
    }
    dut->eval();
    tick();
    if (dut->wr_o) {
      got[{static_cast<int>(dut->wr_buf_o), static_cast<int>(dut->wr_off_o)}] =
          static_cast<int>(static_cast<int8_t>(dut->wr_data_o));
    }
    ++cyc;
    if (dut->done_o) break;
  }
  *cycles = cyc;
  if (cyc >= cap) { *first_bad = -2; return -1; }   // -2 = khong ket thuc

  int bad = 0, fb = 1 << 30;
  for (const auto &wv : b.want) {
    const auto it = got.find(wv.first);
    if (it == got.end() || it->second != wv.second.first) {
      ++bad;
      if (wv.second.second < fb) fb = wv.second.second;
    }
  }
  *first_bad = bad ? fb : -1;
  if (got_out) *got_out = std::move(got);
  return bad;
}
}  // namespace

double sc_time_stamp() { return 0; }

// Doc mot san toi thieu tu moi truong. Mac dinh KHAC 0 la co y: cai gia tri
// vo ly can chan chinh la 0, nen mac dinh khong duoc phep la 0.
static long env_long(const char *ten, long mac_dinh) {
  const char *v = std::getenv(ten);
  if (!v || !*v) return mac_dinh;
  char *het = nullptr;
  const long x = std::strtol(v, &het, 10);
  if (het == v || *het) {
    std::printf("%s = '%s' khong phai so, dung mac dinh %ld\n", ten, v, mac_dinh);
    return mac_dinh;
  }
  return x;
}

// Moi `return 2` trong duong PHAN TICH phai NOI RA cho nao hong. Truoc do chung im
// lang: mot tep vector bi CAT CUT hay mot header khai nhieu ho hon thuc te lam
// `fscanf` that bai roi `return 2` khong in gi, va `make sim-coproc` do voi KHONG
// MOT DONG dau ra -- nguoi doc phai di doc ma nguon de biet tai sao. Do la cung ho
// voi `%Error: Unknown warning specified: -Wno-UNUSEDPARAM`: mot thong bao chi vao
// mot CO thay vi vao nguyen nhan, chi la o day khong co thong bao nao ca.
// Da do: header khai 8 ho tren mot tep co 4, va mot tep bi cat giua -- ca hai
// exit 2 va im lang.
#define DOC_HONG(what) do {                                                        \
    std::printf("tb_ecg_coproc: doc HONG tep vector %s\n"                          \
                "  dang doc: %s  (tb_ecg_coproc.cpp:%d)\n"                         \
                "  Tep bi CAT CUT, hay header khai nhieu hon thuc te. Sinh lai:\n" \
                "    python3 tools/rtl_ref/coproc_ref.py\n",                       \
                path, (what), __LINE__);                                   \
    return 2; } while (0)

int main(int argc, char **argv) {
  Verilated::commandArgs(argc, argv);

  const char *path = (argc > 1) ? argv[1] : "40-rtl/tb/vectors/coproc.txt";
  FILE *fp = std::fopen(path, "r");
  if (!fp) {
    std::printf("tb_ecg_coproc: khong mo duoc %s (chay `python3 "
                "tools/rtl_ref/coproc_ref.py`)\n", path);
    return 2;
  }
  int nfam = 0;
  if (std::fscanf(fp, "%d", &nfam) != 1) DOC_HONG("fp, '%d', &nfam) != 1");

  // Doc HET vao bo nho truoc, roi moi chay. Truoc day doc va chay xen ke nen
  // khong the chay ho 0 sau ho 3 -- va do la ly do khoang trong G-1 ton tai.
  std::vector<Fam> fams(nfam);
  for (int fi = 0; fi < nfam; ++fi) {
    Fam &f = fams[fi];
    if (std::fscanf(fp, "%31s %d %d", f.stem, &f.n_layers, &f.in_len) != 3) DOC_HONG("fp, '%31s %d %d', f.stem, &f.n_layers, &f.in_len) != 3");
    f.desc.resize(f.n_layers);
    for (int i = 0; i < f.n_layers; ++i) {
      char hex[64];
      if (std::fscanf(fp, "%40s", hex) != 1) DOC_HONG("fp, '%40s', hex) != 1");
      f.desc[i] = hex;
    }
    if (!rdvec(fp, f.w)) DOC_HONG("fp, f.w)");
    int n = 0;
    if (std::fscanf(fp, "%d", &n) != 1) DOC_HONG("fp, '%d', &n) != 1");
    f.sc.resize(n);
    for (int i = 0; i < n; ++i) {
      if (std::fscanf(fp, "%d %d", &f.sc[i].first, &f.sc[i].second) != 2) DOC_HONG("fp, '%d %d', &f.sc[i].first, &f.sc[i].second) != 2");
    }
    if (!rdvec(fp, f.bias)) DOC_HONG("fp, f.bias)");
    int nbeat = 0;
    if (std::fscanf(fp, "%d", &nbeat) != 1) DOC_HONG("fp, '%d', &nbeat) != 1");
    f.beats.resize(nbeat);
    for (int bi = 0; bi < nbeat; ++bi) {
      Beat &b = f.beats[bi];
      if (!rdvec(fp, b.xq) || !rdvec(fp, b.rq)) DOC_HONG("fp, b.xq) || !rdvec(fp, b.rq)");
      if (std::fscanf(fp, "%d", &n) != 1) DOC_HONG("fp, '%d', &n) != 1");
      for (int i = 0; i < n; ++i) {
        int buf, o, v, ly;
        if (std::fscanf(fp, "%d %d %d %d", &buf, &o, &v, &ly) != 4) DOC_HONG("fp, '%d %d %d %d', &buf, &o, &v, &ly) != 4");
        b.want[{buf, o}] = {v, ly};
      }
    }
  }
  std::fclose(fp);

  // ====================================================== CHOT DAU VAO
  // Truoc ban nay dieu kien PASS la CHI `errors == 0`, nen mot tep vector khai
  // `0` ho cho 0 loi va 0 loi cho PASS: TB in "PASS ... tren 0 nhip" va con in
  // "ADR-0013 dung" tren khong bang chung nao. Do la mot cong KHONG THE THAT
  // BAI do chinh cau PASS lai duoc luong hoa bang hai con so (`n_beat_total`,
  // `sw_ok`) ma no khong kiem. Vector duoc SINH LAI moi luot boi coproc_ref.py,
  // nen mot doi so `--families` sai hay mot ngoai le bi nuot se cho ra tep rong
  // va phep kiem van xanh.
  //
  // Hai lop chot, va can CA HAI: doi chieu voi khai bao cua chinh tep bat duoc
  // tep bi CAT CUT, con san tuyet doi bat duoc tep tu khai `0` mot cach NHAT
  // QUAN -- lop hai khong suy ra duoc tu lop mot.
  long n_beat_khai = 0;
  for (const Fam &f : fams) n_beat_khai += static_cast<long>(f.beats.size());
  const long min_fam  = env_long("ECG_TB_MIN_FAM", 2);   // <2 thi khong co BIEN de chuyen
  const long min_beat = env_long("ECG_TB_MIN_BEAT", 8);  // 2 ho x 4 nhip la san cua pha B
  int chot_bad = 0;
  if (nfam < min_fam) {
    std::printf("CHOT DAU VAO: tep khai %d ho, can >= %ld\n", nfam, min_fam);
    ++chot_bad;
  }
  if (n_beat_khai < min_beat) {
    std::printf("CHOT DAU VAO: tep khai %ld nhip tong, can >= %ld\n",
                n_beat_khai, min_beat);
    ++chot_bad;
  }
  for (int fi = 0; fi < nfam; ++fi) {
    if (fams[fi].beats.empty()) {
      std::printf("CHOT DAU VAO: ho %s khai 0 nhip\n", fams[fi].stem);
      ++chot_bad;
    }
  }
  if (chot_bad) {
    std::printf("tb_ecg_coproc: FAIL (%d chot dau vao, tep vector %s khong du "
                "de mang tin)\n", chot_bad, path);
    return 2;
  }

  // So nhip pha B PHAI chay, suy ra tu khai bao: hai luot x min(n_sw, so nhip).
  const int n_sw = 4;
  long sw_khai = 0;
  for (const Fam &f : fams)
    sw_khai += 2L * std::min<long>(n_sw, static_cast<long>(f.beats.size()));

  dut = new Vecg_coproc;

  int errors = 0, n_ok = 0, n_beat_total = 0;
  long total_cycles = 0;
  std::map<std::string, int> per_fam_ok, per_fam_bad, per_fam_first_bad;
  std::map<std::string, long> per_fam_cyc;

  // ================================================== PHA A · theo tung ho
  // Reset va nap trong so mot lan moi ho, roi N nhip chay lien tiep. Day la pha
  // do SAU: 250 nhip moi ho, lay mau phan tang, bat bat bien ADR-0013 giua cac
  // nhip TRONG cung mot ho.
  std::printf("=== PHA A · tung ho, %zu nhip moi ho, cac nhip lien tiep khong reset\n",
              fams.empty() ? 0 : fams[0].beats.size());
  for (Fam &f : fams) {
    reset_dut();
    if (!dma_weights(f)) {
      std::printf("%s: DMA con chay sau khi nap het\n", f.stem);
      ++errors;
    }
    for (int bi = 0; bi < static_cast<int>(f.beats.size()); ++bi) {
      long cyc = 0; int fb = -1;
      std::map<std::pair<int, int>, int> got;
      const int bad = run_beat(f, f.beats[bi], &cyc, &fb, &got);
      ++n_beat_total; total_cycles += cyc; per_fam_cyc[f.stem] += cyc;
      if (bad == 0) { ++n_ok; per_fam_ok[f.stem]++; continue; }
      ++errors; per_fam_bad[f.stem]++;
      if (!per_fam_first_bad.count(f.stem)) per_fam_first_bad[f.stem] = bi;
      // In tung dia chi sai cua LOP DAU TIEN phan ky. Ban refactor truoc bo mat phan
      // nay va hau qua hien ra ngay lan dau can no: mot loi that chi bao duoc "27 /
      // 5189 dia chi sai, lop 10" ma khong noi dia chi nao, nen khong the doan duoc
      // no la loi cap phat, loi dia chi hay loi gia tri.
      if (per_fam_bad[f.stem] == 1 && fb >= 0) {
        int shown = 0;
        for (const auto &wv : f.beats[bi].want) {
          if (wv.second.second != fb) continue;
          const auto it = got.find(wv.first);
          if (it != got.end() && it->second == wv.second.first) continue;
          if (shown++ >= 12) break;
          std::printf("    %s lop %d bo dem %2d offset %5d: ky vong %4d, %s\n",
                      f.stem, fb, wv.first.first, wv.first.second,
                      wv.second.first,
                      it == got.end() ? "KHONG CO"
                          : (std::string("nhan ") + std::to_string(it->second)).c_str());
        }
      }
      if (per_fam_bad[f.stem] <= 3) {
        if (fb == -2) {
          std::printf("%s nhip %d: khong ket thuc trong 4.000.000 chu ky\n",
                      f.stem, bi);
        } else {
          std::printf("%s nhip %d: %d / %zu dia chi sai, lop dau tien lech = %d, "
                      "%ld chu ky\n", f.stem, bi, bad, f.beats[bi].want.size(),
                      fb, cyc);
        }
      }
      if (fb == -2) break;   // trang thai khong xac dinh, bo ho nay
    }
  }

  // ============================================ PHA B · CHUYEN MO HINH (G-1)
  // Khoang trong G-1 trong dac ta kiem thu: KHONG phep kiem nao chuyen mo hinh.
  // Pha A reset moi ho, nen bon ho chua bao gio chay noi nhau trong MOT lan
  // reset -- trong khi chuyen mo hinh la dong gop trung tam cua luan van (D1,
  // T_switch) va la muc dich cua ADR-0013.
  //
  // Pha nay reset DUNG MOT LAN cho ca pha, roi di qua bon ho HAI LUOT: luot mot
  // theo thu tu 0,1,2,3 va luot hai theo thu tu NGUOC 3,2,1,0. Hai luot la co y:
  // moi ho khi do chay sau MOT HO KHAC NHAU o hai luot, nen mot lop doc byte ma
  // MO HINH TRUOC de lai se sai o mot trong hai luot. Mot luot duy nhat se bo
  // sot dung lop loi ay.
  //
  // Chi lay it nhip moi ho (mac dinh 4): do sau da do o pha A, viec can do o day
  // la BIEN giua hai mo hinh.
  int sw_ok = 0, sw_bad = 0;
  long sw_cycles = 0;
  std::printf("\n=== PHA B · CHUYEN MO HINH · mot lan reset, hai luot "
              "(thuan roi nguoc), %d nhip moi ho\n", n_sw);
  reset_dut();
  for (int pass = 0; pass < 2 && !fams.empty(); ++pass) {
    for (int k = 0; k < nfam; ++k) {
      const int fi = pass ? (nfam - 1 - k) : k;
      Fam &f = fams[fi];
      const char *prev = "reset";
      if (k > 0) prev = fams[pass ? (nfam - k) : (k - 1)].stem;
      else if (pass) prev = fams[0].stem;

      if (!dma_weights(f)) {
        std::printf("PHA B luot %d: %s DMA con chay sau khi nap het\n", pass,
                    f.stem);
        ++errors; ++sw_bad;
      }
      const int nb = std::min(n_sw, static_cast<int>(f.beats.size()));
      for (int bi = 0; bi < nb; ++bi) {
        long cyc = 0; int fb = -1;
        const int bad = run_beat(f, f.beats[bi], &cyc, &fb, nullptr);
        sw_cycles += cyc;
        if (bad == 0) { ++sw_ok; continue; }
        ++sw_bad; ++errors;
        if (sw_bad <= 6) {
          std::printf("  PHA B luot %d: %s nhip %d SAI %d dia chi, lop dau tien "
                      "lech = %d (mo hinh chay TRUOC no la %s)\n",
                      pass, f.stem, bi, bad, fb, prev);
        }
        if (fb == -2) break;
      }
    }
  }
  std::printf("PHA B: %d dung, %d sai tren %d nhip · %ld chu ky\n",
              sw_ok, sw_bad, sw_ok + sw_bad, sw_cycles);
  // `sw_bad == 0` mot minh KHONG cho phat bieu nay: 0 nhip cung cho 0 sai.
  if (sw_bad == 0 && sw_ok > 0) {
    std::printf("PHA B: khong ro ri trang thai giua CAC MO HINH tren %d nhip "
                "(ADR-0013 dung ca qua bien chuyen mo hinh)\n", sw_ok);
  }

  dut->final();
  ECG_COV_WRITE();
  delete dut;

  std::printf("\ntb_ecg_coproc: PHA A %d / %d nhip dung, %ld chu ky tong\n",
              n_ok, n_beat_total, total_cycles);
  for (int pass = 0; pass < 2; ++pass) {
    const auto &src = pass ? per_fam_bad : per_fam_ok;
    for (const auto &kv : src) {
      if (pass && per_fam_ok.count(kv.first)) continue;
      const int ok = per_fam_ok.count(kv.first) ? per_fam_ok.at(kv.first) : 0;
      const int bd = per_fam_bad.count(kv.first) ? per_fam_bad.at(kv.first) : 0;
      const long nb = ok + bd;
      std::printf("   %-14s %d dung, %d sai, %ld chu ky/nhip", kv.first.c_str(),
                  ok, bd, nb ? per_fam_cyc[kv.first] / nb : 0L);
      // Nhip dau tien sai la chan doan: nhip 0 sai = loi trong mot nhip; nhip 0
      // dung ma nhip sau sai = trang thai ro ri giua cac nhip (ADR-0013).
      if (bd && per_fam_first_bad.count(kv.first)) {
        std::printf(" (nhip dau tien sai = %d%s)", per_fam_first_bad.at(kv.first),
                    per_fam_first_bad.at(kv.first) > 0
                        ? ", tuc ro ri trang thai giua cac nhip" : "");
      }
      std::printf("\n");
    }
  }

  // ====================================================== CHOT HAU CHAY
  // `errors == 0` mot minh khong du: no xanh ca khi khong nhip nao chay. Cau
  // PASS duoi day duoc luong hoa bang `n_beat_total` va `sw_ok`, nen CA HAI con
  // so do phai bang dung cai ma tep da khai -- neu khong thi cau PASS phat bieu
  // nhieu hon phan da do.
  int chot_sau = 0;
  if (n_beat_total != n_beat_khai) {
    std::printf("CHOT HAU CHAY: pha A chay %d nhip, tep khai %ld\n",
                n_beat_total, n_beat_khai);
    ++chot_sau;
  }
  if (n_ok != n_beat_total) {
    std::printf("CHOT HAU CHAY: pha A %d dung tren %d chay\n", n_ok, n_beat_total);
    ++chot_sau;
  }
  if (sw_ok + sw_bad != sw_khai) {
    std::printf("CHOT HAU CHAY: pha B chay %d nhip, suy tu khai bao la %ld\n",
                sw_ok + sw_bad, sw_khai);
    ++chot_sau;
  }
  if (chot_sau) {
    std::printf("tb_ecg_coproc: FAIL (%d chot hau chay -- so nhip DA CHAY khong "
                "khop khai bao cua tep vector)\n", chot_sau);
    return 1;
  }

  if (errors == 0) {
    // Bo vector co the sinh tu DS2 hay tu DS1-validation (`ECG_SPLIT` cua
    // coproc_ref.py). Testbench KHONG biet duoc cai nao, nen no khong duoc KHAI
    // cai nao -- nguon nam trong ban niem vector, khong trong dong nay.
    std::printf("tb_ecg_coproc: PASS (N2: %d ho dung tung bit tren %d nhip THAT "
                "tu bo vector, chay lien tiep khong reset, VA %d nhip qua hai "
                "luot chuyen mo hinh)\n", nfam, n_beat_total, sw_ok);
    return 0;
  }
  std::printf("tb_ecg_coproc: FAIL (%d nhip sai)\n", errors);
  return 1;
}

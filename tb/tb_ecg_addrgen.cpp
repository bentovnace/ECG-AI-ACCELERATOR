// Verilator harness for ecg_addrgen.
//
// Checks two separate things, and the second is the one that matters most.
//
//   1. The index sequence matches the reference element for element, over the
//      34 real layers of the four sealed models.
//   2. The ADR-0013 invariant: an issue is marked out-of-bounds if and only if
//      its input index lies outside [0, len_in). If a boundary read is ever let
//      through, activations from the previously loaded model leak into the
//      current result. That failure appears only after a model switch, biases
//      the output only slightly, and depends on which model ran before, so a
//      single-model tolerance-based test would not catch it.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "Vecg_addrgen.h"
#include "verilated.h"
#include "ecg_cov.h"

namespace {

struct Issue {
  int32_t out_pos, out_ch, tap, in_ch, in_pos, oob, acc_clear, out_valid;
};

struct Layer {
  int32_t len_in, len_out, cin, cout, k, stride, pad, dw;
  std::vector<Issue> issues;
};

std::vector<Layer> load(const std::string &path) {
  std::vector<Layer> layers;
  FILE *fd = std::fopen(path.c_str(), "r");
  if (!fd) {
    std::fprintf(stderr, "tb_ecg_addrgen: cannot open %s\n", path.c_str());
    std::exit(2);
  }
  char tag = 0;
  while (std::fscanf(fd, " %c", &tag) == 1) {
    if (tag != 'L') {
      std::fprintf(stderr, "tb_ecg_addrgen: malformed vector file\n");
      std::exit(2);
    }
    Layer L{};
    int32_t n = 0;
    if (std::fscanf(fd, "%d %d %d %d %d %d %d %d %d", &L.len_in, &L.len_out,
                    &L.cin, &L.cout, &L.k, &L.stride, &L.pad, &L.dw, &n) != 9) {
      std::fprintf(stderr, "tb_ecg_addrgen: malformed layer header\n");
      std::exit(2);
    }
    L.issues.resize(static_cast<size_t>(n));
    for (int32_t i = 0; i < n; ++i) {
      Issue &e = L.issues[static_cast<size_t>(i)];
      if (std::fscanf(fd, "%d %d %d %d %d %d %d %d", &e.out_pos, &e.out_ch,
                      &e.tap, &e.in_ch, &e.in_pos, &e.oob, &e.acc_clear,
                      &e.out_valid) != 8) {
        std::fprintf(stderr, "tb_ecg_addrgen: truncated layer body\n");
        std::exit(2);
      }
    }
    layers.push_back(std::move(L));
  }
  std::fclose(fd);
  return layers;
}

Vecg_addrgen *dut = nullptr;

void tick() {
  dut->clk_i = 0;
  dut->eval();
  dut->clk_i = 1;
  dut->eval();
}

// Verilator packs a signed 12-bit port into a 16-bit word; sign-extend it.
int32_t sext12(uint32_t v) {
  const int32_t x = static_cast<int32_t>(v & 0xFFFu);
  return (x & 0x800) ? (x - 0x1000) : x;
}

}  // namespace

// Verilator 4.x calls this to timestamp $error/$display from the RTL. The
// harness has no notion of time beyond the tick counter, so a constant is
// sufficient and keeps the message format stable across runs.
double sc_time_stamp() { return 0; }

// Doc mot san toi thieu tu moi truong. Mac dinh KHAC 0 la co y: gia tri vo ly
// can chan chinh la 0, nen mac dinh khong duoc phep la 0.
static long env_long(const char *ten, long mac_dinh) {
  const char *v = std::getenv(ten);
  if (!v || !*v) return mac_dinh;
  char *het = nullptr;
  const long x = std::strtol(v, &het, 10);
  if (het == v || *het) return mac_dinh;
  return x;
}

int main(int argc, char **argv) {
  Verilated::commandArgs(argc, argv);
  std::string path = "40-rtl/tb/addrgen_vectors.txt";
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a.rfind("+vec=", 0) == 0) path = a.substr(5);
  }

  const std::vector<Layer> layers = load(path);
  size_t total = 0;
  for (const Layer &L : layers) total += L.issues.size();
  std::printf("tb_ecg_addrgen: %zu layers, %zu issues from %s\n", layers.size(),
              total, path.c_str());

  // ====================================================== CHOT DAU VAO
  // Truoc ban nay mot tep RONG in "0 layers, 0 issues" roi khang dinh "PASS
  // (0 issues, index-exact, ADR-0013 invariant holds)" -- tuc phat bieu ADR-0013
  // dung tren khong bang chung nao. Chot theo KHAI BAO cua chinh tep (moi lop
  // khai so issue) VA mot san tuyet doi: lop mot bat tep bi cat cut, lop hai bat
  // tep tu khai 0 mot cach nhat quan.
  {
    const long min_layer = env_long("ECG_TB_MIN_LAYER", 1);
    const long min_issue = env_long("ECG_TB_MIN_ISSUE", 8);
    int chot_bad = 0;
    if (static_cast<long>(layers.size()) < min_layer) {
      std::printf("CHOT DAU VAO: tep khai %zu lop, can >= %ld\n",
                  layers.size(), min_layer);
      ++chot_bad;
    }
    if (static_cast<long>(total) < min_issue) {
      std::printf("CHOT DAU VAO: tep khai %zu issue tong, can >= %ld\n",
                  total, min_issue);
      ++chot_bad;
    }
    for (size_t li = 0; li < layers.size(); ++li) {
      if (layers[li].issues.empty()) {
        std::printf("CHOT DAU VAO: lop %zu khai 0 issue\n", li);
        ++chot_bad;
      }
    }
    if (chot_bad) {
      std::printf("tb_ecg_addrgen: FAIL (%d chot dau vao, tep %s khong du de "
                  "mang tin)\n", chot_bad, path.c_str());
      return 2;
    }
  }

  dut = new Vecg_addrgen;
  dut->rst_ni = 0;
  dut->start_i = 0;
  dut->step_i = 0;
  for (int i = 0; i < 3; ++i) tick();
  dut->rst_ni = 1;
  tick();

  int errors = 0;
  int oob_violations = 0;
  size_t oob_seen = 0;
  size_t layer_idx = 0;

  for (const Layer &L : layers) {
    dut->len_in_i = static_cast<uint16_t>(L.len_in);
    dut->len_out_i = static_cast<uint16_t>(L.len_out);
    dut->cin_i = static_cast<uint16_t>(L.cin);
    dut->cout_i = static_cast<uint16_t>(L.cout);
    dut->k_i = static_cast<uint8_t>(L.k);
    dut->stride_i = static_cast<uint8_t>(L.stride);
    dut->pad_i = static_cast<uint8_t>(L.pad);
    dut->dw_i = static_cast<uint8_t>(L.dw);

    dut->start_i = 1;
    dut->step_i = 0;
    tick();
    dut->start_i = 0;
    dut->step_i = 1;

    for (size_t i = 0; i < L.issues.size(); ++i) {
      const Issue &e = L.issues[i];
      const int32_t got_pos = sext12(dut->in_pos_o);

      bool bad = false;
      bad |= (static_cast<int32_t>(dut->out_pos_o) != e.out_pos);
      bad |= (static_cast<int32_t>(dut->out_ch_o) != e.out_ch);
      bad |= (static_cast<int32_t>(dut->tap_o) != e.tap);
      bad |= (static_cast<int32_t>(dut->in_ch_o) != e.in_ch);
      bad |= (got_pos != e.in_pos);
      bad |= (static_cast<int32_t>(dut->oob_o) != e.oob);
      bad |= (static_cast<int32_t>(dut->acc_clear_o) != e.acc_clear);
      bad |= (static_cast<int32_t>(dut->out_valid_o) != e.out_valid);

      if (bad) {
        if (++errors <= 15) {
          std::printf("layer %zu issue %zu: got (pos=%d ch=%d tap=%d ich=%d"
                      " ipos=%d oob=%d clr=%d ov=%d) want (%d %d %d %d %d %d"
                      " %d %d)\n",
                      layer_idx, i, dut->out_pos_o, dut->out_ch_o, dut->tap_o,
                      dut->in_ch_o, got_pos, dut->oob_o, dut->acc_clear_o,
                      dut->out_valid_o, e.out_pos, e.out_ch, e.tap, e.in_ch,
                      e.in_pos, e.oob, e.acc_clear, e.out_valid);
        }
      }

      // The invariant, checked against the DUT's own outputs rather than
      // against the reference, so it still holds if both were wrong together.
      const bool inside = (got_pos >= 0) && (got_pos < L.len_in);
      if (inside == (dut->oob_o != 0)) {
        if (++oob_violations <= 5) {
          std::printf("ADR-0013 violation: layer %zu issue %zu ipos=%d"
                      " len_in=%d oob=%d\n",
                      layer_idx, i, got_pos, L.len_in, dut->oob_o);
        }
      }
      if (dut->oob_o) ++oob_seen;

      tick();
    }
    dut->step_i = 0;
    tick();
    ++layer_idx;
  }

  dut->final();
  ECG_COV_WRITE();
  delete dut;

  std::printf("tb_ecg_addrgen: %zu of %zu issues were boundary reads"
              " (zero-injected, never fetched)\n", oob_seen, total);

  if (errors == 0 && oob_violations == 0) {
    std::printf("tb_ecg_addrgen: PASS (%zu issues, index-exact,"
                " ADR-0013 invariant holds)\n", total);
    return 0;
  }
  std::printf("tb_ecg_addrgen: FAIL (%d index mismatches, %d invariant"
              " violations)\n", errors, oob_violations);
  return 1;
}

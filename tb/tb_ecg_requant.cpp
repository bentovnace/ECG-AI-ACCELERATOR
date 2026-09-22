// Verilator harness for ecg_requant.
//
// The vectors and the expected results come from tools/rtl_ref/requant_ref.py,
// a separate implementation of the same specification. Comparing the RTL
// against a reimplementation rather than against itself is what makes this test
// able to fail: a mistake shared by both would cancel out, and N2 would then
// fail much later with the cause hidden a layer away.
//
// The RTL is compiled with a local simulator (Verilator 4.x here). Icarus
// Verilog 11 cannot build it because ecg_pkg uses an `inside` expression.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "Vecg_requant.h"
#include "verilated.h"
#include "ecg_cov.h"

namespace {

struct Vec {
  int32_t psum, bias, expected;
  uint32_t mult, shift, relu;
};

std::vector<Vec> load(const std::string &path) {
  std::vector<Vec> v;
  FILE *fd = std::fopen(path.c_str(), "r");
  if (!fd) {
    std::fprintf(stderr, "tb_ecg_requant: cannot open %s\n", path.c_str());
    std::exit(2);
  }
  Vec e{};
  while (std::fscanf(fd, "%d %d %u %u %u %d", &e.psum, &e.bias, &e.mult,
                     &e.shift, &e.relu, &e.expected) == 6) {
    v.push_back(e);
  }
  std::fclose(fd);
  return v;
}

Vecg_requant *dut = nullptr;

void tick() {
  dut->clk_i = 0;
  dut->eval();
  dut->clk_i = 1;
  dut->eval();
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
  std::string path = "tb/requant_vectors.txt";
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a.rfind("+vec=", 0) == 0) path = a.substr(5);
  }

  const std::vector<Vec> vecs = load(path);
  std::printf("tb_ecg_requant: %zu vectors from %s\n", vecs.size(), path.c_str());

  // ====================================================== CHOT DAU VAO
  // Truoc ban nay dieu kien PASS la CHI `errors == 0`, nen mot tep RONG in
  // "PASS (0 vectors, bit-exact)" -- no in ra so 0 roi bao PASS. Tep nay duoc
  // SINH LAI moi luot boi requant_ref.py, ma script do co `--random` mac dinh
  // 20000; mot luot `--random 100` thu nho tep va khong ai chot so luong.
  //
  // KHAC coproc/addrgen: tep nay chi la mot danh sach vector, KHONG khai truoc
  // so luong, nen khong co gi de doi chieu -- san tuyet doi la chot DUY NHAT co
  // the o day, va con so cua bai bao phai duoc ghim o recipe qua ECG_TB_MIN_VEC.
  const long min_vec = env_long("ECG_TB_MIN_VEC", 64);
  if (static_cast<long>(vecs.size()) < min_vec) {
    std::printf("tb_ecg_requant: FAIL (chot dau vao: %zu vector, can >= %ld -- "
                "tep %s khong du de mang tin)\n", vecs.size(), min_vec,
                path.c_str());
    return 2;
  }

  dut = new Vecg_requant;
  dut->rst_ni = 0;
  dut->valid_i = 0;
  for (int i = 0; i < 3; ++i) tick();
  dut->rst_ni = 1;
  tick();

  // One vector per cycle, which is also how the sequencer will drive this
  // stage. An earlier version of this loop checked vector i-1 after applying
  // vector i and reported every case as a mismatch; the giveaway was an error
  // count larger than the number of vectors.
  int errors = 0;
  for (size_t i = 0; i < vecs.size(); ++i) {
    const Vec &e = vecs[i];
    dut->valid_i = 1;
    dut->psum_i = e.psum;
    dut->bias_i = e.bias;
    dut->mult_i = e.mult;
    dut->shift_i = e.shift;
    dut->act_i = e.relu ? 1 : 0;  // ECG_ACT_RELU : ECG_ACT_NONE

    // tick() drives the posedge with these inputs applied, and the DUT
    // registers its combinational result on that edge, so act_o is the answer
    // for this vector once tick() returns.
    tick();

    const int32_t got = static_cast<int8_t>(dut->act_o);
    if (got != e.expected) {
      ++errors;
      if (errors <= 20) {
        std::printf("vec %zu: psum=%d bias=%d mult=%u shift=%u relu=%u"
                    " -> got %d want %d\n",
                    i, e.psum, e.bias, e.mult, e.shift, e.relu, got,
                    e.expected);
      }
    }
  }
  dut->valid_i = 0;
  tick();

  dut->final();
  ECG_COV_WRITE();
  delete dut;

  if (errors == 0) {
    std::printf("tb_ecg_requant: PASS (%zu vectors, bit-exact)\n", vecs.size());
    return 0;
  }
  std::printf("tb_ecg_requant: FAIL (%d of %zu mismatched)\n", errors,
              vecs.size());
  return 1;
}

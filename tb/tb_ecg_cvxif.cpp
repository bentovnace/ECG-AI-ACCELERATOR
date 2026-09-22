// Verilator harness for ecg_cvxif.
//
// Vectors come from tools/rtl_ref/cvxif_ref.py, which decides accept/reject and
// blocking from the RULES in docs/isa.md, not from this module's structure -- its
// queue is a Python list, not a model of the FIFO.
//
// The harness plays the core and the coprocessor. Two things it must do properly
// or the result means nothing:
//
//   * Model the coprocessor as taking a VARIABLE number of cycles per layer, and
//     vary it. A shim that only works when the coprocessor is instantly ready
//     passes a fixed-latency model and deadlocks on real hardware.
//   * Check that a rejected instruction is rejected on the ACCEPT line, not
//     merely ignored. A shim that accepts everything and does nothing for unknown
//     opcodes passes any test built only from legal instructions, and on real
//     hardware it swallows the core's illegal-instruction trap.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <vector>

#include "Vecg_cvxif.h"
#include "verilated.h"
#include "ecg_cov.h"

namespace {

Vecg_cvxif *dut = nullptr;

// F11: `ecg_cvxif` hanh dong o COMMIT. Tep nay lai shim TRUC TIEP (khong qua
// `ecg_xif_bridge`), nen no phai MO HINH HOA kenh commit -- mot nhip `cmt_ok_i`
// MOT chu ky sau moi phep nhan, khong bao gio huy.
//
// Thieu no thi `spec_valid_q` khong bao gio duoc xoa va khang dinh chan tren
// (`ecg_cvxif.sv:412`) ban dung nhu no phai: duong issue TREO that. Do la cach
// toi phat hien ra tep nay -- va no khong nam trong danh sach hoi quy F11 cua
// toi (`sim-cvxif` khac `sim-cvxif-loadw`), nen chinh phep do do phu moi bat.
//
// Mot tb mo hinh hoa mot kenh thi phai NOI RA. Duong KILL duoc do o
// `tb_ecg_xif_kill`, noi commit duoc lai TU NGOAI va ca hai nhanh deu co commit.
bool cho_commit = false;

void tick() {
  // MOT NHIP, khong cung nhip: `spec_valid_q` chi len O CHINH canh nay, nen mot
  // `cmt_ok_i` cung chu ky voi phep nhan se khong bao gio giai phong cho.
  const bool nhan = dut->iss_valid_i && dut->iss_ready_o && dut->iss_accept_o;
  dut->clk_i = 0;
  dut->eval();
  dut->cmt_ok_i = cho_commit ? 1 : 0;
  dut->clk_i = 1;
  dut->eval();
  cho_commit = nhan;
}

}  // namespace

double sc_time_stamp() { return 0; }

// Moi `return 2` trong duong PHAN TICH phai NOI RA cho nao hong. Truoc do chung
// im lang: `fscanf` that bai roi `return 2` khong in gi, nen mot tep vector bi
// CAT CUT lam `make sim-cvxif` do voi KHONG MOT DONG dau ra va nguoi doc phai di
// doc ma nguon. Chot bao ve dung truong hop TU BAO MINH ("khong mo duoc", co
// thong bao) va bo truong hop IM LANG.
#define DOC_HONG(what) do {                                                    \
    std::printf("tb_ecg_cvxif: doc HONG tep vector %s\n"                         \
                "  dang doc: %s  (tb_ecg_cvxif.cpp:%d)\n"                       \
                "  Tep bi CAT CUT hay sai dinh dang. Sinh lai:\n"              \
                "    python3 tools/rtl_ref/cvxif_ref.py\n",                              \
                path, (what), __LINE__);                                       \
    return 2; } while (0)

int main(int argc, char **argv) {
  Verilated::commandArgs(argc, argv);

  const char *path = (argc > 1) ? argv[1] : "40-rtl/tb/vectors/cvxif.txt";
  FILE *fp = std::fopen(path, "r");
  if (!fp) {
    std::printf("tb_ecg_cvxif: khong mo duoc %s (chay `python3 "
                "tools/rtl_ref/cvxif_ref.py`)\n", path);
    return 2;
  }
  int ninstr = 0, nlayer = 0;
  if (std::fscanf(fp, "%d %d", &ninstr, &nlayer) != 2) DOC_HONG("std::fscanf(fp, '%d %d', &ninstr, &nlayer) != 2");
  std::vector<int> want_layer(nlayer);
  for (int i = 0; i < nlayer; ++i) {
    if (std::fscanf(fp, "%d", &want_layer[i]) != 1) DOC_HONG("std::fscanf(fp, '%d', &want_layer[i]) != 1");
  }
  // Cot thu SAU `dma_len` la ky vong do `cvxif_ref.py` suy tu isa.md: LOADW duoc
  // nhan thi `dma_len_o` phai bang rs2[13:0], khong thi -1. Truoc khi co cot nay
  // testbench KHONG HE kiem `dma_len_o` -- `grep dma` chi ra mot dong, va no LAI
  // `dma_busy_i` chu khong doc gi. Hai dau ra `dma_len_o`/`dma_start_o` khong ai
  // kiem, cung lop "dat ma khong doc" voi vu `ecg_mmio`.
  struct Instr { uint32_t w; int rs1, rs2, acc, blk, dma_len; };
  std::vector<Instr> prog(ninstr);
  for (int i = 0; i < ninstr; ++i) {
    if (std::fscanf(fp, "%x %d %d %d %d %d", &prog[i].w, &prog[i].rs1,
                    &prog[i].rs2, &prog[i].acc, &prog[i].blk,
                    &prog[i].dma_len) != 6) {
      std::printf("tb_ecg_cvxif: vector thieu cot `dma_len` -- chay lai "
                  "`python3 tools/rtl_ref/cvxif_ref.py`\n");
      return 2;
    }
  }
  std::fclose(fp);

  dut = new Vecg_cvxif;
  dut->rst_ni = 0;
  dut->iss_valid_i = 0;
  dut->cp_busy_i = 0;
  dut->cp_done_i = 0;
  dut->dma_busy_i = 0;
  dut->cmt_ok_i = 0;
  dut->cmt_kill_i = 0;
  cho_commit = false;
  for (int i = 0; i < 3; ++i) tick();
  dut->rst_ni = 1;
  tick();

  int errors = 0;
  int n_acc = 0, n_rej = 0, n_blk_stall = 0;
  std::vector<int> got_layer;
  int cp_left = 0;               // chu ky con lai cua lop dang chay
  unsigned rng = 12345;
  long cycles = 0;

  auto step_cp = [&]() {
    // Bo dong xu ly: nhan start, chay mot so chu ky BIEN DOI, roi bao done.
    dut->cp_done_i = 0;
    if (cp_left > 0) {
      if (--cp_left == 0) dut->cp_done_i = 1;
    }
    dut->cp_busy_i = (cp_left > 0) ? 1 : 0;
  };

  int n_dma_pulse = 0, n_dma_ok = 0, n_dma_expect = 0;
  std::deque<int> cho_dma;      // ky vong dang cho mot xung dma_start_o
  for (const auto &e : prog) if (e.dma_len >= 0) ++n_dma_expect;
  for (int i = 0; i < ninstr; ++i) {
    const Instr &e = prog[i];
    dut->iss_valid_i = 1;
    dut->iss_instr_i = e.w;
    dut->iss_rs1_i = static_cast<uint32_t>(e.rs1);
    dut->iss_rs2_i = static_cast<uint32_t>(e.rs2);
    if (e.dma_len >= 0) cho_dma.push_back(e.dma_len);

    long guard = 0;
    while (true) {
      step_cp();
      dut->eval();

      const bool acc = dut->iss_accept_o != 0;
      const bool rdy = dut->iss_ready_o != 0;

      if (acc != (e.acc != 0)) {
        if (++errors <= 10) {
          std::printf("lenh %d (%08x): accept = %d, ky vong %d\n", i, e.w,
                      acc ? 1 : 0, e.acc);
        }
      }

      // Mot xung `dma_start_o` phai mang DUNG do dai ma isa.md noi.
      //
      // KY VONG PHAI THEO HANG DOI, KHONG THEO LENH HIEN TAI. `dma_start_o` la
      // mot xung DANG KY, nen no toi o nhip SAU nhip phat -- va voi mot LOADW
      // khong chan thi `iss_ready_o` len ngay, vong lap thoat sau mot tick, va
      // xung do duoc quan sat trong khi tb DA sang lenh ke tiep. Ban dau toi so
      // voi `e.dma_len` cua lenh hien tai va phep kiem bao 11 xung / 0 dung voi
      // mot mau lech DUNG MOT lenh -- do la loi cua HARNESS, va chinh cai mau
      // lech deu do chi ra nguyen nhan.
      if (dut->dma_start_o) {
        ++n_dma_pulse;
        if (cho_dma.empty()) {
          if (++errors <= 10)
            std::printf("lenh %d (%08x): dma_start_o xung ma khong LOADW nao "
                        "dang cho\n", i, e.w);
        } else {
          const int want = cho_dma.front();
          cho_dma.pop_front();
          if (static_cast<int>(dut->dma_len_o) != want) {
            if (++errors <= 10)
              std::printf("xung dma thu %d: dma_len_o = %d, ky vong %d\n",
                          n_dma_pulse, static_cast<int>(dut->dma_len_o), want);
          } else {
            ++n_dma_ok;
          }
        }
      }

      // A start pulse means the coprocessor picked up a layer.
      if (dut->cp_start_o) {
        got_layer.push_back(static_cast<int>(dut->cp_layer_o));
        cp_left = 1 + static_cast<int>((rng = rng * 1103515245u + 12345u)
                                       >> 28);   // 1..16 chu ky
      }

      tick();
      ++cycles;

      if (!e.acc) break;          // lenh bi tu choi: khong cho gi
      if (rdy) break;
      ++n_blk_stall;
      if (++guard > 5000) {
        std::printf("lenh %d (%08x): khong bao gio ready\n", i, e.w);
        ++errors;
        break;
      }
    }

    if (e.acc) ++n_acc; else ++n_rej;
    dut->iss_valid_i = 0;
  }

  // Let anything in flight finish.
  for (int i = 0; i < 4000; ++i) {
    step_cp();
    dut->eval();
    if (dut->cp_start_o) {
      got_layer.push_back(static_cast<int>(dut->cp_layer_o));
      cp_left = 1 + static_cast<int>((rng = rng * 1103515245u + 12345u) >> 28);
    }
    tick();
  }

  dut->final();
  ECG_COV_WRITE();
  delete dut;

  std::printf("tb_ecg_cvxif: %d lenh nhan, %d tu choi, %d chu ky dung o rao "
              "chan, %zu lop phat / %d ky vong\n", n_acc, n_rej, n_blk_stall,
              got_layer.size(), nlayer);

  if (got_layer.size() != want_layer.size()) {
    std::printf("tb_ecg_cvxif: FAIL (phat %zu lop, ky vong %d)\n",
                got_layer.size(), nlayer);
    return 1;
  }
  for (size_t i = 0; i < got_layer.size(); ++i) {
    if (got_layer[i] != want_layer[i]) {
      if (++errors <= 10) {
        std::printf("lop thu %zu: phat %d, ky vong %d\n", i, got_layer[i],
                    want_layer[i]);
      }
    }
  }
  // A suite where the barrier never stalls proves nothing about blocking.
  if (n_blk_stall == 0) {
    std::printf("tb_ecg_cvxif: FAIL (rao chan khong dung lan nao)\n");
    return 1;
  }
  if (n_rej == 0) {
    std::printf("tb_ecg_cvxif: FAIL (khong co lenh nao bi tu choi)\n");
    return 1;
  }
  // MOI LOADW duoc nhan phai sinh DUNG mot xung dma dung do dai. Khong co chot
  // nay thi `n_dma_ok` co the bang 0 va phep kiem tren khong bao gio chay --
  // dung lop "mot chot chi dat vi no khong bao gio duoc kich thich".
  if (!cho_dma.empty()) {
    std::printf("tb_ecg_cvxif: FAIL (%zu LOADW khong bao gio sinh xung dma)\n",
                cho_dma.size());
    return 1;
  }
  if (n_dma_ok != n_dma_expect || n_dma_pulse != n_dma_expect) {
    std::printf("tb_ecg_cvxif: FAIL (dma: %d xung, %d dung, ky vong %d)\n",
                n_dma_pulse, n_dma_ok, n_dma_expect);
    return 1;
  }
  if (errors == 0) {
    std::printf("tb_ecg_cvxif: PASS (giai ma, tu choi, rao chan, thu tu lop, va "
                "%d do dai LOADW toi dung dma_len_o)\n", n_dma_ok);
    return 0;
  }
  std::printf("tb_ecg_cvxif: FAIL (%d loi)\n", errors);
  return 1;
}

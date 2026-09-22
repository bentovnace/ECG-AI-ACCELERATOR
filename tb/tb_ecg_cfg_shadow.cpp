// Kiem chot cau hinh theo lan chay (muc EXEC-04).
//
// HAI KHANG DINH KHAC NHAU, va mot ban chot sai chi pha MOT trong hai:
//   (A) GIU:      doi `live_i` GIUA lan chay khong duoc doi `held_o`.
//   (B) DI THANG: trong CHINH chu ky `start_i`, `held_o` phai la gia tri MOI.
// (B) ton tai vi `ecg_coproc` lay mau cau hinh ngay o chu ky bat dau; mot thanh
// ghi thuan (bo phep di thang) van qua duoc (A) nhung dua gia tri cua lan chay
// TRUOC vao lan nay -- va do la mot lop loi im lang han lop loi ban dau.
#include <cstdio>
#include "Vecg_cfg_shadow.h"
#include "verilated.h"

static Vecg_cfg_shadow *dut = nullptr;
static vluint64_t nhip = 0;
static int loi = 0, n_khang_dinh = 0;
double sc_time_stamp() { return nhip; }

static void tick() {
  dut->clk_i = 0; dut->eval(); nhip++;
  dut->clk_i = 1; dut->eval(); nhip++;
}

static void bang(const char *ten, unsigned mong, unsigned duoc) {
  ++n_khang_dinh;
  if (mong != duoc) {
    ++loi;
    std::printf("  LOI %-42s mong %u  duoc %u\n", ten, mong, duoc);
  }
}

int main(int argc, char **argv) {
  Verilated::commandArgs(argc, argv);
  dut = new Vecg_cfg_shadow;

  dut->rst_ni = 0; dut->start_i = 0; dut->live_i = 0;
  tick(); tick();
  dut->rst_ni = 1;
  tick();
  bang("sau reset held_o = 0", 0, dut->held_o);

  // --- lan chay 1: bat dau voi 11 ---
  dut->live_i = 11; dut->start_i = 1; dut->eval();
  bang("(B) trong chu ky start held_o = gia tri MOI", 11, dut->held_o);
  tick();
  dut->start_i = 0; dut->eval();
  bang("ngay sau start held_o giu 11", 11, dut->held_o);

  // --- host ghi 29 GIUA lan chay: held_o phai KHONG doi ---
  dut->live_i = 29; dut->eval();
  bang("(A) doi live GIUA lan chay: held_o van 11", 11, dut->held_o);
  for (int i = 0; i < 5; ++i) {
    tick(); dut->eval();
    bang("(A) held_o van 11 qua cac chu ky sau", 11, dut->held_o);
  }

  // --- lan chay 2: start moi thi lay gia tri moi ---
  dut->start_i = 1; dut->eval();
  bang("(B) lan chay 2: trong chu ky start held_o = 29", 29, dut->held_o);
  tick();
  dut->start_i = 0; dut->live_i = 3; dut->eval();
  bang("(A) lan chay 2 giu 29 du live da ve 3", 29, dut->held_o);

  std::printf("  %d khang dinh da chay\n", n_khang_dinh);
  if (n_khang_dinh != 11) {
    std::printf("  LOI so khang dinh DA CHAY phai dung 11 -- duoc %d\n", n_khang_dinh);
    ++loi;
  }
  std::printf("tb_ecg_cfg_shadow: %s (%d loi)\n", loi ? "FAIL" : "PASS", loi);
  delete dut;
  return loi ? 2 : 0;
}

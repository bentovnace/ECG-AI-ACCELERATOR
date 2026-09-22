// Kiem UART 8-N-1 cua ecg_uart.
//
// BON PHEP KIEM, va phep thu ba la phep DUY NHAT bien minh cho thiet ke:
//   1. VONG LAP  tx_o noi vao rx_i, gui CA 256 gia tri byte
//   2. LOI KHUNG stop bit = 0 phai cho `rx_frame_err_o`, KHONG cho mot byte rac
//   3. LECH DONG HO  bo gui chay o +-N% so voi bo nhan. Day la ly do lay mau
//      GIUA bit ton tai: lay o canh thi mot lech 1 % tich luy qua 10 bit thanh
//      10 % va bit cuoi lay sai -- IM LANG, vi 8-N-1 khong co parity.
//   4. NHIEU tren start bit  mot xung ngan hon nua bit phai bi bo, khong nhan
//      mot byte rac.
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "Vecg_uart.h"
#include "verilated.h"
#include "ecg_cov.h"

static Vecg_uart *dut = nullptr;
static vluint64_t nhip = 0;
static int errors = 0, n_byte = 0;
double sc_time_stamp() { return nhip; }

// CK_MOI_BIT PHAI khop -GCK_MOI_BIT cua lenh dung. Lay tu moi truong de mot
// ban dung khac do lai duoc bien lech dong ho o do phan giai khac -- va do la
// mot phep do CAN, vi bien do la thuoc tinh cua CAU HINH chu khong cua thiet ke:
// o CKB=16 mot buoc dem la 6,25 % cua mot bit, con o 868 (100 MHz / 115200) mot
// buoc la 0,115 %. Bao mot con so bien ma khong bao CKB thi con so do vo nghia.
static const int CKB = []{ const char *e = std::getenv("ECG_UART_CKB");
                           return e ? std::atoi(e) : 16; }();

static void tick() { dut->clk_i = 0; dut->eval(); dut->clk_i = 1; dut->eval(); ++nhip; }
static void bao(const char *m) { std::printf("  SAI: %s\n", m); ++errors; }

// Bo gui MEM: dat rx_i tung bit, moi bit `ck` chu ky. `ck != CKB` = lech dong ho.
// Tra ve moi byte bo nhan bao hop le trong khoang do.
static std::vector<int> gui_mem(uint8_t b, int ck, bool stop_1 = true) {
  std::vector<int> ra;
  auto giu = [&](int muc, int n) {
    for (int i = 0; i < n; ++i) {
      dut->rx_i = muc; tick();
      if (dut->rx_valid_o) ra.push_back(dut->rx_data_o);
      if (dut->rx_frame_err_o) ra.push_back(-1);
    }
  };
  giu(0, ck);                                   // start
  for (int i = 0; i < 8; ++i) giu((b >> i) & 1, ck);   // LSB truoc
  giu(stop_1 ? 1 : 0, ck);                      // stop
  giu(1, CKB * 2);                              // ranh, de rvalid kip ve
  return ra;
}

int main(int argc, char **argv) {
  Verilated::commandArgs(argc, argv);
  dut = new Vecg_uart;
  dut->rst_ni = 0; dut->rx_i = 1; dut->tx_valid_i = 0; dut->tx_data_i = 0;
  for (int i = 0; i < 4; ++i) tick();
  dut->rst_ni = 1; tick();

  // ============================================ 1. VONG LAP tx -> rx, 256 byte
  std::printf("=== 1 · VONG LAP tx_o -> rx_i, ca 256 gia tri byte\n");
  {
    int sai = 0, nhan = 0;
    for (int v = 0; v < 256; ++v) {
      // day mot byte vao bo gui
      while (!dut->tx_ready_o) { dut->rx_i = dut->tx_o; tick(); }
      dut->tx_data_i = v; dut->tx_valid_i = 1;
      dut->rx_i = dut->tx_o; tick();
      dut->tx_valid_i = 0;
      // chay den khi bo nhan bao xong mot byte
      int got = -2;
      for (int i = 0; i < CKB * 14; ++i) {
        dut->rx_i = dut->tx_o; tick();
        if (dut->rx_valid_o) { got = dut->rx_data_o; ++nhan; break; }
        if (dut->rx_frame_err_o) { got = -1; break; }
      }
      if (got != v) { if (sai < 4) std::printf("    byte %d -> nhan %d\n", v, got); ++sai; }
      else ++n_byte;
      // ve ranh
      for (int i = 0; i < CKB * 3; ++i) { dut->rx_i = 1; tick(); }
    }
    if (sai) bao("vong lap: co byte sai");
    else std::printf("    256/256 byte dung, %d lan rx_valid\n", nhan);
  }

  // ============================================ 2. LOI KHUNG
  std::printf("=== 2 · stop bit = 0 phai cho LOI KHUNG, khong cho byte rac\n");
  {
    auto ra = gui_mem(0x5A, CKB, /*stop_1=*/false);
    if (ra.size() != 1) bao("loi khung: phai co dung mot su kien");
    else if (ra[0] != -1) bao("stop = 0 ma bao byte HOP LE -- byte rac di qua");
    else std::printf("    stop=0 -> rx_frame_err_o, khong co rx_valid_o\n");
    // va mot khung DUNG ngay sau phai lai chay
    auto ra2 = gui_mem(0x5A, CKB, true);
    if (ra2.size() != 1 || ra2[0] != 0x5A)
      bao("sau mot loi khung, khung DUNG ke tiep khong nhan duoc (may trang thai ket)");
    else { ++n_byte; std::printf("    khung dung ngay sau: nhan 0x5A\n"); }
  }

  // ============================================ 3. LECH DONG HO
  // Day la phep kiem bien minh cho viec lay mau GIUA bit.
  std::printf("=== 3 · LECH DONG HO bo gui, CK_MOI_BIT = %d (mot buoc dem = %.2f%% cua mot bit)\n", CKB, 100.0 / CKB);
  {
    std::vector<int> quet;
    for (int d = -CKB / 8; d <= CKB / 8; ++d) quet.push_back(CKB + d);
    if (quet.size() < 3) quet = {CKB - 1, CKB, CKB + 1};
    int lo = 0, hi = 0;
    for (int ck : quet) {
      const double lech = 100.0 * (ck - CKB) / CKB;
      auto ra = gui_mem(0xA7, ck);
      const bool ok = (ra.size() == 1 && ra[0] == 0xA7);
      std::printf("    %2d ck/bit (%+5.1f%%): %s\n", ck, lech,
                  ok ? "nhan 0xA7" : "SAI");
      if (ok) { ++n_byte; if (ck - CKB < lo) lo = ck - CKB; if (ck - CKB > hi) hi = ck - CKB; }
      // Bien do duoc, KHONG chot mot nguong go tay -- in ra de con so la phep DO.
    }
    std::printf("    BIEN DO DUOC o CK_MOI_BIT=%d: %+.1f%% .. %+.1f%%\n",
                CKB, 100.0 * lo / CKB, 100.0 * hi / CKB);
    if (lo == 0 && hi == 0) bao("bien lech = 0: bo nhan khong chiu duoc mot buoc dem nao");
  }

  // ============================================ 4. NHIEU tren start
  std::printf("=== 4 · xung nhieu ngan hon nua bit phai bi BO\n");
  {
    int rac = 0;
    for (int n : {1, 2, CKB / 2 - 1}) {
      for (int i = 0; i < n; ++i) { dut->rx_i = 0; tick(); }
      for (int i = 0; i < CKB * 14; ++i) {
        dut->rx_i = 1; tick();
        if (dut->rx_valid_o || dut->rx_frame_err_o) ++rac;
      }
    }
    if (rac) bao("mot xung nhieu ngan sinh ra su kien -- byte rac di qua");
    else std::printf("    ba xung nhieu (1, 2, %d chu ky): 0 su kien\n", CKB / 2 - 1);
    // va duong van song sau nhieu
    auto ra = gui_mem(0x3C, CKB);
    if (ra.size() != 1 || ra[0] != 0x3C) bao("sau nhieu, duong khong nhan duoc byte");
    else { ++n_byte; std::printf("    sau nhieu: nhan 0x3C\n"); }
  }

  dut->final();
  ECG_COV_WRITE();
  delete dut;

  // CHOT CHONG "PASS TREN 0 BYTE".
  const int TOI_THIEU = 256 + 1 + 1 + 1;   // vong lap + khung dung + it nhat 1 lech + sau nhieu
  if (n_byte < TOI_THIEU) {
    std::printf("tb_ecg_uart: FAIL (chot: %d byte dung, cho >= %d)\n", n_byte, TOI_THIEU);
    return 2;
  }
  if (errors == 0) {
    std::printf("\ntb_ecg_uart: PASS (%d byte: vong lap 256/256, loi khung khong "
                "cho byte rac, lech dong ho do duoc, nhieu ngan bi bo)\n", n_byte);
    return 0;
  }
  std::printf("\ntb_ecg_uart: FAIL (%d loi)\n", errors);
  return 1;
}

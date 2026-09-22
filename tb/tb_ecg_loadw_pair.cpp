// F03: LOADW lien tiep -- do tren HE (shim that + bo nap that), khong tren module.
//
// Ban kiem dinh 2026-09-05 ghi nhan phep sua `!dma_busy_i` (commit c3cef21) va
// noi ro: DUNG lap lai ket luan cu "LOADW hoan toan khong xet busy". Nhung con
// mot CUA SO khac, va no la mot do TRE PHAN HOI giua hai module:
//
//   `dma_start_o` la dau ra CO THANH GHI; `ecg_wmem` chi bat `busy` SAU khi lay
//   start. Nen o chu ky ngay sau khi nhan LOADW #1, shim nhin thay
//   `dma_busy_i` = 0 va nhan tiep LOADW #2.
//
// VI SAO PHEP DO CU KHONG THAY: `tb_ecg_cvxif_loadw` dat `dma_busy_i` BANG TAY,
// tuc no gia dinh phan hoi TUC THOI. Cung mot module, hai vo boc, hai ket luan
// -- va don vi o day la HE, khong MODULE.
//
// Tep nay quet KHOANG CACH 0/1/2/3 chu ky giua hai LOADW (LOAD-03), va dem ba
// dai luong khac nhau (LOAD-04): so lan NHAN, so lan PHAT start, va SO BYTE
// THAT SU duoc bo nap ghi. Mot "token tra ve" khong duoc tinh la bang chung
// trong so da nap.
#include <cstdio>
#include <cstdint>
#include "Vtb_loadw_pair_wrap.h"
#include "verilated.h"
#include "ecg_cov.h"

namespace {
Vtb_loadw_pair_wrap *dut = nullptr;

void tick() {
  dut->clk_i = 0; dut->eval();
  dut->clk_i = 1; dut->eval();
}
void settle() { dut->eval(); }

const int LOADW = 10, CLS_CTRL = 1;
uint32_t ma_lenh(int op, int fn3, int rd) {
  return (uint32_t(op) << 27) | (uint32_t(fn3) << 12) | (uint32_t(rd) << 7) | 0x0bu;
}

struct Kq { int nhan, start, byte_ghi, done; };

// MOT vong lay mau moi chu ky. Ban truoc tach `phat()` va `nhip()` thanh hai
// ham, va vi the o khoang cach 0 thi xung `dma_start_o` roi vao chu ky ma
// `phat()` da tick qua -- bang bao `nhan=1 start=0`, mot con so ve CUA SO QUAN
// SAT chu khong ve DUT. Cung ly do lam so byte ra 8 thay vi 16.
//
// Nen: dat dau vao -> eval -> LAY MAU -> tick, moi chu ky, khong ngoai le.
Kq hai_loadw(int khoang, int A, int B) {
  Kq k{0, 0, 0, 0};
  dut->rst_ni = 0; dut->iss_valid_i = 0; dut->s_valid_i = 0; dut->s_data_i = 0;
  for (int i = 0; i < 3; ++i) tick();
  dut->rst_ni = 1; tick();

  // Lich phat: LOADW #1 o chu ky 0, LOADW #2 o chu ky `khoang + 1`.
  // Neu chu ky do khong duoc nhan (`ready` = 0) thi GIU `valid` len cho den khi
  // duoc nhan -- do la dung cach mot loi CV-X-IF cu xu voi `ready` = 0, va neu
  // ta bo lenh di thi phep do se doc mot lenh BI CHAN thanh mot lenh KHONG PHAT.
  int con_phat = 2;
  int chu_ky_phat_2 = khoang + 1;
  for (int c = 0; c < 200 && con_phat >= 0; ++c) {
    const int muon_1 = (c == 0 && con_phat == 2);
    const int muon_2 = (c >= chu_ky_phat_2 && con_phat == 1);
    if (muon_1 || muon_2) {
      dut->iss_valid_i = 1;
      dut->iss_instr_i = ma_lenh(LOADW, CLS_CTRL, muon_1 ? 5 : 6);
      dut->iss_rs1_i = 0;
      dut->iss_rs2_i = uint32_t(muon_1 ? A : B);
    } else {
      dut->iss_valid_i = 0;
    }
    dut->s_valid_i = 1;              // luon co du lieu de nap
    settle();

    if (dut->iss_valid_i && dut->iss_ready_o && dut->iss_accept_o) {
      ++k.nhan;
      --con_phat;
    }
    if (dut->dma_start_o) ++k.start;
    if (dut->s_valid_i && dut->s_ready_o) k.byte_ghi += 8;
    if (dut->dma_done_o) ++k.done;
    tick();
  }
  dut->iss_valid_i = 0; dut->s_valid_i = 0;
  return k;
}
}  // namespace

double sc_time_stamp() { return 0; }

int main(int argc, char **argv) {
  Verilated::commandArgs(argc, argv);
  dut = new Vtb_loadw_pair_wrap;
  int loi = 0;
  const int A = 16, B = 24;

  std::printf("=== F03: LOADW lien tiep tren HE (ecg_cvxif + ecg_wmem that) ===\n");
  std::printf("   LOADW #1 len=%d B, LOADW #2 len=%d B\n\n", A, B);
  std::printf("   %-9s %-7s %-7s %-10s %-6s\n", "khoang", "nhan", "start",
              "byte ghi", "done");
  Kq k[4];
  for (int g = 0; g <= 3; ++g) {
    k[g] = hai_loadw(g, A, B);
    std::printf("   %-9d %-7d %-7d %-10d %-6d\n", g, k[g].nhan, k[g].start,
                k[g].byte_ghi, k[g].done);
  }

  // CHOT 1: o MOI khoang cach, so lan NHAN phai bang so lan PHAT start. Mot
  // lenh duoc nhan ma khong sinh mot chuyen la mot lenh BI NUOT -- va loi da
  // duoc bao "da nhan".
  for (int g = 0; g <= 3; ++g) {
    if (k[g].nhan != k[g].start) {
      std::printf("  CHOT: khoang=%d nhan=%d nhung start=%d -- %d lenh bi NUOT "
                  "sau khi loi da duoc bao 'da nhan'\n",
                  g, k[g].nhan, k[g].start, k[g].nhan - k[g].start);
      ++loi;
    }
  }
  // CHOT 2: neu ca hai lenh duoc nhan thi SO BYTE phai bang A+B. Dem byte THAT
  // SU ghi, khong dem token tra ve (LOAD-04).
  for (int g = 0; g <= 3; ++g) {
    const int can = (k[g].nhan == 2) ? A + B : A;
    if (k[g].byte_ghi != can) {
      std::printf("  CHOT: khoang=%d nhan=%d nen phai ghi %d B, do duoc %d B\n",
                  g, k[g].nhan, can, k[g].byte_ghi);
      ++loi;
    }
  }
  // CHOT 3: chong-rong. Neu khong khoang nao nhan duoc lenh dau thi phep do
  // khong cham vao duong LOADW va mot bang toan 0 se doc nhu "an toan".
  int nhan_it_nhat_mot = 0;
  for (int g = 0; g <= 3; ++g) if (k[g].nhan >= 1) ++nhan_it_nhat_mot;
  if (nhan_it_nhat_mot != 4) {
    std::printf("  CHOT: chi %d/4 khoang nhan duoc LOADW dau -- phep do khong "
                "cham duoc duong LOADW\n", nhan_it_nhat_mot);
    ++loi;
  }

  ECG_COV_WRITE();
  dut->final();
  delete dut;
  if (loi) { std::printf("tb_ecg_loadw_pair: FAIL (%d loi)\n", loi); return 1; }
  std::printf("tb_ecg_loadw_pair: PASS (moi khoang: nhan == start, va so byte "
              "ghi khop tong do dai)\n");
  return 0;
}

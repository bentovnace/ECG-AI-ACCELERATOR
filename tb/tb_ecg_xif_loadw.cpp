// P1-17: hai khang dinh, va chung can HAI phep do khac nhau.
//
//   (1) "decode chap nhan qua rong"  -> quet TOAN BO 256 cap (op, fn3)
//   (2) "LOADW khong backpressure"   -> LIEU la `dma_busy_i`
//
// (1) da duoc sua truoc day (loi TC2: `is_compute` dinh nghia bang PHAN BU nen
// moi cap chua dinh nghia roi vao no; nay `in_space` doi `ecg_enc_dinh_nghia`).
// Nhung "da sua" doc tu MA, con day la mot phep DO: quet ca 256 cap va doi
// chieu voi dac ta, nen mot ban sua sau nay lam rong lai se bi bat.
//
// (2) doc tu ma thi ro: `iss_ready_o` co ba nhanh, va LOADW roi vao `1'b1` --
// `dma_busy_i` CHI xuat hien trong `drained` (nhanh rao chan). Phep do duoi day
// dat `dma_busy_i` lam LIEU. Neu duong phang -- chap nhan y het khi busy va khi
// khong -- thi duong phang do MANG TIN: no la mot CHAN, cu the la "khong co
// backpressure", khac han "backpressure yeu".
#include <cstdio>
#include <cstdint>
#include <cstring>
#include "Vtb_xif_kill_wrap.h"
#include "verilated.h"
#include "ecg_cov.h"

static Vtb_xif_kill_wrap *dut = nullptr;
static int loi = 0;

static void tick() { dut->clk_i = 0; dut->eval(); dut->clk_i = 1; dut->eval(); }

static void reset() {
  dut->rst_ni = 0;
  dut->issue_valid_i = 0; dut->issue_instr_i = 0; dut->issue_id_i = 0;
  dut->issue_rs1_i = 0; dut->issue_rs2_i = 0;
  dut->commit_valid_i = 0; dut->commit_id_i = 0; dut->commit_kill_i = 0;
  dut->result_ready_i = 1;
  dut->cp_busy_i = 0; dut->cp_done_i = 0; dut->dma_busy_i = 0;
  for (int i = 0; i < 4; ++i) tick();
  dut->rst_ni = 1;
  for (int i = 0; i < 2; ++i) tick();
}

static uint32_t ma_lenh(int op, int fn3, int rd) {
  return (uint32_t(op) << 27) | (uint32_t(fn3) << 12) | (uint32_t(rd) << 7) | 0x0bu;
}

// Dac ta, chep tu ecg_pkg.sv::ecg_enc_dinh_nghia + bang opcode. Viet LAI o day
// chu khong goi RTL: mot phep doi chieu voi CHINH minh khong phan dinh gi.
static const int N_OPCODE = 12, LOADW = 10, STORE = 11;
static const int CLS_COMPUTE = 0, CLS_CTRL = 1;
static bool dac_ta_nhan(int op, int fn3) {
  if (op >= N_OPCODE) return false;
  return (op < LOADW) ? (fn3 == CLS_COMPUTE) : (fn3 == CLS_CTRL);
}

int main(int argc, char **argv) {
  Verilated::commandArgs(argc, argv);
  dut = new Vtb_xif_kill_wrap;

  // ── A: quet TOAN BO 256 cap (op, fn3) ────────────────────────────────
  int nhan = 0, tu_choi = 0, lech = 0;
  for (int op = 0; op < 32; ++op) {
    for (int fn3 = 0; fn3 < 8; ++fn3) {
      reset();
      dut->issue_valid_i = 1;
      dut->issue_instr_i = ma_lenh(op, fn3, 5);
      dut->issue_rs1_i = 1; dut->issue_rs2_i = 8;
      dut->eval();
      const int co = dut->issue_accept_o ? 1 : 0;
      const int mong = dac_ta_nhan(op, fn3) ? 1 : 0;
      if (co) ++nhan; else ++tu_choi;
      if (co != mong) {
        if (lech < 6)
          std::printf("  LECH op=%2d fn3=%d: shim %s, dac ta %s\n", op, fn3,
                      co ? "NHAN" : "tu choi", mong ? "NHAN" : "tu choi");
        ++lech;
      }
      dut->issue_valid_i = 0;
    }
  }
  std::printf("A · quet 256 cap (op,fn3): %d nhan · %d tu choi · %d LECH so voi "
              "dac ta\n", nhan, tu_choi, lech);
  // CHOT dan so: 12 opcode, moi cai DUNG mot lop hop le -> dung 12 cap duoc nhan.
  if (nhan != N_OPCODE) {
    std::printf("  CHOT: %d cap duoc nhan, dac ta co DUNG %d (mot lop moi "
                "opcode)\n", nhan, N_OPCODE);
    ++loi;
  }
  if (lech) ++loi;

  // ── B: LIEU la `dma_busy_i` tren duong LOADW ─────────────────────────
  // Phat LOADW #1 (len = 0x100), roi trong khi DMA "dang ban" phat LOADW #2
  // (len = 0x2aa). Cau hoi: #2 co duoc NHAN khong, va `dma_start_o` co no lai
  // voi do dai MOI khong -- tuc mot phep DMA dang chay co bi doi dich khong.
  std::printf("\nB · LIEU dma_busy_i tren duong LOADW\n");
  // Thu tu cot GIONG HET `tb_ecg_cvxif_loadw.cpp`. Hai tep in hai thu tu khac
  // nhau thi bo doc phai mang hai anh xa, va mot ngay nao do mot nguoi sua mot
  // ben se lam ben kia doc lech ma khong cong nao bao.
  std::printf("   %-10s %-8s %-8s %-10s %-10s\n", "dma_busy", "#2 ready",
              "#2 accept", "dma_start", "dma_len");
  int nhan_khi_ban = -1, start_khi_ban = -1, r2_khi_ban = -1;
  for (int busy = 0; busy <= 1; ++busy) {
    reset();
    // LOADW #1
    dut->issue_valid_i = 1;
    dut->issue_instr_i = ma_lenh(LOADW, CLS_CTRL, 5);
    dut->issue_rs2_i   = 0x100;
    dut->eval(); tick();
    dut->issue_valid_i = 0; dut->issue_rs2_i = 0;
    dut->eval(); tick();
    // DMA "dang ban" theo lieu
    dut->dma_busy_i = busy;
    // LOADW #2 voi do dai KHAC
    dut->issue_valid_i = 1;
    dut->issue_instr_i = ma_lenh(LOADW, CLS_CTRL, 6);
    dut->issue_rs2_i   = 0x2aa;
    dut->eval();
    const int a2 = dut->issue_accept_o ? 1 : 0;
    const int r2 = dut->issue_ready_o ? 1 : 0;
    tick();
    dut->issue_valid_i = 0;
    int st = 0, ln = 0;
    for (int i = 0; i < 6; ++i) {
      dut->eval();
      if (dut->dma_start_o) { ++st; ln = dut->dma_len_o; }
      tick();
    }
    std::printf("   %-10d %-8d %-8d %-10d 0x%-8x\n", busy, r2, a2, st, ln);
    if (busy) { nhan_khi_ban = a2; start_khi_ban = st; r2_khi_ban = r2; }
  }

  // ── PHAN DINH, va phep do B KHONG tra loi duoc cau cua P1-17 ─────────
  //
  // Do duoc: `#2 ready = 0` o CA HAI lieu, nen cai bat tay khong bao gio xay ra
  // va `dma_start` = 0 vi CVXIF CHUA HE THAY lenh thu hai. Cau noi chan no
  // truoc -- muc giu ket qua (`skid_full_q`) ha `issue_ready` cho den khi loi
  // rut ket qua cua lenh truoc.
  //
  // Nen B tra loi mot cau KHAC voi cau cua P1-17:
  //   B  : "HE co the phat hai LOADW lien tiep khi DMA dang ban khong?" -> KHONG
  //   P1-17: "CVXIF co backpressure tren duong LOADW khong?"            -> can
  //          lai CVXIF TRUC TIEP, xem tb_ecg_cvxif_loadw.cpp
  // Hai cau nay doc gan giong nhau va tra loi khac nhau. Ghi mot ket luan
  // "P1-17 khong tai lap" tu B la NOI QUA: no ket luan ve MODULE tu mot phep do
  // qua CAU, va cai chan lai la cau chu khong module.
  if (nhan_khi_ban == 1 && r2_khi_ban == 0) {
    std::printf("\n   B tra loi: HE khong phat duoc LOADW thu hai lien tiep --"
                " cau noi ha `issue_ready` (muc giu ket qua) TRUOC khi cvxif\n"
                "   thay lenh. Nen B KHONG the tra loi cau cua P1-17 ve chinh"
                " cvxif; xem `make sim-cvxif-loadw`.\n");
  } else if (nhan_khi_ban == 1 && start_khi_ban >= 1) {
    std::printf("\n   B: LOADW thu hai DI QUA duoc cau va lam `dma_start_o` no"
                " lai khi `dma_busy_i` cao -- muc lo o CAP HE.\n");
  } else {
    std::printf("\n   CHOT: B ra mot hinh dang chua duoc du tinh (nhan=%d "
                "ready=%d start=%d) -- khong ket luan\n",
                nhan_khi_ban, r2_khi_ban, start_khi_ban);
    ++loi;
  }

  ECG_COV_WRITE();
  delete dut;
  if (loi) { std::printf("tb_ecg_xif_loadw: FAIL (%d loi)\n", loi); return 1; }
  std::printf("tb_ecg_xif_loadw: PASS (A: decode dung dac ta tren 256 cap · "
              "B: he chan LOADW thu hai o CAU, nen B khong tra loi P1-17)\n");
  return 0;
}

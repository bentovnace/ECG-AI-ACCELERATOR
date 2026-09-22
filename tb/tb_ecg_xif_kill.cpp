// DO MUC LO cua `commit_kill`: khi mot lenh DA NHAN bi huy, trang thai nao cua
// shim DA DOI?
//
// Phep PHAT HIEN da co va da duoc thu ky (T5, T5a-T5d cua tb_ecg_xif_bridge):
// `kill_seen_o` bat CHI khi id bi huy khop lenh dang cho. Tep nay hoi cau KHAC
// va no khong the tra loi boi cau noi mot minh: `ecg_cvxif` hanh dong o thoi
// diem ISSUE, nen cai gi CON LAI sau mot phep huy? Vo boc
// `tb_xif_kill_wrap.sv` noi shim THAT thay vi gia lap no.
//
// KHONG phep thu nao o day noi thiet ke SAI. Muc lo la mot HE QUA da ghi cua
// mot quyet dinh kien truc ("HAN CHE DA BIET" o dau ecg_core_xif.sv), va tep
// nay bien no tu mot cau van thanh mot BANG SO. Mot han che duoc luong hoa thi
// so sanh duoc voi gia cua phep sua (39->56 LUT, 52->96 FF theo §C24); mot han
// che chi duoc ghi bang van xuoi thi khong.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include "Vtb_xif_kill_wrap.h"
#include "verilated.h"
#include "ecg_cov.h"

static Vtb_xif_kill_wrap *dut = nullptr;
static vluint64_t tick_count = 0;
static int loi = 0;

static void tick() {
  dut->clk_i = 0; dut->eval();
  dut->clk_i = 1; dut->eval();
  ++tick_count;
}

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

// major = instr[6:0] = 0b0001011; op = instr[31:27]; fn3 = instr[14:12];
// rd = instr[11:7]. Lop: op < LOADW(10) -> fn3 = COMPUTE(0); op >= 10 -> CTRL(1).
static uint32_t ma_lenh(int op, int fn3, int rd) {
  return (uint32_t(op) << 27) | (uint32_t(fn3) << 12) | (uint32_t(rd) << 7)
       | 0x0bu;
}

struct Quan_sat {
  int accepted;         // shim co nhan lenh khong
  int kill_seen;        // co dinh cua cau noi
  int cp_start_pulses;  // so lan cp_start_o bat -> mot lop CHUA COMMIT se chay
  int cp_layer;         // chi so lop bi day vao FIFO
  int dma_start_pulses; // so lan dma_start_o bat -> GHI bo nho trong so
  int dma_len;
  int result_valid;     // shim co hua mot ket qua khong
  uint32_t result_data;
};

// Phat mot lenh, roi HUY dung id do. QUAN SAT LIEN TUC tu chu ky phat.
//
// BAN TRUOC CUA HAM NAY DO SAI, va sai theo dung cai lop ta dat ten hom nay.
// No lai issue, tick, roi (neu huy) lai commit + tick, ROI moi bat dau vong
// quan sat. Nhung `dma_start_o` la mot XUNG MOT CHU KY: o nhanh CO HUY no no
// TRONG chu ky commit, tuc TRUOC khi vong quan sat bat dau. Ket qua doc ra la
// "LOADW bi huy thi DMA khong chay" -- mot ket luan ve THIET KE rut ra tu mot
// CUA SO QUAN SAT. Va no khong the dung: `ecg_cvxif` KHONG CO cong commit, nen
// mot phep huy khong the cham tro tay vao no.
// Nay moi chu ky deu duoc lay mau, va hai nhanh (huy / khong huy) chay CUNG
// mot so chu ky -- neu khong thi hai cot cua bang khong so sanh duoc.
// MUC XIF-14, truc THOI DIEM COMMIT. `tre` la so chu ky commit den SAU chu ky
// 1; `tre = 0` la hanh vi cu cua ham nay. Truc nay truoc day chua he duoc cham:
// commit bi ghim cung o `i == 1`, va tham so `3` o cho goi la ID LENH chu khong
// mot do tre -- doc luot qua thi trong nhu da co quet.
static Quan_sat phat_roi_huy(int op, int fn3, int rd, uint32_t rs1, uint32_t rs2,
                             int id, bool huy, int tre = 0) {
  Quan_sat q;
  std::memset(&q, 0, sizeof(q));
  reset();
  const int N = 16 + tre;
  for (int i = 0; i < N; ++i) {
    // chu ky 0: phat lenh. chu ky 1: huy (neu co). Con lai: yen.
    dut->issue_valid_i = (i == 0);
    dut->issue_instr_i = (i == 0) ? ma_lenh(op, fn3, rd) : 0;
    dut->issue_id_i    = id;
    dut->issue_rs1_i   = (i == 0) ? rs1 : 0;
    dut->issue_rs2_i   = (i == 0) ? rs2 : 0;
    // F11. Ban truoc lai `commit_valid` CHI o nhanh huy. Truoc F11 dieu do vo
    // hai (ecg_cvxif khong co cong commit), nhung sau F11 no lam cot KHONG-HUY
    // RONG -- va mot bang hai cot deu 0 khong phan biet duoc "thiet ke an toan"
    // voi "phep do khong quan sat dung cho".
    // CV32E40X gui commit cho MOI lenh da phat, dung mot lan; nen ca hai nhanh
    // phai co commit, va chung chi khac o `commit_kill`.
    dut->commit_valid_i = (i == 1 + tre);
    dut->commit_kill_i  = (huy && i == 1 + tre);
    dut->commit_id_i    = id;
    dut->eval();
    if (i == 0) q.accepted = dut->issue_accept_o && dut->issue_ready_o;
    // Lay mau MOI chu ky, ke ca chu ky commit.
    if (dut->cp_start_o)  { ++q.cp_start_pulses;  q.cp_layer = dut->cp_layer_o; }
    if (dut->dma_start_o) { ++q.dma_start_pulses; q.dma_len  = dut->dma_len_o; }
    if (dut->result_valid_o) { q.result_valid = 1; q.result_data = dut->result_data_o; }
    tick();
  }
  dut->eval();
  q.kill_seen = dut->kill_seen_o;
  return q;
}

static void bao(const char *ten, const Quan_sat &k, const Quan_sat &h) {
  std::printf("%-22s nhan=%d | KHONG huy: cp_start=%d lop=%d dma=%d len=%d "
              "res=%d data=%u | CO huy: cp_start=%d lop=%d dma=%d len=%d "
              "res=%d data=%u kill_seen=%d\n",
              ten, k.accepted,
              k.cp_start_pulses, k.cp_layer, k.dma_start_pulses, k.dma_len,
              k.result_valid, k.result_data,
              h.cp_start_pulses, h.cp_layer, h.dma_start_pulses, h.dma_len,
              h.result_valid, h.result_data, h.kill_seen);
}

int main(int argc, char **argv) {
  Verilated::commandArgs(argc, argv);
  dut = new Vtb_xif_kill_wrap;

  // op: CONV1D=0 ... LOADW=10, STORE=11. fn3: COMPUTE=0, CTRL=1.
  // MUC XIF-14 (danh gia 2026-09-05): bang ca duoi day quet BON LOP OPCODE voi
  // ca hai cuc (huy / khong huy). No dat VE HUY cua muc. Ba ve con lai CHUA dat va
  // co y ghi ra day: THOI DIEM commit khong duoc quet (commit phat co dinh o i==1,
  // tham so `3` la ID LENH chu khong mot do tre), va ngoai le / ngat / pipeline
  // flush khong duoc phan biet -- testbench lai `commit_kill_i` TRUC TIEP, tuc no
  // thu HOP cac nguyen nhan chu khong tung nguyen nhan. Ba canh ay sinh tu LOI nen
  // doi mot phep thu o muc loi (sim-soc), khong o muc cau noi.
  struct Ca { const char *ten; int op, fn3, rd; uint32_t rs1, rs2; };
  const Ca ca[] = {
    {"CONV1D (tinh toan)",  0, 0, 5, 0x0000002a, 0x00000000},  // lop 42
    {"FC (tinh toan)",      7, 0, 6, 0x00000011, 0x00000000},  // lop 17
    {"LOADW (dieu khien)", 10, 1, 7, 0x00000000, 0x00000456},  // 1110 B
    {"STORE (rao chan)",   11, 1, 8, 0x00000000, 0x00000000},
  };
  const int n = int(sizeof(ca) / sizeof(ca[0]));

  std::printf("=== MUC LO cua mot lenh DA NHAN bi commit_kill ===\n");
  int lo_dma = 0, lo_cp = 0, khong_thay = 0;
  for (int i = 0; i < n; ++i) {
    Quan_sat k = phat_roi_huy(ca[i].op, ca[i].fn3, ca[i].rd, ca[i].rs1,
                              ca[i].rs2, 3, false);
    Quan_sat h = phat_roi_huy(ca[i].op, ca[i].fn3, ca[i].rd, ca[i].rs1,
                              ca[i].rs2, 3, true);
    bao(ca[i].ten, k, h);

    // CHOT 1: lenh phai duoc NHAN, khong thi ca phep thu vo nghia -- mot lenh
    // bi tu choi thi khong co gi de huy, va mot bang toan 0 se doc nhu "khong
    // co muc lo".
    if (!k.accepted) {
      std::printf("  CHOT: %s KHONG duoc shim nhan -- ca phep thu nay vo nghia, "
                  "khong mot bang chung ve muc lo\n", ca[i].ten);
      ++loi;
      continue;
    }
    // CHOT 2: co dinh PHAI bat khi ta huy dung id da nhan. Khong bat thi phep
    // do khong he cham vao duong huy.
    if (!h.kill_seen) {
      std::printf("  CHOT: %s bi huy ma kill_seen_o KHONG bat -- phep huy khong "
                  "den duoc cau noi\n", ca[i].ten);
      ++loi;
      continue;
    }
    // CHOT 3 (F11): nhanh KHONG HUY phai LAM viec cua no. Neu no khong lam thi
    // cot huy toan 0 khong noi len dieu gi -- ta se dang do mot phep do da hong
    // va doc no la mot thiet ke an toan. Day la ve ma ban truoc thieu.
    const bool phai_co_cp  = (ca[i].op == 0 || ca[i].op == 7);   // CONV1D, FC
    const bool phai_co_dma = (ca[i].op == 10);                    // LOADW
    if (phai_co_cp && k.cp_start_pulses == 0) {
      std::printf("  CHOT: %s KHONG bi huy ma cp_start cung khong no -- phep do "
                  "khong quan sat dung cho, hay duong commit chua noi\n", ca[i].ten);
      ++loi;
    }
    if (phai_co_dma && k.dma_start_pulses == 0) {
      std::printf("  CHOT: %s KHONG bi huy ma dma_start cung khong no -- phep do "
                  "khong quan sat dung cho, hay duong commit chua noi\n", ca[i].ten);
      ++loi;
    }
    if (k.result_valid == 0) {
      std::printf("  CHOT: %s KHONG bi huy ma shim khong tra ket qua -- CV-X-IF "
                  "doi mot ket qua cho moi lenh da commit\n", ca[i].ten);
      ++loi;
    }

    // CHOT 4 (F11): nhanh HUY khong duoc de lai gi, VA khong duoc tra ket qua.
    if (h.dma_start_pulses > 0) { ++lo_dma;
      std::printf("  CHOT: %s bi huy ma DMA VAN ghi bo nho trong so\n", ca[i].ten);
      ++loi; }
    if (h.cp_start_pulses > 0)  { ++lo_cp;
      std::printf("  CHOT: %s bi huy ma bo dong xu ly VAN chay mot lop\n", ca[i].ten);
      ++loi; }
    if (h.result_valid != 0) {
      std::printf("  CHOT: %s bi huy ma shim VAN tra ket qua -- loi se ghi mot "
                  "gia tri cho mot lenh no da bo\n", ca[i].ten);
      ++loi;
    }
    if (h.dma_start_pulses == 0 && h.cp_start_pulses == 0
        && h.result_valid == 0) ++khong_thay;
  }

  std::printf("\n%d ca: %d ca lam DMA ghi bo nho trong so du bi huy · "
              "%d ca lam bo dong xu ly chay mot lop chua commit · "
              "%d ca khong de lai dau vet quan sat duoc\n",
              n, lo_dma, lo_cp, khong_thay);

  // F11 DAO CHIEU CHOT NAY. Truoc F11 no doi PHAI tim thay muc lo, vi mot bang
  // toan 0 luc ay khong phan biet duoc "thiet ke an toan" voi "phep do khong
  // quan sat dung cho" -- va ta biet thiet ke KHONG an toan.
  // Sau F11, hai kha nang do duoc phan biet bang CHOT 3 o tren: nhanh KHONG HUY
  // phai lam viec cua no. Nen dieu duoc doi bay gio la NGUOC LAI -- khong ca nao
  // de lai dau vet -- va phep do da co du hai cot de cau do mang tin.
  if (khong_thay != n) {
    std::printf("CHOT: chi %d/%d ca khong de lai dau vet sau khi bi huy -- F11 "
                "doi CA %d ca\n", khong_thay, n, n);
    ++loi;
  }

  // ── MUC XIF-14: QUET THOI DIEM COMMIT ────────────────────────────────────
  // Cau hoi: cua so huy co bi gioi han theo THOI GIAN khong? Tuc mot commit
  // den MUON co con huy duoc lenh khong, hay lenh da "lot" mat roi.
  //
  // Thiet ke tra loi truoc bang cau truc: `push` vao FIFO xay ra o COMMIT chu
  // khong o ISSUE (ecg_cvxif.sv:205-206). Neu dieu do dung thi `cp_start_o`
  // KHONG THE no truoc commit, va mot commit-kill o BAT KY do tre nao cung
  // chan duoc. Quet nay do dieu do thay vi tin no.
  {
    std::printf("\n=== XIF-14 quet thoi diem commit (CONV1D, id=3) ===\n");
    std::printf("   %-5s | %-22s | %s\n", "tre", "KHONG huy: cp_start",
                "CO huy: cp_start / kill_seen");
    int lech = 0, tong_khong_huy = 0;
    const int TRE_MAX = 6;
    for (int tre = 0; tre <= TRE_MAX; ++tre) {
      const Quan_sat k = phat_roi_huy(0, 0, 5, 0x0000002a, 0, 3, false, tre);
      const Quan_sat h = phat_roi_huy(0, 0, 5, 0x0000002a, 0, 3, true, tre);
      std::printf("   %-5d | %-22d | %d / %d\n", tre, k.cp_start_pulses,
                  h.cp_start_pulses, h.kill_seen);
      tong_khong_huy += k.cp_start_pulses;
      // BAT BIEN 1: huy o BAT KY do tre nao deu phai chan duoc launch.
      if (h.cp_start_pulses != 0) {
        std::printf("  CHOT XIF-14: tre=%d -- commit-kill KHONG chan duoc "
                    "launch (cp_start=%d). Cua so huy bi gioi han theo thoi "
                    "gian, tuc `push` da xay ra TRUOC commit.\n",
                    tre, h.cp_start_pulses);
        ++lech;
      }
      // BAT BIEN 2: khong huy thi phai launch DUNG MOT lan, o moi do tre.
      if (k.cp_start_pulses != 1) {
        std::printf("  CHOT XIF-14: tre=%d -- khong huy ma cp_start=%d (phai "
                    "la 1). Do tre cua commit khong duoc lam mat lenh.\n",
                    tre, k.cp_start_pulses);
        ++lech;
      }
    }
    // CHONG RONG: neu KHONG do tre nao lam lenh chay thi ca cot phai-chay dang
    // rong, va mot cot rong lam bat bien 1 dung mot cach vo nghia.
    if (tong_khong_huy == 0) {
      std::printf("  CHOT XIF-14: KHONG mot do tre nao lam lenh chay -- cot "
                  "doi chung rong, nen 'huy chan duoc' khong noi len dieu gi\n");
      ++lech;
    }
    loi += lech;
    if (!lech)
      std::printf("   XIF-14: cua so huy KHONG gioi han theo thoi gian tren "
                  "%d do tre -- `push` o COMMIT chu khong o ISSUE\n", TRE_MAX + 1);
  }

  ECG_COV_WRITE();
  delete dut;
  if (loi) {
    std::printf("tb_ecg_xif_kill: FAIL (%d loi)\n", loi);
    return 1;
  }
  std::printf("tb_ecg_xif_kill: PASS (muc lo DO DUOC tren %d lop opcode)\n", n);
  return 0;
}

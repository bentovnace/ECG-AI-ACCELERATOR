// P1-17 muc 2, do TRUC TIEP tren `ecg_cvxif` -- khong qua cau noi.
//
// VI SAO PHAI TRUC TIEP. Phep do qua cau (`make sim-xif-loadw`, muc B) cho
// `#2 ready = 0` o CA HAI lieu `dma_busy_i`, nen cai bat tay khong bao gio xay
// ra va cvxif CHUA HE THAY lenh thu hai: cau noi ha `issue_ready` vi muc giu
// ket qua con day. Do la mot cau tra loi that cho cau hoi "HE co phat duoc hai
// LOADW lien tiep khong" (KHONG), nhung no KHONG tra loi "CVXIF co backpressure
// tren duong LOADW khong". Ket luan tu B ve module se la mot ket luan ve MODULE
// rut tu mot phep do qua CAU.
//
// Doc ma thi `iss_ready_o` co ba nhanh:
//     is_barrier ? drained : is_compute ? !full : 1'b1
// va LOADW roi vao `1'b1`. `dma_busy_i` CHI xuat hien trong `drained`. Tep nay
// DO dieu do thay vi doc no.
//
// LIEU la `dma_busy_i`. Neu duong PHANG -- chap nhan y het khi busy va khi
// khong -- thi duong phang MANG TIN: no la mot CHAN, "khong co backpressure",
// khac han "backpressure yeu".
#include <cstdio>
#include <cstdint>
#include "Vecg_cvxif.h"
#include "verilated.h"
#include "ecg_cov.h"

static Vecg_cvxif *dut = nullptr;
static int loi = 0;

// F11: `ecg_cvxif` hanh dong o COMMIT, khong o issue. Tep nay lai shim TRUC
// TIEP (khong qua `ecg_xif_bridge`), nen no phai MO HINH HOA kenh commit --
// mot nhip `cmt_ok_i` MOT chu ky sau moi phep nhan, khong bao gio huy.
//
// Do la hanh vi cua CV32E40X o duong khong bi huy (`commit_valid` len o chu ky
// dau EX khong bi halt), va cau hoi cua tep nay la BACKPRESSURE cua LOADW theo
// `dma_busy_i` chu khong phai kill. Duong KILL duoc do o `tb_ecg_xif_kill`, noi
// commit duoc lai TU NGOAI va ca hai nhanh (huy / khong huy) deu co commit.
//
// Mot tb mo hinh hoa mot kenh thi phai NOI RA, khong thi phep do doc nhu mot
// phep do tren he that.
static bool cho_commit = false;   // phep nhan cua chu ky TRUOC

static void tick() {
  // MOT NHIP, khong cung nhip. Ban truoc dat `cmt_ok_i` o CUNG chu ky voi phep
  // nhan, nhung `spec_valid_q` chi len O CHINH canh do -- nen `lam_luc_nay =
  // spec_valid_q && cmt_ok_i` bang 0 ca hai chu ky va cho khong bao gio duoc
  // giai phong. Bang doc ra la "LOADW bi chan ke ca khi bo nap ranh", tuc mot
  // ket luan ve THIET KE rut ra tu mot loi TRE MOT NHIP cua chinh phep do.
  // Dung dang lai la `mh_cmt_ok <= nhan` cua vo boc SV: commit den chu ky SAU.
  const bool nhan = dut->iss_valid_i && dut->iss_ready_o && dut->iss_accept_o;
  dut->clk_i = 0; dut->eval();
  dut->cmt_ok_i = cho_commit ? 1 : 0;
  dut->clk_i = 1; dut->eval();
  cho_commit = nhan;
}

static void reset() {
  dut->rst_ni = 0;
  dut->iss_valid_i = 0; dut->iss_instr_i = 0;
  dut->iss_rs1_i = 0; dut->iss_rs2_i = 0;
  dut->cp_busy_i = 0; dut->cp_done_i = 0; dut->dma_busy_i = 0;
  dut->cmt_ok_i = 0; dut->cmt_kill_i = 0; cho_commit = false;
  for (int i = 0; i < 4; ++i) tick();
  dut->rst_ni = 1;
  for (int i = 0; i < 2; ++i) tick();
}

static const int LOADW = 10, CLS_CTRL = 1;
static uint32_t ma_lenh(int op, int fn3, int rd) {
  return (uint32_t(op) << 27) | (uint32_t(fn3) << 12) | (uint32_t(rd) << 7) | 0x0bu;
}

struct Kq { int ready, accept, start, len; };

// Phat LOADW #1 (len A), roi voi `dma_busy_i` = lieu, phat LOADW #2 (len B).
static Kq hai_loadw(int busy, int lenA, int lenB) {
  Kq k{0, 0, 0, 0};
  reset();
  // #1
  dut->iss_valid_i = 1;
  dut->iss_instr_i = ma_lenh(LOADW, CLS_CTRL, 5);
  dut->iss_rs2_i   = lenA;
  dut->eval();
  tick();
  dut->iss_valid_i = 0; dut->iss_rs2_i = 0;
  dut->eval();
  tick();
  // DMA "dang ban" theo lieu, roi #2 voi do dai KHAC
  dut->dma_busy_i  = busy;
  dut->iss_valid_i = 1;
  dut->iss_instr_i = ma_lenh(LOADW, CLS_CTRL, 6);
  dut->iss_rs2_i   = lenB;
  dut->eval();
  k.ready  = dut->iss_ready_o ? 1 : 0;
  k.accept = dut->iss_accept_o ? 1 : 0;
  tick();
  dut->iss_valid_i = 0;
  for (int i = 0; i < 6; ++i) {
    dut->eval();
    if (dut->dma_start_o) { ++k.start; k.len = dut->dma_len_o; }
    tick();
  }
  return k;
}

// ── MUC LOAD-02, ve BACKPRESSURE cua chot ────────────────────────────────────
// `iss_ready_o` cho LOADW la `!loadw_q_valid_q` (:214) -- no ha KHI HANG DOI DA
// DAY. Ve ay dung nhung TRUOC DAY khong phep thu nao canh: thay no bang `1'b1`
// thi ca ba dich LOADW van PASS.
//
// Dai luong dung de kiem KHONG phai `ready` ma la mot BAT BIEN:
//     moi lenh duoc NHAN thi phai duoc PHAT.
// Vi hop dong o day la "NHAN roi HOAN", khong "chan" -- chan o cong issue lam
// sim-soc khoa chet (CPU vua phat LOADW vua la nguon cap du lieu). Nen mot
// backpressure DUNG khong the do bang "co ha ready khong", ma bang "co lenh nao
// duoc nhan roi bien mat khong".
//
// Hang doi sau 1: #1 xuong bo nap, #2 vao hang doi, #3 PHAI bi tu choi. Neu #3
// duoc nhan thi no ghi de #2 va #2 MAT -- da nhan ma khong bao gio phat.
struct Kq3 { int nhan; int phat; int len_nhan[3]; int len_phat[3]; };

static Kq3 ba_loadw(int lenA, int lenB, int lenC) {
  Kq3 k{0, 0, {0, 0, 0}, {0, 0, 0}};
  reset();
  const int len[3] = {lenA, lenB, lenC};
  const int rd[3]  = {5, 6, 7};
  // MO HINH BO NAP, khong phai mot lieu dat tay. Lan dau viet ham nay toi dat
  // `dma_busy_i = 1` truoc khi mot `dma_start_o` nao no -- mot dau vao KHONG
  // THE XAY RA (bo nap khong the ban khi chua ai khoi dong no), va no cho ra
  // mot "phat hien" gia: lenh #1 bien mat. Bo nap that BAN VI DA NHAN MOT
  // START, nen `busy` phai la HE QUA cua `dma_start_o`, khong phai mot bien doc
  // lap toi vat len.
  bool ban = false;
  auto nhip = [&]() {
    dut->dma_busy_i = ban ? 1 : 0;
    dut->eval();
    if (dut->dma_start_o) {
      if (k.phat < 3) k.len_phat[k.phat++] = dut->dma_len_o;
      ban = true;                    // bo nap nhan start -> ban tu chu ky nay
    }
    tick();
  };
  for (int i = 0; i < 3; ++i) {
    dut->iss_valid_i = 1;
    dut->iss_instr_i = ma_lenh(LOADW, CLS_CTRL, rd[i]);
    dut->iss_rs2_i   = len[i];
    dut->dma_busy_i  = ban ? 1 : 0;
    dut->eval();
    if (dut->iss_ready_o && dut->iss_accept_o) k.len_nhan[k.nhan++] = len[i];
    nhip();
    dut->iss_valid_i = 0; dut->iss_rs2_i = 0;
    nhip();
  }
  // Tha bo nap ra: lenh trong hang doi phai duoc phat bay gio.
  ban = false;
  for (int i = 0; i < 12; ++i) nhip();
  return k;
}

int main(int argc, char **argv) {
  Verilated::commandArgs(argc, argv);
  dut = new Vecg_cvxif;

  const int A = 0x100, B = 0x2aa;
  std::printf("=== P1-17(2): LIEU `dma_busy_i` tren duong LOADW cua ecg_cvxif ===\n");
  std::printf("   LOADW #1 len=0x%x, roi LOADW #2 len=0x%x\n\n", A, B);
  std::printf("   %-9s %-7s %-8s %-10s %-9s\n", "dma_busy", "ready", "accept",
              "dma_start", "dma_len");
  Kq k[2];
  for (int busy = 0; busy <= 1; ++busy) {
    k[busy] = hai_loadw(busy, A, B);
    std::printf("   %-9d %-7d %-8d %-10d 0x%-7x\n", busy, k[busy].ready,
                k[busy].accept, k[busy].start, k[busy].len);
  }

  // CHOT 1: lenh phai duoc NHAN o ca hai lieu, khong thi phep do khong cham
  // vao duong LOADW va mot bang toan 0 se doc nhu "co backpressure".
  for (int busy = 0; busy <= 1; ++busy) {
    if (!k[busy].accept) {
      std::printf("  CHOT: dma_busy=%d -> LOADW #2 khong duoc NHAN, nen phep do "
                  "khong cham duoc duong LOADW\n", busy);
      ++loi;
    }
  }

  // CHOT 2: HOP DONG LA "NHAN ROI HOAN", khong "chan".
  //
  // Ban truoc chot nay doi `ready` = 0 khi ban -- do la hop dong cua phep sua
  // 2026-09-04, va no SAI. Chan o cong issue lam `sim-soc` hong hoan toan: CPU
  // vua phat LOADW vua la NGUON CAP du lieu cho bo nap, nen chan lenh thi CPU
  // dung, khong ai day du lieu, `busy` khong bao gio ha -- khoa chet.
  //
  // Hop dong dung (F03, LOAD-02): LOADW luon duoc NHAN (hang doi 1 sau), nhung
  // `dma_start_o` KHONG duoc no khi bo nap dang ban -- lenh nam trong hang doi
  // va duoc phat khi bo nap ranh. Tuc dai luong phai kiem la `start`, khong
  // `ready`.
  if (k[1].start != 0) {
    std::printf("  CHOT: dma_busy=1 ma `dma_start`=%d -- lenh duoc PHAT trong "
                "luc bo nap dang ban. `ecg_wmem` doi `!busy_q` de chot mot "
                "chuyen moi nen lenh do bi BO IM LANG sau khi loi da duoc bao "
                "'da nhan'.\n", k[1].start);
    ++loi;
  }
  if (k[1].ready != 1) {
    std::printf("  CHOT: dma_busy=1 ma `ready`=%d -- LOADW bi CHAN thay vi xep "
                "hang. Chan o cong issue lam CPU dung, ma CPU la nguon cap du "
                "lieu cho bo nap -> khoa chet (do duoc: sim-soc hong).\n",
                k[1].ready);
    ++loi;
  }
  if (k[0].ready != 1 || k[0].start < 1) {
    std::printf("  CHOT: dma_busy=0 ma `ready`=%d `dma_start`=%d -- LOADW bi "
                "chan ke ca khi bo nap ranh\n", k[0].ready, k[0].start);
    ++loi;
  }

  const int phang = (k[0].ready == k[1].ready) && (k[0].start == k[1].start)
                 && (k[0].len == k[1].len);
  if (loi == 0 && phang && k[1].ready == 1 && k[1].start >= 1) {
    std::printf("\n   XAC NHAN P1-17(2). Duong PHANG theo lieu: `dma_busy_i` "
                "khong doi mot bit nao cua duong LOADW.\n"
                "   LOADW thu hai duoc nhan (`ready`=1) va lam `dma_start_o` no "
                "lai voi do dai MOI (0x%x) TRONG KHI DMA dang ban.\n"
                "   Tuc mot phep nap trong so dang chay bi DOI DICH. Va duong "
                "phang o day MANG TIN: khong phai backpressure YEU ma\n"
                "   KHONG CO backpressure -- `dma_busy_i` chi xuat hien trong "
                "`drained` (nhanh rao chan) cua `iss_ready_o`.\n", k[1].len);
  } else if (loi == 0 && k[1].ready == 0) {
    std::printf("\n   P1-17(2) KHONG tai lap tren chinh module: `ready`=0 khi "
                "DMA ban, tuc CO backpressure. Bao cao ngoai lac hau.\n");
  } else if (loi == 0 && !phang) {
    std::printf("\n   Duong KHONG phang: `dma_busy_i` doi hanh vi. Backpressure "
                "co mot phan -- xem hai hang tren de biet doi cai gi.\n");
  }

  // ── CHOT 3 -- MUC LOAD-02, ve BACKPRESSURE cua chot ────────────────────────
  // Bat bien: MOI LENH DUOC NHAN THI PHAI DUOC PHAT. Khong kiem `ready` truc
  // tiep, vi hop dong o day la "NHAN roi HOAN" chu khong "chan" -- mot phep do
  // doi `ready`=0 se doi dung cai phep sua 2026-09-04 da lam sim-soc khoa chet.
  // Kiem cai HE QUA QUAN SAT DUOC cua backpressure thay vi kiem co che cua no.
  {
    const int A3 = 0x110, B3 = 0x220, C3 = 0x330;
    const Kq3 k3 = ba_loadw(A3, B3, C3);
    std::printf("\n   LOAD-02 hang doi sau 1: NHAN %d lenh, PHAT %d lenh\n",
                k3.nhan, k3.phat);
    for (int i = 0; i < k3.nhan; ++i)
      std::printf("      nhan[%d] = 0x%x\n", i, k3.len_nhan[i]);
    for (int i = 0; i < k3.phat; ++i)
      std::printf("      phat[%d] = 0x%x\n", i, k3.len_phat[i]);

    // CHONG RONG: khong nhan lenh nao thi phep do khong cham duong LOADW, va
    // mot bang rong se doc nhu "khong lenh nao mat".
    if (k3.nhan == 0) {
      std::printf("  CHOT: KHONG lenh nao duoc NHAN -- phep do khong cham duoc "
                  "duong LOADW, nen no khong tra loi duoc cau hoi nao\n");
      ++loi;
    }
    // DOI CHUNG: hai lenh dau PHAI duoc nhan (hang doi sau 1 = mot dang chay +
    // mot dang cho). Neu chung bi tu choi thi phep do dang do mot cai khac.
    if (k3.nhan < 2) {
      std::printf("  CHOT: chi %d/2 lenh dau duoc nhan -- hang doi sau 1 dang "
                  "khong nhan du, phep do khong con la phep do ve LOADW #3\n",
                  k3.nhan);
      ++loi;
    }
    // BAT BIEN CHINH: moi do dai da NHAN phai xuat hien trong cac lenh da PHAT.
    for (int i = 0; i < k3.nhan; ++i) {
      bool thay = false;
      for (int j = 0; j < k3.phat; ++j)
        if (k3.len_phat[j] == k3.len_nhan[i]) thay = true;
      if (!thay) {
        std::printf("  CHOT LOAD-02: LOADW len=0x%x da duoc NHAN nhung KHONG "
                    "BAO GIO duoc PHAT -- mot lenh bien mat trong im lang. "
                    "`iss_ready_o` phai ha khi hang doi day (ecg_cvxif.sv:214); "
                    "neu no luon 1 thi lenh thu ba GHI DE lenh dang cho.\n",
                    k3.len_nhan[i]);
        ++loi;
      }
    }
    if (k3.nhan != k3.phat) {
      std::printf("  CHOT LOAD-02: nhan %d nhung phat %d -- so lenh vao khac so "
                  "lenh ra\n", k3.nhan, k3.phat);
      ++loi;
    }
  }

  // ── CHOT 4 -- MUC EXEC-09, canh "FIFO CON DU LIEU CU" ─────────────────────
  // Ba canh kia cua EXEC-09 do o `tb_ecg_negative` (coproc). Canh nay o DAY vi
  // FIFO nam trong `ecg_cvxif`.
  //
  // PHAI CO LAP DUNG `empty`. `drained` co NAM ve:
  //   empty && !cp_busy_i && !dma_busy_i && !start_q && !spec_valid_q   (:202)
  // Ban dau toi giu `cp_busy_i = 1` roi ket luan "FIFO con du lieu chan duoc
  // rao chan". SAI: voi cp_busy_i = 1 thi rao chan bi tu choi DU FIFO rong hay
  // khong, nen phep do ay do ve `!cp_busy_i` chu khong ve `empty`. Do duoc:
  // bo `empty` khoi `drained` KHONG lam ket qua doi mot chut nao.
  //
  // Co lap: giu `cp_busy_i = 0` SUOT. Sau khi mot COMPUTE duoc commit, FIFO
  // KHONG rong trong vai chu ky truoc khi `pop` rut no ra. Dem so chu ky rao
  // chan bi tu choi trong khoang ay -- do la so chu ky ma DUY NHAT `empty`
  // (hoac `start_q` sinh ra tu chinh phep pop) dang chan.
  {
    reset();
    dut->cp_busy_i = 0;
    bool cho_xong = false;
    auto nhip = [&]() {
      dut->cp_done_i = cho_xong ? 1 : 0;
      dut->eval();
      const bool bat_dau = dut->cp_start_o != 0;
      tick();
      cho_xong = bat_dau;
    };

    dut->iss_valid_i = 1;
    dut->iss_instr_i = ma_lenh(0, 0, 5);      // COMPUTE
    dut->iss_rs2_i   = 0;
    dut->eval();
    nhip();                                   // nhan
    dut->iss_valid_i = 0;
    nhip();                                   // commit -> push

    // Giu rao chan valid LIEN TUC va dem so chu ky no bi tu choi.
    int bi_tu_choi = 0, duoc_nhan = 0;
    dut->iss_valid_i = 1;
    dut->iss_instr_i = ma_lenh(11, 1, 0);     // rao chan
    for (int i = 0; i < 16; ++i) {
      dut->eval();
      if (dut->iss_ready_o) { ++duoc_nhan; break; }
      ++bi_tu_choi;
      nhip();
    }
    dut->iss_valid_i = 0;
    dut->cp_done_i = 0;

    std::printf("\n   EXEC-09c FIFO con du lieu cu (cp_busy_i = 0 SUOT): rao chan "
                "bi tu choi %d chu ky roi duoc nhan = %d\n",
                bi_tu_choi, duoc_nhan);
    if (bi_tu_choi < 1) {
      std::printf("  CHOT EXEC-09c: rao chan duoc nhan NGAY trong khi FIFO con "
                  "du lieu -- mot luot moi co the bat dau khi luot cu chua ra "
                  "het.\n");
      ++loi;
    }
    // DOI CHUNG: cuoi cung no PHAI duoc nhan. Mot rao chan khong bao gio duoc
    // nhan cung cho `bi_tu_choi >= 1`, va luc do so tren khong noi len dieu gi.
    if (duoc_nhan != 1) {
      std::printf("  CHOT EXEC-09c: DOI CHUNG HONG -- rao chan KHONG BAO GIO "
                  "duoc nhan trong 16 chu ky, nen 'bi tu choi' khong phan biet "
                  "duoc voi mot rao chan chet.\n");
      ++loi;
    }
  }

  ECG_COV_WRITE();
  delete dut;
  if (loi) { std::printf("tb_ecg_cvxif_loadw: FAIL (%d loi)\n", loi); return 1; }
  std::printf("tb_ecg_cvxif_loadw: PASS (hop dong NHAN-ROI-HOAN: ready=1 o ca "
              "hai lieu, nhung dma_start chi no khi bo nap ranh)\n");
  return 0;
}

// Kiem giao thuc APB4 slave -> OBI cua ecg_apb_slave.
//
// PHAM VI, noi truoc: tep nay kiem GIAO THUC, khong kiem noi dung thanh ghi. Phia
// OBI la mot mo hinh hanh vi trong C++ (`gnt` va `rvalid` do phep kiem dieu khien
// duoc), vi dieu can bat o day la nhung ca ma ecg_mmio KHONG BAO GIO sinh ra:
// `gnt` bi giu, `rvalid` den muon. Noi voi ecg_mmio that thi `gnt` noi cung 1 nen
// nhanh S_REQ khong bao gio duoc chay, va mot nhanh khong bao gio chay la mot
// nhanh khong duoc kiem. Phep kiem duong THAT (APB -> ecg_mmio -> thanh ghi) la
// tb_ecg_mmio sau khi ghep, khong phai tep nay.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "Vecg_apb_slave.h"
#include "verilated.h"
#include "ecg_cov.h"

static Vecg_apb_slave *dut = nullptr;
static vluint64_t nhip = 0;
static int errors = 0;
static int n_phep = 0;   // so phep truyen DA CHAY -- chot chong "PASS tren 0 phep"

double sc_time_stamp() { return nhip; }

// Mo hinh OBI hanh vi. `gnt_tre` = so chu ky giu `gnt` xuong; `rvalid_tre` = so
// chu ky tu luc co gnt den luc `rvalid` len.
static int gnt_tre = 0, rvalid_tre = 1;
static int gnt_dem = 0, rv_dem = -1;
static uint32_t obi_rdata = 0;
static uint32_t obi_ghi_addr = 0, obi_ghi_data = 0;
static int obi_ghi_be = 0, obi_so_ghi = 0, obi_so_doc = 0;
// `pslverr` lay o CHINH chu ky `pready` len: hai tin hieu nay to hop tu
// cung mot trang thai, doc o chu ky khac la doc ra 0 vo nghia.
static int apb_slverr = 0;

static void tick() {
  // THU TU LAY MAU LA PHAN QUAN TRONG CUA TEP NAY, va ban dau toi lam sai no:
  // toi doc `req_o` SAU canh len, nhung `req_o` la mot XUNG MOT CHU KY sinh to
  // hop tu `psel && penable && tt_q == S_IDLE`. Sau canh len thi `tt_q` da roi
  // S_IDLE nen `req_o` da ve 0, va mo hinh OBI khong thay phep truyen nao ->
  // pha 1 bao "khong thay dung MOT phep ghi" va pha 2 TREO vi khong ai bat
  // `rvalid`. Ca hai la loi HARNESS, khong phai loi thiet ke.
  //
  // Nen: dat dau vao, cho to hop lang o PHA THAP, LAY MAU o do, roi moi danh
  // canh len. Day la thu tu duy nhat doc dung mot xung mot chu ky.
  dut->clk_i = 0;
  dut->gnt_i = (gnt_dem >= gnt_tre) ? 1 : 0;
  dut->rvalid_i = (rv_dem == 0) ? 1 : 0;
  dut->rdata_i = obi_rdata;
  dut->eval();

  const bool s_req = dut->req_o, s_we = dut->we_o, s_gnt = dut->gnt_i;
  const uint32_t s_addr = dut->addr_o, s_wdata = dut->wdata_o;
  const int s_be = dut->be_o;

  dut->clk_i = 1; dut->eval();
  ++nhip;

  if (s_req) {
    if (s_gnt) {
      if (s_we) {
        obi_ghi_addr = s_addr; obi_ghi_data = s_wdata;
        obi_ghi_be = s_be; ++obi_so_ghi;
        rv_dem = -1;                 // GHI khong sinh rvalid (nhu ecg_mmio)
      } else {
        ++obi_so_doc; rv_dem = rvalid_tre;
      }
      gnt_dem = 0;
    } else {
      ++gnt_dem;
    }
  }
  if (rv_dem > 0) --rv_dem;
  else if (rv_dem == 0) rv_dem = -1;
}

static void reset() {
  dut->rst_ni = 0; dut->psel_i = 0; dut->penable_i = 0; dut->pwrite_i = 0;
  dut->paddr_i = 0; dut->pwdata_i = 0; dut->pstrb_i = 0;
  dut->gnt_i = 1; dut->rvalid_i = 0; dut->rdata_i = 0;
  for (int i = 0; i < 3; ++i) tick();
  dut->rst_ni = 1; tick();
}

static void bao(const char *m) { std::printf("  SAI: %s\n", m); ++errors; }

// Mot phep truyen APB day du. Tra ve so chu ky tu SETUP den `pready`.
static int apb(bool ghi, uint32_t addr, uint32_t data, uint32_t strb,
               uint32_t *ra = nullptr) {
  // --- pha SETUP: psel len, penable XUONG ---
  dut->psel_i = 1; dut->penable_i = 0; dut->pwrite_i = ghi;
  dut->paddr_i = addr; dut->pwdata_i = data; dut->pstrb_i = strb;
  tick();
  if (dut->pready_o) bao("pready len o pha SETUP (phai cho ACCESS)");
  // --- pha ACCESS: penable len, giu den khi pready ---
  dut->penable_i = 1;
  int ck = 1;
  for (int i = 0; i < 64; ++i) {
    tick(); ++ck;
    if (dut->pready_o) {
      apb_slverr = dut->pslverr_o;
      if (ra) *ra = dut->prdata_o;
      // pready phai la MOT chu ky: kiem ngay o chu ky sau.
      dut->psel_i = 0; dut->penable_i = 0;
      tick();
      if (dut->pready_o) bao("pready giu HAI chu ky -- master se tuong hai phep truyen");
      ++n_phep;
      return ck;
    }
  }
  bao("pready khong bao gio len trong 64 chu ky (TREO)");
  dut->psel_i = 0; dut->penable_i = 0;
  return -1;
}

int main(int argc, char **argv) {
  Verilated::commandArgs(argc, argv);
  dut = new Vecg_apb_slave;
  reset();

  // ===================================================== PHA 1 · ghi co ban
  std::printf("=== PHA 1 · GHI, va cai bay chinh: ghi KHONG duoc cho rvalid\n");
  gnt_tre = 0; rvalid_tre = 1;
  int truoc = obi_so_ghi;
  int ck = apb(true, 0x0000000C, 0x00000025, 0xF);   // NLAYERS = 37
  if (ck < 0) bao("phep GHI treo -- rat co the may trang thai cho rvalid tren ca ghi");
  if (obi_so_ghi != truoc + 1) bao("phia OBI khong thay dung MOT phep ghi");
  if (obi_ghi_addr != 0x0C) bao("dia chi OBI sai");
  if (obi_ghi_data != 0x25) bao("du lieu OBI sai");
  if (obi_ghi_be != 0xF)    bao("be OBI sai");
  std::printf("    ghi xong trong %d chu ky (SETUP + ACCESS)\n", ck);

  // ===================================================== PHA 2 · doc
  std::printf("=== PHA 2 · DOC, rvalid den sau mot chu ky\n");
  obi_rdata = 0xDEADBEEF;
  uint32_t ra = 0;
  ck = apb(false, 0x00000004, 0, 0xF, &ra);
  if (ck < 0) bao("phep DOC treo");
  if (ra != 0xDEADBEEF) bao("prdata khong bang rdata cua OBI");
  std::printf("    doc xong trong %d chu ky, prdata = 0x%08X\n", ck, ra);

  // ===================================================== PHA 3 · gnt bi giu
  // ecg_mmio noi cung gnt = 1 nen nhanh nay KHONG BAO GIO chay o duong that.
  // Mot nhanh khong bao gio chay la mot nhanh khong duoc kiem, nen kiem o day.
  std::printf("=== PHA 3 · `gnt` bi giu 3 chu ky (nhanh ecg_mmio khong sinh ra)\n");
  for (int g = 1; g <= 3; ++g) {
    gnt_tre = g;
    int ckw = apb(true, 0x00000010, 0x00000103 + g, 0xF);
    int ckr = apb(false, 0x00000004, 0, 0xF, &ra);
    if (ckw < 0 || ckr < 0) bao("treo khi gnt bi giu");
    std::printf("    gnt_tre=%d  ghi %d ck  doc %d ck\n", g, ckw, ckr);
  }
  gnt_tre = 0;

  // ===================================================== PHA 4 · rvalid muon
  std::printf("=== PHA 4 · `rvalid` den muon (1..4 chu ky)\n");
  for (int r = 1; r <= 4; ++r) {
    rvalid_tre = r;
    obi_rdata = 0x1000 + r;
    ck = apb(false, 0x00000018, 0, 0xF, &ra);
    if (ck < 0) bao("treo khi rvalid muon");
    if (ra != (uint32_t)(0x1000 + r)) bao("prdata sai khi rvalid muon");
    std::printf("    rvalid_tre=%d  doc %d ck  prdata = 0x%X\n", r, ck, ra);
  }
  rvalid_tre = 1;

  // ===================================================== PHA 5 · pstrb
  // W1-D (F07+F08) DOI HOP DONG O DAY. Ban truoc doi `pstrb == 0` phai thanh
  // `be = 1111`, lap luan la "master APB3 de pstrb '0 nen coi la ca tu".
  //
  // Lap luan do sai theo hai duong. Mot: no quyet dinh theo GIA TRI RUNTIME --
  // doan y dinh cua master tu mot gia tri hop le co nghia NGUOC LAI (mat na
  // rong nghia la khong ghi byte nao). Hai: no bien mot phep khong-ghi thanh
  // mot phep ghi du bon byte, va do duoc (`tb_ecg_apb_mmio`) no cham duoc bit
  // START cua CTRL trong khi bus bao THANH CONG.
  //
  // Hop dong moi: `pstrb != 4'hF` tren GHI => KHONG giao dich OBI nao, va
  // `pslverr = 1`. Muon do mot master APB3 thi lam wrapper/parameter rieng.
  //
  // Do bang `obi_so_ghi` chu khong bang `obi_ghi_be`: cau hoi la "co phep ghi
  // nao di ra khong", va `obi_ghi_be` chi giu gia tri phep ghi TRUOC nen no tra
  // loi duoc ca khi khong co phep ghi nao -- mot dau hieu khop ca hai kha nang.
  std::printf("=== PHA 5 · pstrb != 0xF tren GHI: khong giao dich + pslverr\n");
  int n_mat_na = 0, n_qua = 0, n_tu_choi = 0;
  for (uint32_t m = 0; m < 16; ++m) {
    ++n_mat_na;
    const int truoc_ghi = obi_so_ghi;
    apb(true, 0x00000008, 0x00000007, m);
    const int them = obi_so_ghi - truoc_ghi;
    if (m == 0xF) {
      if (them != 1)      bao("pstrb = 0xF bi tu choi -- mot phep ghi HOP LE bi mat");
      if (obi_ghi_be != 0xF) bao("pstrb = 0xF khong di qua nguyen ven");
      if (apb_slverr)     bao("pstrb = 0xF ma pslverr = 1 -- bao loi cho phep ghi hop le");
      ++n_qua;
    } else {
      if (them != 0)      bao("pstrb khong day tu VAN phat mot giao dich OBI");
      if (!apb_slverr)    bao("pstrb khong day tu ma pslverr = 0 -- bus bao THANH CONG");
      ++n_tu_choi;
    }
  }
  // MUC APB-03 / APB-05 (danh gia 2026-09-05): du CA 16 mat na.
  // BAN TRUOC IN MOT CHUOI GHIM CUNG "16/16" va dat loi khai trong mot CHU THICH.
  // Ca hai deu khong phai mot phep do: doi vong lap thanh `m < 1` thi phep thu VAN
  // PASS va VAN in dung cau "16/16" -- 15 phep truyen bien mat ma khong ai bat.
  // Mot phep thu KHANG DINH mot do phu no khong co la te hon mot phep thu im lang.
  // Gio ba con so duoc DEM va IN, va chung phai khop mot cach chinh xac.
  if (n_mat_na != 16)
    bao("PHA 5 khong chay du 16 mat na");
  if (n_qua != 1)
    bao("PHA 5: so mat na DI QUA phai dung 1 (chi 0xF)");
  if (n_tu_choi != 15)
    bao("PHA 5: so mat na BI TU CHOI phai dung 15");
  std::printf("    %d/16 mat na da chay: %d di qua, %d bi tu choi\n",
              n_mat_na, n_qua, n_tu_choi);
  // MUC APB-05 (danh gia 2026-09-05): hai lua chon la "chi nhan ca tu" hay
  // "gop byte dung nghia"; du an CHON "chi nhan ca tu", va vong lap tren cuong
  // che lua chon do tren CA 16 mat na -- nen mot cai dat gop byte (vi du chap
  // nhan pstrb=0x3) se lam ca nay DO.
  // MUC APB-07 (danh gia 2026-09-05): giao thuc APB duoc do rieng -- pha SETUP
  // khong duoc len `pready` (dong ~94), `pready` chi mot chu ky, timeout treo, va
  // mot loat giao dich lien tiep tron doc/ghi. `n_phep` o dau tep la chot chong
  // "PASS tren 0 phep truyen".
  // DOC thi `pstrb` khong co nghia va KHONG duoc chan.
  {
    const int truoc_doc = obi_so_doc;
    uint32_t ra5 = 0;
    obi_rdata = 0xC0FFEE01;
    apb(false, 0x00000018, 0, 0x0, &ra5);
    if (obi_so_doc - truoc_doc != 1) bao("phep DOC bi chan vi pstrb -- pstrb chi co nghia khi GHI");
    if (ra5 != 0xC0FFEE01)           bao("prdata sai o phep doc voi pstrb = 0");
    if (apb_slverr)                  bao("phep DOC bi bao pslverr");
    std::printf("    doc voi pstrb=0 van di qua, prdata = 0x%X\n", ra5);
  }

  // ===================================================== PHA 6 · lien tiep
  std::printf("=== PHA 6 · 64 phep truyen lien tiep, xen ke ghi/doc\n");
  int g0 = obi_so_ghi, d0 = obi_so_doc;
  for (int i = 0; i < 64; ++i) {
    if (i & 1) {
      obi_rdata = 0xA000 + i;
      apb(false, 0x00000018, 0, 0xF, &ra);
      if (ra != (uint32_t)(0xA000 + i)) bao("prdata sai trong chuoi lien tiep");
    } else {
      apb(true, 0x00000028, 0x00010000 + i, 0xF);
      if (obi_ghi_data != (uint32_t)(0x00010000 + i))
        bao("wdata sai trong chuoi lien tiep");
    }
  }
  if (obi_so_ghi - g0 != 32) bao("so phep ghi OBI khong bang 32");
  if (obi_so_doc - d0 != 32) bao("so phep doc OBI khong bang 32");
  std::printf("    32 ghi + 32 doc, khop\n");

  // ===================================================== PHA 7 · khong req roi
  // `req` khong duoc phat khi khong co phep truy cap APB nao: mot `req` roi se
  // ghi vao thanh ghi mot cach im lang.
  std::printf("=== PHA 7 · khong co `req` roi khi psel xuong\n");
  dut->psel_i = 0; dut->penable_i = 0;
  int gt = obi_so_ghi, dt = obi_so_doc;
  for (int i = 0; i < 32; ++i) {
    tick();
    if (dut->req_o) bao("`req` len khi psel xuong -- mot phep ghi im lang");
  }
  if (obi_so_ghi != gt || obi_so_doc != dt) bao("OBI thay phep truyen khi psel xuong");
  std::printf("    32 chu ky psel=0: 0 req\n");

  dut->final();
  ECG_COV_WRITE();
  delete dut;

  // CHOT CHONG "PASS TREN 0 PHEP". Bai hoc 04c4fbf: mot cau PASS duoc luong hoa
  // bang mot con so ma dieu kien PASS khong kiem thi la mot cong khong the that
  // bai. So phep truyen toi thieu suy tu chinh cac pha o tren: 1 + 1 + 6 + 4 + 2
  // + 64 = 78.
  const int TOI_THIEU = 78;
  if (n_phep < TOI_THIEU) {
    std::printf("tb_ecg_apb: FAIL (chot: chay %d phep truyen, cho >= %d)\n",
                n_phep, TOI_THIEU);
    return 2;
  }
  if (errors == 0) {
    std::printf("\ntb_ecg_apb: PASS (%d phep truyen APB4: ghi khong cho rvalid, "
                "doc cho dung rvalid, pready dung MOT chu ky, gnt giu va rvalid "
                "muon deu khong treo)\n", n_phep);
    return 0;
  }
  std::printf("\ntb_ecg_apb: FAIL (%d loi tren %d phep truyen)\n", errors, n_phep);
  return 1;
}

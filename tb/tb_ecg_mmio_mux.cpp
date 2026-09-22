// Kiem bo trong tai hai master cua khoi thanh ghi.
//
// LOP LOI CHINH ma tep nay ton tai de bat: `rvalid` cua ecg_mmio den SAU MOT chu
// ky, va den luc do master da req o chu ky truoc CO THE da ha req, hay master
// kia da len. Neu dinh tuyen `rvalid` theo ai dang req O CHU KY HIEN TAI thi du
// lieu doc ve SAI MASTER trong im lang. Va lop loi do KHONG lo ra o mot phep
// kiem mot-master-mot-luc -- phai co hai master req sat nhau moi thay.
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "Vecg_mmio_mux.h"
#include "verilated.h"
#include "ecg_cov.h"

static Vecg_mmio_mux *dut = nullptr;
static vluint64_t nhip = 0;
static int errors = 0, n_giao_dich = 0;
static int a_yeu_cau = 0, b_yeu_cau = 0, a_phan_hoi = 0, b_phan_hoi = 0;
double sc_time_stamp() { return nhip; }

// Slave hanh vi PHAI KHOP ecg_mmio, va ban dau cua toi KHONG khop: no chi sinh
// `rvalid` cho phep DOC. `ecg_mmio.sv:587` co dong `// rvalid cho CA HAI chieu`
// -- khoi thanh ghi bat `rvalid` cho ca GHI. Mo hinh de dai hon slave that khong
// lam phep kiem sai, nhung lam no KHONG THE phat hien viec dinh tuyen `rvalid`
// cua mot phep GHI sai master -- va do dung la lop loi da lam CV32E40X treo.
// Nay mo hinh sinh rvalid cho CA HAI chieu.
static int rv_dem = -1;
static uint32_t s_rdata = 0;
static uint32_t o_bo_nho[64];

struct Mau { bool a, b; uint32_t data; };
static std::vector<Mau> ve;   // rvalid da ve master nao, voi du lieu gi

static void tick() {
  dut->clk_i = 0;
  dut->s_gnt_i   = 1;
  dut->s_rvalid_i = (rv_dem == 0) ? 1 : 0;
  dut->s_rdata_i  = s_rdata;
  dut->eval();

  const bool req = dut->s_req_o, we = dut->s_we_o;
  const uint32_t ad = dut->s_addr_o, wd = dut->s_wdata_o;
  // PHEP KIEM BAO TOAN. Day la chot ma phep kiem nay THIEU o ban dau, va vi
  // thieu no thi mot ban `!s_we_o` (dinh tuyen sai `rvalid` cua phep GHI) di qua
  // duoc CA phep kiem: toi chi kiem phep ghi co vao bo nho, khong kiem master
  // nao NHAN LAI phan hoi. `ecg_mmio` bat `rvalid` cho CA HAI chieu, nen moi
  // yeu cau duoc cap phai tra dung MOT rvalid cho DUNG master do -- ke ca ghi.
  if (dut->a_gnt_o) ++a_yeu_cau;
  if (dut->b_gnt_o) ++b_yeu_cau;
  if (dut->a_rvalid_o) ++a_phan_hoi;
  if (dut->b_rvalid_o) ++b_phan_hoi;
  // Ghi lai rvalid da ve dau -- lay o PHA THAP vi day la to hop.
  if (dut->s_rvalid_i)
    ve.push_back({(bool)dut->a_rvalid_o, (bool)dut->b_rvalid_o,
                  (uint32_t)dut->a_rdata_o});

  dut->clk_i = 1; dut->eval(); ++nhip;

  if (req) {
    ++n_giao_dich;
    const uint32_t idx = (ad >> 2) & 63u;
    if (we) { o_bo_nho[idx] = wd; rv_dem = 1; }   // rvalid cho CA HAI chieu
    else    { s_rdata = o_bo_nho[idx]; rv_dem = 1; }
  }
  if (rv_dem > 0) --rv_dem; else if (rv_dem == 0) rv_dem = -1;
}

static void ranh() {
  dut->a_req_i = 0; dut->b_req_i = 0; dut->a_we_i = 0; dut->b_we_i = 0;
  dut->a_be_i = 0xF; dut->b_be_i = 0xF;
}
static void bao(const char *m) { std::printf("  SAI: %s\n", m); ++errors; }

int main(int argc, char **argv) {
  Verilated::commandArgs(argc, argv);
  dut = new Vecg_mmio_mux;
  dut->rst_ni = 0; ranh();
  for (int i = 0; i < 3; ++i) tick();
  dut->rst_ni = 1; tick();

  // ============================================ PHA 1 · mot master mot luc
  std::printf("=== PHA 1 · tung master rieng: ghi roi doc lai\n");
  for (int m = 0; m < 2; ++m) {
    const uint32_t gt = 0xA5000000u + m;
    if (m == 0) { dut->a_req_i = 1; dut->a_we_i = 1; dut->a_addr_i = 0x10; dut->a_wdata_i = gt; }
    else        { dut->b_req_i = 1; dut->b_we_i = 1; dut->b_addr_i = 0x10; dut->b_wdata_i = gt; }
    tick(); ranh(); tick();
    ve.clear();
    if (m == 0) { dut->a_req_i = 1; dut->a_we_i = 0; dut->a_addr_i = 0x10; }
    else        { dut->b_req_i = 1; dut->b_we_i = 0; dut->b_addr_i = 0x10; }
    tick(); ranh(); tick(); tick();
    if (ve.size() != 1) bao("phai co DUNG mot rvalid");
    else if (m == 0 && (!ve[0].a || ve[0].b)) bao("rvalid phai ve A, khong ve B");
    else if (m == 1 && (ve[0].a || !ve[0].b)) bao("rvalid phai ve B, khong ve A");
    else if (ve[0].data != gt) bao("rdata sai");
    else std::printf("    master %c: ghi roi doc lai 0x%08X, rvalid ve dung master\n",
                     m ? 'B' : 'A', gt);
  }

  // ============================================ PHA 2 · UU TIEN
  std::printf("=== PHA 2 · hai master req CUNG chu ky: A phai thang\n");
  o_bo_nho[8] = 0;
  dut->a_req_i = 1; dut->a_we_i = 1; dut->a_addr_i = 0x20; dut->a_wdata_i = 0xAAAAAAAAu;
  dut->b_req_i = 1; dut->b_we_i = 1; dut->b_addr_i = 0x20; dut->b_wdata_i = 0xBBBBBBBBu;
  // LAY MAU `gnt` PHAI SAU khi `s_gnt_i` da duoc dat. Ban dau toi doc `a_gnt_o`
  // TRUOC `tick()`, luc `s_gnt_i` con la gia tri cu -- ca hai deu 0, nen phep
  // kiem `!(ga && gb)` DAT vi ca hai bang 0 chu khong vi bo trong tai dung. Mot
  // phep kiem RONG NGHIA: no khong the that bai. Nen phai lay mau trong pha
  // thap cua tick, va chot them rang DUNG MOT trong hai phai len.
  dut->clk_i = 0; dut->s_gnt_i = 1; dut->eval();
  const bool ga = dut->a_gnt_o, gb = dut->b_gnt_o;
  tick();
  if (o_bo_nho[8] != 0xAAAAAAAAu) bao("A khong thang khi hai master req cung luc");
  else std::printf("    ghi cua A thang (0x%08X)\n", o_bo_nho[8]);
  if (ga && gb) bao("cap gnt cho CA HAI master trong mot chu ky");
  else if (!ga)  bao("A req + slave gnt ma a_gnt_o KHONG len -- phep kiem truoc do RONG NGHIA");
  else if (gb)   bao("b_gnt_o len khi A dang thang");
  else std::printf("    DUNG MOT gnt len va la cua A (a_gnt=%d b_gnt=%d)\n", ga, gb);
  ranh(); tick();

  // ============================================ PHA 3 · lop loi chinh
  // A phat mot phep DOC, roi NGAY chu ky sau A ha req va B len req. `rvalid`
  // cua phep doc cua A den o dung chu ky do. Neu dinh tuyen theo `chon_b` hien
  // tai thi du lieu cua A ve B.
  std::printf("=== PHA 3 · A doc, roi B len req NGAY chu ky sau (rvalid phai ve A)\n");
  o_bo_nho[4] = 0x11112222u; o_bo_nho[12] = 0x33334444u;
  ve.clear();
  dut->a_req_i = 1; dut->a_we_i = 0; dut->a_addr_i = 0x10;
  dut->b_req_i = 0;
  tick();                                  // A duoc chon, phep doc di
  dut->a_req_i = 0;
  dut->b_req_i = 1; dut->b_we_i = 0; dut->b_addr_i = 0x30;
  tick();                                  // rvalid cua A ve DUNG luc B dang req
  ranh();
  tick(); tick();
  if (ve.size() < 1) bao("khong co rvalid nao");
  else if (!ve[0].a || ve[0].b)
    bao("rvalid cua phep doc cua A ve SAI MASTER khi B dang req cung chu ky");
  else if (ve[0].data != 0x11112222u) bao("rdata cua A sai");
  else std::printf("    rvalid dau ve A voi 0x%08X (dung), du B dang req\n", ve[0].data);
  if (ve.size() >= 2) {
    if (ve[1].a || !ve[1].b) bao("rvalid thu hai phai ve B");
    else std::printf("    rvalid sau ve B (dung)\n");
  }

  // ============================================ PHA 4 · khong CHET DOI
  // Phat bieu trong RTL: master ngoai chi bi TRI HOAN, khong chet doi, vi CPU
  // khong giu req mai. Kiem bang mot chuoi CPU HUU HAN rat dai.
  std::printf("=== PHA 4 · A req lien tuc 200 chu ky roi nha: B phai duoc phuc vu\n");
  int b_duoc = 0;
  dut->b_req_i = 1; dut->b_we_i = 1; dut->b_addr_i = 0x40; dut->b_wdata_i = 0xCAFEu;
  for (int i = 0; i < 200; ++i) {
    dut->a_req_i = 1; dut->a_we_i = 1; dut->a_addr_i = 0x50; dut->a_wdata_i = i;
    tick();
    if (dut->b_gnt_o) ++b_duoc;
  }
  if (b_duoc != 0) bao("B duoc cap gnt trong khi A dang req lien tuc (uu tien sai)");
  else std::printf("    200 chu ky A req: B duoc 0 lan (uu tien dung)\n");
  dut->a_req_i = 0;
  int cho = 0;
  for (int i = 0; i < 8; ++i) { tick(); if (dut->b_gnt_o) { break; } ++cho; }
  // gnt la to hop nen kiem qua bo nho: B phai ghi duoc sau khi A nha
  for (int i = 0; i < 4; ++i) tick();
  ranh(); tick();
  if (o_bo_nho[16] != 0xCAFEu)
    bao("B khong ghi duoc sau khi A nha req -- day la CHET DOI, khong phai tri hoan");
  else std::printf("    A nha req -> B ghi duoc sau %d chu ky (tri hoan, khong chet doi)\n", cho);

  // Cho cac phan hoi con bay ve.
  ranh(); for (int i = 0; i < 4; ++i) tick();
  std::printf("=== BAO TOAN · moi yeu cau duoc cap phai tra DUNG mot rvalid cho "
              "DUNG master\n");
  std::printf("    A: %d yeu cau -> %d phan hoi   B: %d yeu cau -> %d phan hoi\n",
              a_yeu_cau, a_phan_hoi, b_yeu_cau, b_phan_hoi);
  if (a_yeu_cau == 0 || b_yeu_cau == 0) bao("mot master khong co yeu cau nao -- phep kiem RONG");
  if (a_phan_hoi != a_yeu_cau)
    bao("A: so phan hoi khac so yeu cau -- rvalid bi dinh tuyen sai master");
  if (b_phan_hoi != b_yeu_cau)
    bao("B: so phan hoi khac so yeu cau -- rvalid bi dinh tuyen sai master");
  if (a_phan_hoi == a_yeu_cau && b_phan_hoi == b_yeu_cau)
    std::printf("    khop ca hai chieu\n");

  dut->final();
  ECG_COV_WRITE();
  delete dut;

  // Chot chong "PASS tren 0 giao dich".
  const int TOI_THIEU = 200;
  if (n_giao_dich < TOI_THIEU) {
    std::printf("tb_ecg_mmio_mux: FAIL (chot: %d giao dich, cho >= %d)\n",
                n_giao_dich, TOI_THIEU);
    return 2;
  }
  if (errors == 0) {
    std::printf("\ntb_ecg_mmio_mux: PASS (%d giao dich: uu tien A, rvalid dinh "
                "tuyen theo ai DA duoc chon chu khong ai DANG req, va tri hoan "
                "chu khong chet doi)\n", n_giao_dich);
    return 0;
  }
  std::printf("\ntb_ecg_mmio_mux: FAIL (%d loi tren %d giao dich)\n",
              errors, n_giao_dich);
  return 1;
}

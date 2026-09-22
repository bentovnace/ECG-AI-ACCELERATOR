// W1-D / F07+F08: mot mat na ghi KHONG DAY co duoc phep cham thanh ghi khong?
//
// Cau hoi phai hoi tren TRANG THAI THANH GHI THAT, khong tren `be_o`.
// `tb_ecg_apb` chay `ecg_apb_slave` dung mot minh va chi kiem `be_o` -- no noi
// duoc "cau DINH ghi bao nhieu byte" nhung khong noi duoc "ngoai vi da LAM gi".
// Tep nay noi CAU THAT voi NGOAI VI THAT va do `cp_start_o`, tuc chinh he qua
// cua bit START trong CTRL.
//
// HAI LOI DUOC DO O DAY:
// MUC APB-02 (danh gia 2026-09-05): thiet ke KHONG ho tro APB3, va cho lam ay
// duoc GHIM: mot master APB3 de `pstrb` o 0, va tep nay chung minh mat na 0 bi
// TU CHOI chu khong duoc hieu la "ca tu". Ai them mot duong APB3 ma khong buoc
// byte-enable day thi phep thu nay DO. (`make sim-apb-mmio`)
//   F07  `be_o = (pstrb_i == 4'b0000) ? 4'b1111 : pstrb_i` -- mot mat na RONG
//        (khong ghi byte nao) bi bien thanh ghi DU BON BYTE.
//   F08  `ecg_mmio.sv:537` dat `err_q[6]` khi `be != 4'hF` nhung VAN thuc hien
//        phep ghi, con `pslverr_o` bi buoc 0 nen bus bao THANH CONG.
#include <cstdio>
#include <cstdint>
#include "Vtb_apb_mmio_wrap.h"
#include "verilated.h"
#include "ecg_cov.h"

namespace {
Vtb_apb_mmio_wrap *dut = nullptr;
int loi = 0;
int n_phep = 0;

void tick() {
  dut->clk_i = 0; dut->eval();
  dut->clk_i = 1; dut->eval();
}

void reset() {
  dut->cpu_req_i = 0; dut->cpu_we_i = 0; dut->cpu_be_i = 0;
  dut->cpu_addr_i = 0; dut->cpu_wdata_i = 0;
  dut->rst_ni = 0; dut->psel_i = 0; dut->penable_i = 0; dut->pwrite_i = 0;
  dut->paddr_i = 0; dut->pwdata_i = 0; dut->pstrb_i = 0;
  for (int i = 0; i < 4; ++i) tick();
  dut->rst_ni = 1; tick();
}

// Mot phep truyen APB. Tra ve: co thay `cp_start_o` len trong luc truyen khong.
struct Kq { int start, single, dma, slverr, be; };

Kq apb_ghi(uint32_t addr, uint32_t data, uint32_t strb) {
  Kq k{0, 0, 0, 0, 0};
  dut->psel_i = 1; dut->penable_i = 0; dut->pwrite_i = 1;
  dut->paddr_i = addr; dut->pwdata_i = data; dut->pstrb_i = strb;
  dut->eval();
  tick();
  dut->penable_i = 1;
  for (int i = 0; i < 64; ++i) {
    dut->eval();
    if (dut->cp_start_o)  k.start  = 1;
    if (dut->cp_single_o) k.single = 1;
    if (dut->dma_start_o) k.dma    = 1;
    if (dut->pslverr_o)   k.slverr = 1;
    k.be = dut->be_o;
    // CUA SO QUAN SAT: `pslverr_o` len CUNG NHIP voi `pready_o` (ca hai to hop
    // tu `tt_q == S_XONG`). Ban truoc doc `pready_o` SAU `tick()` roi thoat
    // vong, nen chu ky S_XONG khong bao gio duoc lay mau va `pslverr` doc noi
    // cung 0 -- mot phep do CHAY DUNG nhung cau hoi RONG. Doc ca hai trong
    // CUNG mot `eval`, roi moi tick va thoat.
    const bool xong = dut->pready_o;
    tick();
    if (xong) break;
  }
  dut->psel_i = 0; dut->penable_i = 0;
  // Xung `cp_start_o` co the den mot chu ky sau khi pready: nghe them vai nhip.
  for (int i = 0; i < 4; ++i) {
    dut->eval();
    if (dut->cp_start_o)  k.start  = 1;
    if (dut->cp_single_o) k.single = 1;
    if (dut->dma_start_o) k.dma    = 1;
    tick();
  }
  ++n_phep;
  return k;
}

// Mot phep DOC APB. Tra ve `prdata_o`. Doc la duong DUY NHAT thay TRANG THAI
// THAT cua thanh ghi: `be_o` chi noi cai gi di vao, khong noi cai gi o lai.
uint32_t apb_doc(uint32_t addr) {
  uint32_t v = 0;
  dut->psel_i = 1; dut->penable_i = 0; dut->pwrite_i = 0;
  dut->paddr_i = addr; dut->pstrb_i = 0xF;
  dut->eval();
  tick();
  dut->penable_i = 1;
  for (int i = 0; i < 64; ++i) {
    dut->eval();
    const bool xong = dut->pready_o;
    if (xong) v = dut->prdata_o;
    tick();
    if (xong) break;
  }
  dut->psel_i = 0; dut->penable_i = 0;
  dut->eval(); tick();
  ++n_phep;
  return v;
}

// Mot phep ghi OBI di THANG vao mux (cong A = cong du lieu CPU). Duong nay
// KHONG qua cau APB: khong co `pstrb`, khong co `pslverr`, va khong co lop chan
// nao ngoai `be_day` trong `ecg_mmio`.
void cpu_ghi(uint32_t addr, uint32_t data, uint32_t be) {
  dut->cpu_req_i = 1; dut->cpu_we_i = 1; dut->cpu_be_i = be;
  dut->cpu_addr_i = addr; dut->cpu_wdata_i = data;
  for (int i = 0; i < 64; ++i) {
    dut->eval();
    const bool nhan = dut->cpu_gnt_o;
    tick();
    if (nhan) break;
  }
  dut->cpu_req_i = 0; dut->cpu_we_i = 0;
  dut->eval(); tick(); dut->eval(); tick();
}

uint32_t cpu_doc(uint32_t addr) {
  uint32_t v = 0;
  dut->cpu_req_i = 1; dut->cpu_we_i = 0; dut->cpu_be_i = 0xF;
  dut->cpu_addr_i = addr;
  for (int i = 0; i < 64; ++i) {
    dut->eval();
    const bool nhan = dut->cpu_gnt_o;
    tick();
    if (nhan) break;
  }
  dut->cpu_req_i = 0;
  for (int i = 0; i < 64; ++i) {
    dut->eval();
    const bool co = dut->cpu_rvalid_o;
    if (co) v = dut->cpu_rdata_o;
    tick();
    if (co) break;
  }
  return v;
}
}  // namespace

double sc_time_stamp() { return 0; }

int main(int argc, char **argv) {
  Verilated::commandArgs(argc, argv);
  dut = new Vtb_apb_mmio_wrap;

  // CTRL = 0x000. bit0 start · bit1 single · bit2 dma_start.
  const uint32_t CTRL = 0x000u;
  const uint32_t DAT  = 0x7u;   // bat ca ba bit co he qua QUAN SAT DUOC

  std::printf("=== W1-D: mat na ghi va TRANG THAI THANH GHI THAT ===\n");
  std::printf("    ghi CTRL = 0x%x (start|single|dma_start) voi ca 16 mat na\n\n", DAT);
  std::printf("    %-6s %-5s %-7s %-7s %-6s %-8s %s\n",
              "pstrb", "be_o", "start", "single", "dma", "pslverr", "phan dinh");

  int n_cham = 0, n_slverr = 0;
  for (uint32_t m = 0; m < 16; ++m) {
    reset();
    Kq k = apb_ghi(CTRL, DAT, m);
    const int day_du = (m == 0xF);
    const int cham   = k.start || k.single || k.dma;
    if (!day_du && cham) ++n_cham;
    if (!day_du && k.slverr) ++n_slverr;
    const char *pd = day_du ? (cham ? "ok (day du -> phai cham)" : "!! day du ma KHONG cham")
                            : (cham ? "!! KHONG day du ma VAN cham" : "ok (bi tu choi)");
    std::printf("    0x%-4x 0x%-3x %-7d %-7d %-6d %-8d %s\n",
                m, k.be, k.start, k.single, k.dma, k.slverr, pd);
    // CHOT 1: mat na KHONG day du khong duoc gay he qua nao
    if (!day_du && cham) {
      std::printf("      CHOT F07/F08: pstrb=0x%x (khong day du) van lam "
                  "start=%d single=%d dma=%d -- mot phep ghi mot phan da cham "
                  "thanh ghi CTRL\n", m, k.start, k.single, k.dma);
      ++loi;
    }
    // CHOT 2: mat na KHONG day du phai bao loi tren bus
    if (!day_du && !k.slverr) {
      std::printf("      CHOT F08: pstrb=0x%x (khong day du) ma pslverr=0 -- "
                  "bus bao THANH CONG cho mot phep ghi bi tu choi\n", m);
      ++loi;
    }
    // CHOT 3: mat na DAY DU phai di duoc, khong thi "sua" thanh chan het
    if (day_du && !cham) {
      std::printf("      CHOT: pstrb=0xF ma khong cham thanh ghi nao -- ban sua "
                  "da chan CA phep ghi hop le\n");
      ++loi;
    }
    if (day_du && k.slverr) {
      std::printf("      CHOT: pstrb=0xF ma pslverr=1 -- bao loi cho mot phep "
                  "ghi hop le\n");
      ++loi;
    }
  }

  // ------------------------------------------------------------------------
  // PHA 2 -- TRANG THAI THANH GHI THAT.
  //
  // Pha 1 doc `cp_start_o`/`cp_single_o`/`dma_start_o`: do la XUNG, va mot xung
  // vang mat co the vi phep ghi bi tu choi HAY vi cua so nghe sai. Mot xung
  // khong phan biet duoc hai kha nang do, nen no khong du.
  //
  // Pha nay GHI mot gia tri phan biet roi DOC LAI: neu o thanh ghi giu gia tri
  // NEN cu thi phep ghi khong co he qua, va do la mot dau hieu chi khop MOT kha
  // nang. Bon o phu bon duong giai ma khac nhau (`sel_reg`, `sel_desc`,
  // `sel_scale`, `sel_bias`) vi hop dong phai dung cho CA BON, khong chi cho
  // duong thanh ghi.
  struct O { const char *ten; uint32_t addr; uint32_t nen; uint32_t moi; };
  const O o_kiem[4] = {
      {"N_LAYERS (sel_reg)",  0x00Cu, 0x0000000Bu, 0x00000007u},
      {"DESC[0].w0 (sel_desc)", 0x400u, 0xA5A5A5A5u, 0x5A5A5A5Au},
      {"SCALE[0] (sel_scale)",  0x800u, 0x0000BEEFu, 0x0000CAFEu},
      {"BIAS[0] (sel_bias)",    0x1000u, 0x00000055u, 0x000000AAu},
  };

  std::printf("\n    --- PHA 2: ghi voi mat na le roi DOC LAI o that ---\n");
  std::printf("    %-24s %-6s %-10s %-10s %s\n",
              "o", "pstrb", "nen", "doc lai", "phan dinh");
  int n_doi = 0;      // so lan mot mat na le VAN doi duoc o
  int n_khong_vao = 0; // so lan mat na 0xF KHONG doi duoc o
  // CHONG-RONG: mot pha khong in dong nao khop CA HAI kha nang -- qua het, HAY
  // chua chay. Dem so cap (o, mat na) THAT SU duoc thu voi mot nen da xac nhan.
  int n_thu = 0;
  for (const O &o : o_kiem) {
    for (uint32_t m = 0; m < 16; ++m) {
      // Dat NEN bang mot phep ghi day du, roi doc lai de chac nen la that.
      apb_ghi(o.addr, o.nen, 0xF);
      const uint32_t nen_that = apb_doc(o.addr);
      if (nen_that != o.nen) {
        std::printf("    %-24s --     0x%08x KHONG dat duoc nen -- bo qua o nay\n",
                    o.ten, o.nen);
        break;
      }
      apb_ghi(o.addr, o.moi, m);
      const uint32_t sau = apb_doc(o.addr);
      ++n_thu;
      const bool day_du = (m == 0xF);
      const bool doi    = (sau != o.nen);
      if (!day_du && doi) {
        std::printf("    %-24s 0x%-4x 0x%08x 0x%08x !! mat na le VAN doi o\n",
                    o.ten, m, o.nen, sau);
        ++n_doi; ++loi;
      } else if (day_du && !doi) {
        std::printf("    %-24s 0x%-4x 0x%08x 0x%08x !! mat na DAY DU khong vao\n",
                    o.ten, m, o.nen, sau);
        ++n_khong_vao; ++loi;
      }
    }
  }
  std::printf("    da thu %d/64 cap (o, mat na) voi nen da xac nhan\n", n_thu);
  std::printf("    %d/60 mat na le doi duoc o · %d/4 o tu choi ca mat na day du\n",
              n_doi, n_khong_vao);
  if (n_thu != 64) {
    std::printf("  CHOT: pha 2 chi thu %d/64 cap -- mot o khong dat duoc nen nen "
                "phep do KHONG hoi gi ve o do\n", n_thu);
    ++loi;
  }

  // ------------------------------------------------------------------------
  // PHA 3 -- DUONG CPU DI THANG (khong qua cau APB).
  //
  // `ecg_mmio_mux` co HAI master: cong A la cong du lieu CPU, cong B la cau
  // APB. Mot lenh `sb`/`sh` cua firmware toi MMIO di qua cong A, nen khong cau
  // nao thay no. Tren duong ay `be_day` trong `ecg_mmio` la lop DUY NHAT.
  //
  // Pha 1 va 2 mot minh KHONG hoi duoc cau nay: go rieng lop MMIO ma hai pha do
  // van XANH, vi lop APB do thay. Mot phep thu chi xanh voi moi hinh dang cua
  // lop MMIO thi khong bat duoc hoi quy o lop MMIO.
  std::printf("\n    --- PHA 3: ghi mot phan qua CONG CPU (khong co cau APB) ---\n");
  int n_cpu_thu = 0, n_cpu_doi = 0, n_cpu_khong_vao = 0;
  for (const O &o : o_kiem) {
    for (uint32_t m = 0; m < 16; ++m) {
      cpu_ghi(o.addr, o.nen, 0xF);
      if (cpu_doc(o.addr) != o.nen) {
        std::printf("    %-24s --     KHONG dat duoc nen qua cong CPU\n", o.ten);
        ++loi; break;
      }
      cpu_ghi(o.addr, o.moi, m);
      const uint32_t sau = cpu_doc(o.addr);
      ++n_cpu_thu;
      const bool day_du = (m == 0xF);
      const bool doi    = (sau != o.nen);
      if (!day_du && doi) {
        std::printf("    %-24s 0x%-4x 0x%08x 0x%08x !! be le VAN doi o qua cong CPU\n",
                    o.ten, m, o.nen, sau);
        ++n_cpu_doi; ++loi;
      } else if (day_du && !doi) {
        std::printf("    %-24s 0x%-4x 0x%08x 0x%08x !! be DAY DU khong vao qua cong CPU\n",
                    o.ten, m, o.nen, sau);
        ++n_cpu_khong_vao; ++loi;
      }
    }
  }
  std::printf("    da thu %d/64 cap qua cong CPU · %d/60 be le doi duoc o · "
              "%d/4 o tu choi ca be day du\n", n_cpu_thu, n_cpu_doi, n_cpu_khong_vao);
  if (n_cpu_thu != 64) {
    std::printf("  CHOT: pha 3 chi thu %d/64 cap -- duong CPU khong duoc hoi day du\n",
                n_cpu_thu);
    ++loi;
  }

  // CHOT 4 chong-rong: phai chay du 16 phep, khong thi mot bang rong doc nhu
  // "moi mat na deu bi tu choi".
  if (n_phep < 16) {
    std::printf("  CHOT: chay %d phep truyen, can it nhat 16 -- phep do khong "
                "day du\n", n_phep);
    ++loi;
  }
  std::printf("\n    %d/15 mat na khong-day-du van cham thanh ghi · "
              "%d/15 co pslverr\n", n_cham, n_slverr);

  ECG_COV_WRITE();
  dut->final();
  delete dut;
  if (loi) { std::printf("tb_ecg_apb_mmio: FAIL (%d loi)\n", loi); return 1; }
  std::printf("tb_ecg_apb_mmio: PASS (chi pstrb=0xF cham thanh ghi; 15 mat na "
              "con lai bi tu choi va bao pslverr)\n");
  return 0;
}

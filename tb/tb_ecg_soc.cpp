// Testbench CAP HE: mot chuong trinh RISC-V THAT chay tren loi CV32E40X THAT,
// dieu khien bo dong xu ly ECG qua CV-X-IF va qua thanh ghi anh xa bo nho, nap
// BON mo hinh lan luot theo HAI luot quet.
//
// CAI GI DUOC CHUNG MINH neu phep thu nay dat:
//
//   1. Loi NAP LENH duoc tu IMEM (readmemh + BOOT_ADDR 0x80 khop nhau).
//   2. Loi DOC/GHI duoc bus du lieu, va bo giai ma dia chi chia dung DMEM/MMIO.
//   3. Mot LENH CUSTOM di het duong: loi phat -> cau CV-X-IF -> shim -> ket qua
//      -> tro lai mot thanh ghi cua loi.
//   4. Duong DMA trong so chay cho ca BON ho, moi ho voi so byte khac nhau.
//   5. Khong mot co CHOT nao trong ecg_mmio bat (ERRSTAT = 0).
//   6. HAI MUOI diem lop (4 ho x 5) khop BIT-EXACT tham chieu Python, VA khop lai
//      khi chay theo luot NGUOC -- tuc doi mo hinh khong de lai nhiem ban.
//
// VI SAO HAI LUOT. Bo dem hoat do KHONG duoc xoa giua hai ho, va bat bien ADR-0013
// (doc byte chua ghi) khong bat duoc rac cua ho truoc vi ho truoc DA GHI byte do.
// Mot luot mot chieu thi "ket qua dung" phu hop voi CA HAI kha nang: khong nhiem
// ban, hay co nhiem ban ma tien nhiem tinh co de lai dung thu can. Chay ho k sau
// HAI tien nhiem khac nhau moi phan dinh duoc (bat bien 21).
//
// BA CACH THAT BAI DUOC PHAN BIET, khong gop thanh mot "khong chay":
//
//   SIG(0) = 0x1EC6A11C  main() tra ve binh thuong -> doc SIG(4)/SIG(5).
//   SIG(0) = 0x7BADC0DE  TRAP. crt0 ghi mcause vao SIG(2) va mepc vao SIG(3).
//   SIG(0) = 0          het gio ma khong ghi gi -> loi khong chay, hay treo.
//
// HAI DUONG DO CHU KY, va chung phai khop nhau. Firmware doc CSR mcycle quanh moi
// khoang; testbench DEM CHU KY giua hai lan token SIG(96) doi. Hai duong di qua hai
// co che hoan toan khac (bo dem trong loi qua csrr, va dong ho cua mo phong qua cong
// go loi), nen neu chung khop thi con so khong den tu mot loi doc dan.

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include "Vecg_soc.h"
#include "verilated.h"
#include "ecg_cov.h"

static Vecg_soc *dut;
static int apb_lech = 0;   // pha APB: dem rieng, kiem o cuoi main

// ====================== "PC" gia lap: bo gui/nhan serial KHONG CHAN ==========
// Buoc MOT NHIP moi `tick()` cua vong chay firmware. Phai khong chan vi firmware
// dang chay trong cung vong do -- mot ham gui chan se dung dong ho cua CPU.
namespace pc {
static const int CKB = 8;              // phai khop -GUART_CK_MOI_BIT
static std::vector<uint8_t> se_gui;    // hang doi gui
static size_t gui_i = 0;
static int g_tt = -1, g_dem = 0;       // -1 = ranh; 0 = start; 1..8 = data; 9 = stop
static uint8_t g_byte = 0;
static std::vector<uint8_t> da_nhan;
static int n_tt = -1, n_dem = 0, n_bit = 0;
static uint8_t n_sr = 0;

// Tra ve muc phai dat len `uart_rx_i` o chu ky nay.
static bool cho_phep = false;   // chi gui khi firmware bao da vao pha UART

static int muc_gui() {
  if (g_tt < 0) {
    if (!cho_phep || gui_i >= se_gui.size()) return 1;      // ranh
    g_byte = se_gui[gui_i++]; g_tt = 0; g_dem = 0;
  }
  int muc;
  if (g_tt == 0)      muc = 0;                 // start
  else if (g_tt <= 8) muc = (g_byte >> (g_tt - 1)) & 1;
  else                muc = 1;                 // stop
  if (++g_dem >= CKB) { g_dem = 0; if (++g_tt > 9) g_tt = -1; }
  return muc;
}

// Khi nhan du `nguong_vong` byte thi GUI TRA LAI toan bo -- "PC" khong biet bo
// cuc anh, nen firmware gui 259 mau cua no ra truoc va "PC" chi vong chung ve.
// Nho the phep kiem thanh mot VONG TRON du lieu THAT.
// `nguong_vong` = tong so byte da nhan tai luc kich hoat; `so_vong` = so byte
// CUOI duoc gui tra lai. Hai con so rieng vi thu tu that la: firmware vong 8
// byte truoc (dao bit), ROI gui 259 mau ra -- nen kich hoat o 8+259 va chi vong
// lai 259 byte cuoi.
static size_t nguong_vong = 0, so_vong = 0;
static bool da_vong = false;

// Doc `uart_tx_o`. LAY MAU GIUA BIT, cung ly do voi bo nhan trong RTL.
static void nghe(int tx) {
  if (n_tt < 0) {
    if (!tx) { n_tt = 0; n_dem = 0; }          // canh xuong = start
    return;
  }
  ++n_dem;
  if (n_tt == 0) {
    if (n_dem >= CKB / 2) {
      if (tx) { n_tt = -1; return; }           // nhieu, khong phai start
      n_tt = 1; n_dem = 0; n_bit = 0; n_sr = 0;
    }
  } else if (n_tt == 1) {
    if (n_dem >= CKB) {
      n_dem = 0;
      n_sr = (uint8_t)((n_sr >> 1) | (tx ? 0x80 : 0));
      if (++n_bit == 8) n_tt = 2;
    }
  } else {
    if (n_dem >= CKB) {
      da_nhan.push_back(n_sr); n_tt = -1;
      // Du nguong -> nap lai hang doi gui bang chinh cac byte vua nhan.
      if (nguong_vong && !da_vong && da_nhan.size() >= nguong_vong) {
        da_vong = true;
        se_gui.insert(se_gui.end(), da_nhan.end() - (long)so_vong, da_nhan.end());
      }
    }
  }
}
}  // namespace pc
static vluint64_t main_time = 0;
double sc_time_stamp() { return main_time; }

static void tick() {
    dut->clk_i = 0; dut->eval(); ++main_time;
    dut->clk_i = 1; dut->eval(); ++main_time;
}

// ---- doc vung chu ky qua cong go loi DONG BO ----------------------------
// Cong `dbg_data_o` thanh DONG BO tu 2026-09-05 (V1-C rao 1): mot cong doc TO
// HOP tren `dmem` ep ca mang 48 KB xuong LUTRAM va lam Vivado TU CHOI tong hop
// (393.216 bit). Nen mot chu ky chi doc duoc MOT dia chi, va thu tu bat buoc la
//     dat dia chi -> canh dong ho -> chot gia tri.
//
// Toi da thu cach de hon truoc: goi `tick()` ngay trong `sig()`. No HONG -- hai
// cho goi `sig()` nam TRONG vong chay chinh, nen moi lan doc them mot canh dong
// ho vao duong UART va lam ca lan chay lech ("token pha khong den 36", byte
// vong vong sai). Mot phep doc khong duoc phep DOI thu no dang do.
//
// Va KHONG lam cong doc khac nhau giua mo phong va tong hop: do dung la lop
// phan ky ma ca dot nay dang chong -- mot duong chi song trong mo phong thi
// khong bao gio duoc kiem tren duong that.
static uint32_t sig_cache[256];
static int      sig_dc_dat = -1;      // dia chi da dat cho canh sap toi

static void sig_dat(int i) {          // goi TRUOC tick()
    sig_dc_dat = i & 0xFF;
    dut->dbg_addr_i = static_cast<uint8_t>(sig_dc_dat);
}
static void sig_chot() {              // goi SAU tick()
    if (sig_dc_dat >= 0) sig_cache[sig_dc_dat] = dut->dbg_data_o;
}
static uint32_t sig(int i) { return sig_cache[i & 0xFF]; }

// Quet ca 256 o mot luot: dung SAU khi lan chay ket thuc, khi them canh dong ho
// khong con anh huong gi.
static void sig_quet_het() {
    for (int i = 0; i < 256; ++i) { sig_dat(i); tick(); sig_chot(); }
}

static const uint32_t SIG_MAGIC = 0x1EC6A11Cu;
static const uint32_t SIG_TRAP  = 0x7BADC0DEu;
static const int N_HO = 4;
static const int N_MOC = 4 * 2 * N_HO;      // 4 moc / lan chay, 8 lan chay
// TOKEN CUOI KY VONG. Khac N_MOC vi pha STREAM goi `chay_ho` MOT lan nua.
//
// VA CON SO NAY LA DO DUOC, khong suy ra. Toi suy truoc: dem bon lenh `moc()`
// trong main.c va gan ba cho `chay_ho` + mot cho `luu`, ra N_MOC + 3 = 35. Do
// duoc la 36. Nen CA BON `moc()` nam trong `chay_ho` va `luu` khong goi cai nao
// -- mot lan `chay_ho` cong BON token, khong ba.
//   tam lan chay thuong: 8 x 4 = 32 = N_MOC
//   pha STREAM:          1 x 4 =  4
// Sua ky vong thay vi PHUC HOI bo dem: token la mot bo dem pha THAT, va mot lan
// chay them la mot pha that. Phuc hoi no se lam token noi doi ve so pha da chay.
static const int TOKEN_CUOI = N_MOC + 4;
static const int TOKEN = 127;               // muc cuoi cua dai dbg_addr_i [6:0]

struct Bit { uint32_t mask; const char *ten; };
static const Bit BITS[] = {
    {1u << 0,  "A1 thanh ghi MMIO di va ve nguyen ven"},
    {1u << 1,  "A2 bang scale/bias di va ve nguyen ven"},
    {1u << 2,  "A3 rd cua MOI lenh tinh di 1..n_lop o ca 8 lan chay"},
    {1u << 3,  "A4 duong MMIO: chuyen nguon dieu khien"},
    {1u << 4,  "A5 khong co co CHOT nao bat (ERRSTAT = 0 qua ca 8 lan chay)"},
    {1u << 5,  "B0 MUC LUC nap duoc (magic MDIR + 4 ho tai 0x11000)"},
    {1u << 6,  "B1 ca 4 anh mo hinh co magic MODL"},
    {1u << 7,  "B2 DMA trong so xong o ca 8 lan chay"},
    {1u << 8,  "B3 moi ho chay HET lop, va SO LOP nhu nhau o hai luot"},
    {1u << 9,  "B4 FIFO ket qua co DUNG 5 diem lop o ca 8 lan chay"},
    {1u << 10, "B5 doc lai ca ba bang: 0 muc lech o ca 8 lan chay"},
    {1u << 11, "B7 RAO CHAN tra dung so lop, do bang bo dem KHAC"},
};

// Doc tep ky vong do tools/gen_model_hex.py sinh. KHONG go nam con so vao day: hai
// ban sao cua cung mot su that la hai cho de lech, va lech thi phep thu bao DAT cho
// mot ket qua SAI (hay nguoc lai).
struct KyVong { int n_ho; char ten[N_HO][32]; int n[N_HO]; int v[N_HO][8]; };

static int doc_ky_vong(const char *path, KyVong *k) {
    std::memset(k, 0, sizeof *k);
    FILE *f = std::fopen(path, "r");
    if (!f) { std::printf("  KHONG mo duoc %s\n", path); return 1; }
    char line[512];
    int doc_so = 0, h = 0, noi = 0;
    while (std::fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        if (!doc_so) { noi = std::atoi(line); doc_so = 1; continue; }
        if (h >= N_HO) { ++h; continue; }
        char ten[32]; int lop = 0, nkq = 0;
        int pos = 0;
        if (std::sscanf(line, "%31s %d %d %n", ten, &lop, &nkq, &pos) < 3) continue;
        std::snprintf(k->ten[h], sizeof k->ten[h], "%s", ten);
        k->n[h] = nkq > 8 ? 8 : nkq;
        const char *p = line + pos;
        for (int i = 0; i < k->n[h]; ++i) {
            int v = 0, used = 0;
            if (std::sscanf(p, "%d%n", &v, &used) != 1) { k->n[h] = i; break; }
            k->v[h][i] = v; p += used;
        }
        ++h;
    }
    std::fclose(f);
    k->n_ho = h;
    if (h != noi) {
        std::printf("  CHOT: tep ky vong noi %d ho nhung doc duoc %d\n", noi, h);
        return 1;
    }
    if (h != N_HO) {
        std::printf("  CHOT: tep ky vong co %d ho, testbench cho %d\n", h, N_HO);
        return 1;
    }
    for (int i = 0; i < h; ++i)
        if (k->n[i] != 5) {
            std::printf("  CHOT: ho %s co %d ket qua, cho 5\n", k->ten[i], k->n[i]);
            return 1;
        }
    return 0;
}

// So mot luot quet. Tra so ho lech.
static int so_luot(const char *nhan, const KyVong &kv, int goc_diem, int goc_ps,
                   long tb_doi[], long tb_suy[], const int lich[])
{
    int lech = 0;
    std::printf("  --- luot %s (thu tu chay: %s %s %s %s) ---\n", nhan,
                kv.ten[lich[0]], kv.ten[lich[1]], kv.ten[lich[2]], kv.ten[lich[3]]);
    for (int k = 0; k < N_HO; ++k) {
        int khop = 1;
        std::printf("    %-13s diem:", kv.ten[k]);
        for (int i = 0; i < 5; ++i) {
            int got = (int)sig(goc_diem + 5 * k + i);
            std::printf(" %4d", got);
            if (got != kv.v[k][i]) khop = 0;
        }
        if (!khop) {
            std::printf("   KY VONG:");
            for (int i = 0; i < 5; ++i) std::printf(" %4d", kv.v[k][i]);
        }
        const uint32_t fw_doi = sig(goc_ps + 24 + k);
        const uint32_t fw_suy = sig(goc_ps + 32 + k);
        std::printf("  | lop=%u rao=%u res=%u bang_sai=%u rd=%u err=0x%02x"
                    "  doi=%u ck suy=%u ck  %s\n",
                    sig(goc_ps + 0 + k), sig(goc_ps + 56 + k), sig(goc_ps + 8 + k),
                    sig(goc_ps + 16 + k), sig(goc_ps + 48 + k), sig(goc_ps + 40 + k),
                    fw_doi, fw_suy, khop ? "BIT-EXACT" : "<<< LECH");
        if (!khop) ++lech;
        // PHEP DO TOI UU DUONG NAP, ca hai duong trong CUNG mot luot chay. Chi in
        // o luot THUAN vi firmware chi ghi hai khe do o do (SIG(112/116)).
        if (goc_ps == 48) {
            // `t1` (PRE don) nay bang 0 CO Y: duong san pham chi dung PRE4. Phep
            // so hai duong chuyen ra khoi do o cuoi (SIG(160..162)) vi o trong
            // day no nam TRONG cua so tinh gio va lam `t_suy` phong 1.520 ck.
            const uint32_t t1 = sig(112 + k), t4 = sig(116 + k);
            if (t1 == 0 && t4 != 0) {
                std::printf("        NAP 259 mau qua GOI PRE4: %u ck (%.2f ck/mau) "
                            "-- duong PRE don khong con trong duong san pham\n",
                            t4, (double)t4 / 259.0);
            } else if (t1 == 0 || t4 == 0) {
                std::printf("        CHOT: mot trong hai con so nap bang 0 "
                            "(PRE=%u PRE4=%u) -- khe SIG khong duoc ghi\n", t1, t4);
                ++lech;
            } else {
                std::printf("        NAP 259 mau: PRE don %u ck · GOI PRE4 %u ck "
                            "· giam %d ck (%.1f%%) · %.2f -> %.2f ck/mau\n",
                            t1, t4, (int)t1 - (int)t4,
                            100.0 * ((double)t1 - (double)t4) / (double)t1,
                            (double)t1 / 259.0, (double)t4 / 259.0);
                if (t4 >= t1) {
                    std::printf("        CHOT: duong GOI khong nhanh hon duong don "
                                "-- phep toi uu khong co tac dung\n");
                    ++lech;
                }
            }
        }
        // Duong do THU HAI: dong ho cua mo phong, doc qua token SIG(96).
        const int lan = (goc_diem == 8) ? k : (N_HO - 1 - k) + N_HO;
        // `lan` la thu tu CHAY, con `k` la chi so HO -- o luot nguoc hai cai khac nhau.
        if (tb_doi[lan] >= 0) {
            long d = (long)fw_doi - tb_doi[lan];
            long s = (long)fw_suy - tb_suy[lan];
            std::printf("        duong thu hai (dong ho mo phong): doi=%ld ck suy=%ld ck"
                        "  lech csrr-mo_phong: %+ld / %+ld\n",
                        tb_doi[lan], tb_suy[lan], d, s);
            if (d < -64 || d > 64 || s < -64 || s > 64) {
                std::printf("        CHOT: hai duong do lech qua 64 chu ky -- mot trong"
                            " hai duong khong do dung khoang no tuong do\n");
                ++lech;
            }
        }
    }
    return lech;
}

int main(int argc, char **argv) {
    Verilated::commandArgs(argc, argv);
    dut = new Vecg_soc;

    const long CAP = 20000000;
    const bool trace = getenv("ECG_SOC_TRACE") != nullptr;

    KyVong kv;
    if (doc_ky_vong("80-firmware/build/model-expect.txt", &kv)) {
        std::printf("tb_ecg_soc: FAIL (khong doc duoc ky vong)\n");
        ECG_COV_WRITE();
  delete dut; return 1;
    }

    dut->rst_ni = 0;
    dut->fetch_enable_i = 0;
    dut->dbg_addr_i = 0;
    // DUONG UART PHAI RANH (muc 1) NGAY TU RESET. Verilator dat dau vao khong
    // gan = 0, tuc duong o muc THAP -- va muc thap la mot START BIT. Bo nhan vao
    // R_DATA ngay, va khung THAT dau tien den sau do bi lech: byte 0xA5 doc ra
    // 0xC0. Ba muoi hai chu ky ranh cua toi khong du de xa mot khung 10 bit (80
    // chu ky o CK_MOI_BIT=8). Tren phan cung that duong idle o muc cao nen ca
    // nay khong xay ra -- nhung mot harness khong duoc mo phong mot dieu kien
    // ma phan cung khong co.
    dut->uart_rx_i = 1;
    for (int i = 0; i < 20; ++i) tick();
    dut->rst_ni = 1;
    for (int i = 0; i < 5; ++i) tick();

    // ============================== PHA APB · truoc khi nha loi
    // VI SAO PHA NAY TON TAI. `ecg_apb_slave` da co phep kiem RIENG (tb_ecg_apb,
    // 78 phep truyen) va `ecg_mmio_mux` cung the (212 giao dich). Nhung ca hai
    // kiem KHOI, khong kiem viec KHOI DA DUOC NOI. Mot khoi da kiem ma chua noi
    // thi chua phai mot tinh nang -- va mot cong APB noi sai (dia chi lech, be
    // sai, hay chi don gian khong noi vao dau) van cho `sim-soc` PASS vi firmware
    // khong di qua duong do. Do dung la mot phep kiem khong the that bai.
    //
    // Chay TRUOC khi `fetch_enable_i` len: khong co tranh chap voi CPU, nen mot
    // phep do o day noi ve DUONG NOI chu khong ve bo trong tai (bo trong tai da
    // co phep kiem rieng cho tranh chap).
    // `ECG_SOC_NO_APB=1` bo pha nay: mot co de PHAN DINH "pha APB lam vo"
    // voi "duong noi lam vo", chay tren CUNG mot ban build.
    if (!std::getenv("ECG_SOC_NO_APB")) {
        auto apb = [&](bool ghi, uint32_t addr, uint32_t data) -> uint32_t {
            dut->apb_psel_i = 1; dut->apb_penable_i = 0;
            dut->apb_pwrite_i = ghi; dut->apb_paddr_i = addr;
            dut->apb_pwdata_i = data; dut->apb_pstrb_i = 0xF;
            tick();
            dut->apb_penable_i = 1;
            uint32_t r = 0;
            if (std::getenv("APB_TRACE"))
                std::printf("      [apb] ghi=%d addr=0x%03X wd=%u\n", (int)ghi, addr, data);
            // LAY MAU O PHA THAP roi moi danh canh, va thoat theo gia tri DA LAY
            // chu khong theo gia tri SAU canh. Ban dau toi doc `pready_o` lai sau
            // canh de quyet dinh thoat, nen vong thoat TRUOC khi pha thap cua chu
            // ky co `pready` duoc lay mau -- `r` khong bao gio duoc gan va ca ba
            // phep doc tra 0. Trace cho thay `prdata` DA dung (37 roi 259), tuc
            // duong noi chay va loi nam trong harness. Lan thu BA trong phien nay
            // cung mot mau: doc mot tin hieu to hop o sai pha.
            for (int i = 0; i < 32; ++i) {
                dut->clk_i = 0; dut->eval();
                const bool rdy = dut->apb_pready_o;
                if (rdy) r = dut->apb_prdata_o;
                if (std::getenv("APB_TRACE"))
                    std::printf("      [apb] ck%-2d pready=%d prdata=%u\n",
                                i, (int)rdy, dut->apb_prdata_o);
                dut->clk_i = 1; dut->eval(); ++main_time;
                if (rdy) break;
            }
            dut->apb_psel_i = 0; dut->apb_penable_i = 0; tick();
            return r;
        };
        // Ghi roi doc lai BA thanh ghi voi BA gia tri PHAN BIET: mot phep giai ma
        // dia chi sai khong the qua duoc ca ba (dung ly do voi T18 cua tb_ecg_mmio).
        struct { uint32_t off, val; const char *ten; } ba[3] = {
            {0x00C, 37, "NLAYERS"}, {0x010, 259, "IN_LEN"}, {0x008, 21, "LAYER"}};
        int apb_ok = 0;
        for (int i = 0; i < 3; ++i) {
            apb(true, ba[i].off, ba[i].val);
            const uint32_t rb = apb(false, ba[i].off, 0);
            if (rb == ba[i].val) ++apb_ok;
            else std::printf("  FAIL  APB %s: doc lai %u, ky vong %u\n",
                             ba[i].ten, rb, ba[i].val);
        }
        // CHOT: `pslverr` phai xuong, va `pready` khong duoc dinh.
        if (dut->apb_pslverr_o) { std::printf("  FAIL  APB pslverr len\n"); ++apb_lech; }
        if (dut->apb_pready_o)  { std::printf("  FAIL  APB pready DINH khi psel xuong\n"); ++apb_lech; }
        if (apb_ok == 3)
            std::printf("  ok    APB NOI THAT: ghi/doc lai 3 thanh ghi voi 3 gia tri "
                        "phan biet (37/259/21) qua cong APB4 cua ecg_soc\n");
        else
            ++apb_lech;
        // Tra ve 0 de khong anh huong firmware.
        for (int i = 0; i < 3; ++i) apb(true, ba[i].off, 0);
        const uint32_t er = apb(false, 0x02C, 0);
        const uint32_t st = apb(false, 0x004, 0);
        std::printf("      [apb] sau pha: ERRSTAT=0x%03X STATUS=0x%03X\n", er, st);
    }

    // ============================== PHA UART · truoc khi nha loi
    // VI SAO PHA NAY TON TAI, va no la cung cai bay voi pha APB: `ecg_uart` co
    // phep kiem RIENG (260 byte, bien lech dong ho do o ba do phan giai) nhung
    // phep kiem do kiem KHOI. Neu hai chan `uart_rx_i`/`uart_tx_o` khong noi vao
    // dau, hay noi vao dung khoi ma sai thanh ghi, thi `sim-soc` van PASS -- vi
    // firmware khong di qua duong do. Mot phep kiem khong the that bai.
    //
    // Chuoi day du duoc kiem o day: CHAN NGOAI -> UART -> thanh ghi -> bus.
    if (!std::getenv("ECG_SOC_NO_UART")) {
        const int CKB = 8;   // phai khop -GUART_CK_MOI_BIT cua Makefile
        auto gui_bit = [&](int muc, int n) {
            for (int i = 0; i < n; ++i) { dut->uart_rx_i = muc; tick(); }
        };
        auto gui_byte = [&](uint8_t b) {
            gui_bit(0, CKB);
            for (int i = 0; i < 8; ++i) gui_bit((b >> i) & 1, CKB);
            gui_bit(1, CKB * 3);
        };
        auto doc_apb = [&](uint32_t addr) -> uint32_t {
            dut->apb_psel_i = 1; dut->apb_penable_i = 0; dut->apb_pwrite_i = 0;
            dut->apb_paddr_i = addr; dut->apb_pstrb_i = 0xF; tick();
            dut->apb_penable_i = 1;
            uint32_t r = 0;
            for (int i = 0; i < 32; ++i) {
                dut->clk_i = 0; dut->eval();
                const bool rdy = dut->apb_pready_o;
                if (rdy) r = dut->apb_prdata_o;
                dut->clk_i = 1; dut->eval(); ++main_time;
                if (rdy) break;
            }
            dut->apb_psel_i = 0; dut->apb_penable_i = 0; tick();
            return r;
        };
        // Cho DU 12 bit: mot khung la 10 bit, va bo nhan chi ve RANH sau khi
        // dem het stop bit. Bon bit thi khong du, va do la loi harness dau tien
        // cua pha nay.
        dut->uart_rx_i = 1;
        for (int i = 0; i < CKB * 12; ++i) tick();

        // BA byte PHAN BIET, va mot trong ba la 0x00: mot duong khong noi vao
        // dau doc ra 0, nen neu chi thu cac gia tri khac 0 thi "0" se la mot
        // ket qua khong phan biet duoc voi "khong noi".
        const uint8_t ba[3] = {0xA5, 0x00, 0x3C};
        int uart_ok = 0;
        for (int i = 0; i < 3; ++i) {
            gui_byte(ba[i]);
            const uint32_t r = doc_apb(0x038);
            const bool co_byte = (r >> 8) & 1;
            const bool loi_khung = (r >> 9) & 1;
            if (!co_byte)
                std::printf("  FAIL  UART byte 0x%02X: bit8 (co_byte) khong len\n", ba[i]);
            else if (loi_khung)
                std::printf("  FAIL  UART byte 0x%02X: bao LOI KHUNG\n", ba[i]);
            else if ((r & 0xFF) != ba[i])
                std::printf("  FAIL  UART byte 0x%02X: doc ra 0x%02X\n", ba[i], r & 0xFF);
            else ++uart_ok;
            // doc lai: `co_byte` phai XUONG (doc la RUT)
            const uint32_t r2 = doc_apb(0x038);
            if ((r2 >> 8) & 1) {
                std::printf("  FAIL  UART: doc lan hai van bao co byte -- doc khong RUT\n");
                ++apb_lech;
            }
        }
        if (uart_ok == 3)
            std::printf("  ok    UART NOI THAT: ba byte phan biet (0xA5/0x00/0x3C) tu CHAN "
                        "NGOAI qua UART den thanh ghi, doc qua APB\n");
        else
            ++apb_lech;
    }

    dut->fetch_enable_i = 1;

    // Dong ho DOC LAP: ghi so chu ky tai moi lan token doi.
    long t_moc[N_MOC + 2];
    for (int i = 0; i < N_MOC + 2; ++i) t_moc[i] = -1;
    uint32_t tok_truoc = 0;

    long cyc = 0;
    uint32_t s0 = 0;
    // Nap hang doi cho "PC": tam byte phan biet, gom 0x00 va 0xFF -- hai gia tri
    // ma mot duong khong noi hay mot duong buoc cung se tra ra, nen chung la hai
    // ca phan biet duoc "chay" voi "khong noi".
    pc::se_gui = {0xA5, 0x00, 0xFF, 0x3C, 0x01, 0x80, 0x7F, 0x5A};
    // Sau tam byte vong vong, firmware gui 259 MAU ra; "PC" vong chung ve de
    // firmware chay suy luan tren ban quay ve.
    pc::nguong_vong = 8 + 259;
    pc::so_vong = 259;

    while (cyc < CAP) {
        // "PC" buoc MOT NHIP: dat rx truoc canh, nghe tx sau canh.
        // Chi gui khi firmware da bao vao pha UART (SIG(123)). Xem chu thich o
        // firmware: thanh ghi giu chua MOT byte, nen gui som lam tran no.
        if (!pc::cho_phep && sig(123) == 1) pc::cho_phep = true;
        // LUAN PHIEN ba dia chi ta can theo doi, mot dia chi moi chu ky: cong
        // doc dong bo chi tra MOT o moi canh. Vong nay chay hang nghin chu ky
        // nen doc moi o ba chu ky mot lan khong lam mat mot su kien nao --
        // `tok` va `s0` deu la "da doi chua", khong phai xung.
        {
            static const int dc[3] = {123, TOKEN, 0};
            sig_dat(dc[cyc % 3]);
        }
        dut->uart_rx_i = pc::muc_gui();
        tick();
        sig_chot();
        pc::nghe(dut->uart_tx_o);
        ++cyc;
        if (trace && cyc < 400)
            std::printf("    cyc %4ld  instr_addr = 0x%08x  pc_valid=%d pc=0x%08x\n",
                        cyc, dut->dbg_instr_addr_o, dut->dbg_pc_valid_o, dut->dbg_pc_o);
        const uint32_t tok = sig(TOKEN);
        if (tok != tok_truoc) {
            if (tok >= 1 && tok <= (uint32_t)N_MOC + 1) t_moc[tok] = cyc;
            tok_truoc = tok;
        }
        if ((cyc & 0xFF) == 0) {
            s0 = sig(0);
            if (s0 == SIG_MAGIC || s0 == SIG_TRAP) break;
        }
    }
    sig_quet_het();          // lan chay xong: quet ca 256 o mot luot
    s0 = sig(0);

    // Chu ky do bang dong ho mo phong: lan chay i dung moc 4i+1..4i+4.
    long tb_doi[2 * N_HO], tb_suy[2 * N_HO];
    for (int i = 0; i < 2 * N_HO; ++i) {
        const long a = t_moc[4 * i + 1], b = t_moc[4 * i + 2];
        const long c = t_moc[4 * i + 3], d = t_moc[4 * i + 4];
        tb_doi[i] = (a > 0 && b > 0) ? b - a : -1;
        tb_suy[i] = (c > 0 && d > 0) ? d - c : -1;
    }

    int loi = 0;
    std::printf("  chay %ld chu ky, mcycle = %llu\n", cyc,
                (unsigned long long)dut->mcycle_o);

    if (s0 == SIG_TRAP) {
        std::printf("  TRAP: mcause = 0x%08x, mepc = 0x%08x\n", sig(2), sig(3));
        std::printf("        mcause 2 = lenh khong hop le -> loi TU CHOI mot lenh.\n");
        std::printf("        Kiem ma hoa trong 80-firmware/include/ecg_isa.h, dieu kien\n");
        std::printf("        in_space trong ecg_cvxif.sv, va CSR 0xB00 (mcycle) neu\n");
        std::printf("        mepc tro vao mot csrr.\n");
        loi = 1;
    } else if (s0 != SIG_MAGIC) {
        std::printf("  HET GIO: SIG(0) = 0x%08x (khong phai MAGIC hay TRAP).\n", s0);
        std::printf("        token pha cuoi = %u / %d -- token cho biet firmware DUNG O DAU:\n",
                    sig(TOKEN), N_MOC);
        std::printf("        token 4i+1 = vao doi mo hinh ho thu i · 4i+2 = xong doi\n");
        std::printf("        4i+3 = vao suy luan · 4i+4 = xong suy luan (i tu 0)\n");
        std::printf("        core_sleep_o = %d, cp_busy_o = %d, cp_done_o = %d\n",
                    dut->core_sleep_o, dut->cp_busy_o, dut->cp_done_o);
        std::printf("        instr_addr cuoi = 0x%08x, pc = 0x%08x (valid=%d)\n",
                    dut->dbg_instr_addr_o, dut->dbg_pc_o, dut->dbg_pc_valid_o);
        std::printf("        Chay lai voi ECG_SOC_TRACE=1 de in PC 400 chu ky dau.\n");
        loi = 1;
    } else {
        const uint32_t ma    = sig(1);
        const uint32_t dat   = sig(4);
        const uint32_t truot = sig(5);
        const uint32_t err   = sig(6);
        std::printf("  so ho doc tu MUC LUC = %u · token pha = %u / %d "
                    "(%d thuong + 4 cua pha STREAM)\n",
                    sig(7), sig(TOKEN), TOKEN_CUOI, N_MOC);
        if (sig(TOKEN) != (uint32_t)TOKEN_CUOI) {
            std::printf("  FAIL  token pha khong den %d: mot lan chay khong hoan tat\n",
                        TOKEN_CUOI);
            loi = 1;
        }

        static const int LICH_T[N_HO] = {0, 1, 2, 3};
        static const int LICH_N[N_HO] = {3, 2, 1, 0};
        int lech = so_luot("THUAN", kv,  8, 48, tb_doi, tb_suy, LICH_T);
        lech    += so_luot("NGUOC", kv, 28, 52, tb_doi, tb_suy, LICH_N);
        if (lech == 0)
            std::printf("  ok    B6 ca %d diem lop khop BIT-EXACT o CA HAI luot quet\n",
                        2 * N_HO * 5);
        else {
            std::printf("  FAIL  B6 %d ho lech -- doi mo hinh KHONG sach\n", lech);
            loi = 1;
        }

        // Phep so hai luot: cung mot ho, hai tien nhiem khac nhau.
        std::printf("  --- doi chieu hai luot (cung ho, tien nhiem khac) ---\n");
        for (int k = 0; k < N_HO; ++k) {
            int giong = 1;
            for (int i = 0; i < 5; ++i)
                if (sig(8 + 5 * k + i) != sig(28 + 5 * k + i)) giong = 0;
            std::printf("    %-13s %s\n", kv.ten[k],
                        giong ? "thuan == nguoc" : "<<< THUAN KHAC NGUOC: nhiem ban theo tien nhiem");
            if (!giong) loi = 1;
        }

        std::printf("  main() tra ve %u · dat = 0x%03x · truot = 0x%03x · ERRSTAT = 0x%02x\n",
                    ma, dat, truot, err);
        for (const Bit &b : BITS) {
            if (dat & b.mask)        std::printf("  ok    %s\n", b.ten);
            else if (truot & b.mask) { std::printf("  FAIL  %s\n", b.ten); loi = 1; }
            else                     { std::printf("  ???   %s -- khong chay den\n", b.ten); loi = 1; }
        }
        if (dut->xif_kill_seen_o) {
            std::printf("  FAIL  co xif_kill_seen_o bat: mot lenh da nhan bi HUY, va"
                        " ecg_cvxif khong lui lai duoc (xem ecg_xif_bridge.sv muc 3)\n");
            loi = 1;
        } else {
            std::printf("  ok    xif_kill_seen_o = 0: khong co lenh nao bi huy\n");
        }
    }

    std::printf(loi ? "tb_ecg_soc: FAIL\n"
                    : "tb_ecg_soc: PASS (bon mo hinh, hai luot quet, chuoi RISC-V dau-den-cuoi)\n");
    // ==================== PHA UART-FIRMWARE · vong vong qua CPU
    // Dieu phep kiem nay chung minh, va la dieu hai pha truoc KHONG chung minh:
    // co MA TREN LOI di qua duong UART. Hai pha truoc kiem chan -> thanh ghi (qua
    // APB, tuc mot master NGOAI). Neu khong co dong ma nao tren CPU doc UART_RX
    // thi hai pha do van xanh -- va tieu chi "gia lap tin hieu tu PC" con thieu
    // dung nua phan MEM.
    //
    // Firmware gui lai byte DA DAO BIT (`^ 0xFF`). Dao bit la co y: mot vong vong
    // THUAN dat duoc bang cach noi cung tx voi rx trong phan cung, nen no khong
    // chung minh CPU da doc. Mot byte bi dao thi CHI CPU lam duoc.
    {
        const uint32_t n_vong = sig(120), n_khung = sig(121), er = sig(122);
        std::printf("=== PHA UART-FIRMWARE · vong vong qua CPU\n");
        std::printf("    firmware bao: %u byte vong vong, %u loi khung, ERRSTAT=0x%03X\n",
                    n_vong, n_khung, er);
        std::printf("    \"PC\" gui %zu byte, nhan lai %zu byte\n",
                    pc::se_gui.size(), pc::da_nhan.size());
        if (n_vong == 0) {
            std::printf("  FAIL  firmware vong vong 0 byte -- khong co ma nao doc UART_RX\n");
            ++apb_lech;
        } else if (pc::da_nhan.empty()) {
            std::printf("  FAIL  \"PC\" khong nhan lai byte nao\n");
            ++apb_lech;
        } else {
            int sai = 0;
            // CHI so TAM byte dau: tu byte 8 tro di la 259 MAU firmware gui ra,
            // khong phai byte vong vong. Ban dau toi so ca `da_nhan` va no bao
            // "byte 8: gui 0xFA cho 0x05 nhan 0xFA" -- phep so sai pham vi, khong
            // phai thiet ke sai.
            for (size_t i = 0; i < 8u && i < pc::da_nhan.size(); ++i) {
                const uint8_t cho = (uint8_t)(pc::se_gui[i] ^ 0xFF);
                if (pc::da_nhan[i] != cho) {
                    if (sai < 3)
                        std::printf("  FAIL  byte %zu: gui 0x%02X, cho 0x%02X, nhan 0x%02X\n",
                                    i, pc::se_gui[i], cho, pc::da_nhan[i]);
                    ++sai;
                }
            }
            if (n_khung) { std::printf("  FAIL  %u loi khung\n", n_khung); ++apb_lech; }
            if (er & ((1u << 10) | (1u << 11))) {
                std::printf("  FAIL  ERRSTAT bao TRAN UART (0x%03X)\n", er); ++apb_lech;
            }
            if (sai) { ++apb_lech; }
            else std::printf("  ok    UART-FIRMWARE: %zu byte di PC -> UART -> CPU -> UART -> PC, "
                             "moi byte DAO BIT dung (chi CPU lam duoc)\n", pc::da_nhan.size());
        }
    }

    // ==================== PHA STREAM · 259 mau THAT qua UART roi suy luan
    // Vong vong o tren chung minh duong DI duoc. Pha nay chung minh duong CHO
    // TIN HIEU. Va phep kiem la TUONG DUONG chu khong "co ra so khong": mau di
    // ra roi quay ve nguyen ven, nen NAM DIEM LOP phai GIONG HET luot dung anh.
    // Mot phep kiem "co ra so" thi mot duong UART dua ra rac van qua.
    {
        const uint32_t n_gui = sig(126), n_nhan = sig(124), khop = sig(125);
        std::printf("=== PHA STREAM · 259 mau THAT di PC <-> UART roi suy luan\n");
        std::printf("    firmware gui ra %u mau, nhan ve %u mau\n", n_gui, n_nhan);
        if (n_gui != 259 || n_nhan != 259) {
            std::printf("  FAIL  so mau khong du (gui %u, nhan %u, cho 259)\n", n_gui, n_nhan);
            ++apb_lech;
        } else {
            // Firmware doi chieu, khong tb -- vi `dbg_addr_i` rong 7 bit nen
            // khong con khe de day nam diem ra. Chuoi: tb kiem SIG(8..12) voi
            // model-expect.txt, firmware kiem ban qua UART voi SIG(8..12).
            if (khop != 5) {
                std::printf("  FAIL  chi %u/5 diem lop khop giua ban qua UART va ban dung anh\n",
                            khop);
                ++apb_lech;
            } else {
                std::printf("  ok    TUONG DUONG: 5/5 diem lop tu 259 mau qua UART GIONG HET "
                            "luot dung anh (%d %d %d %d %d)\n", (int)sig(8), (int)sig(9),
                            (int)sig(10), (int)sig(11), (int)sig(12));
            }
        }
    }

    // ==================== NAP MAU · hai duong, do NGOAI cua so tinh gio
    {
        const uint32_t p1 = sig(160), p4 = sig(161), nm = sig(162);
        if (p1 == 0 || p4 == 0 || nm == 0) {
            std::printf("  FAIL  phep do nap mau rong (PRE=%u PRE4=%u n=%u, ky vong n=259)\n",
                        p1, p4, nm);
            ++apb_lech;
        } else {
            std::printf("NAP %u mau: PRE don %u ck · GOI PRE4 %u ck · giam %d ck (%.1f%%) "
                        "· %.2f -> %.2f ck/mau\n", nm, p1, p4, (int)p1 - (int)p4,
                        100.0 * ((double)p1 - (double)p4) / (double)p1,
                        (double)p1 / nm, (double)p4 / nm);
            if (p4 >= p1) {
                std::printf("  FAIL  duong GOI khong nhanh hon duong don\n");
                ++apb_lech;
            }
        }
    }

    // ==================== TC2 · CHI PHI VONG COPY TRONG SO CUA CPU
    // "DMA" trong thiet ke nay KHONG phai mot bus master: `ws_valid_o` den tu
    // WFIFO trong `ecg_mmio`, va CPU nap FIFO do bang cac phep ghi WFIFO_L/H.
    // Nen bom trong so la mot VONG COPY, va so chu ky cua no chinh la co hoi ma
    // mot AXI master co burst se thu duoc. Con so gop `t_doi` khong noi duoc dieu do.
    {
        std::printf("=== TC2 · vong COPY trong so cua CPU (khong co bus master)\n");
        long tong_bang = 0, tong_wgt = 0, tong_word = 0;
        int du = 1;
        for (int k = 0; k < N_HO; ++k) {
            const uint32_t tb = sig(136 + k), tw = sig(140 + k), nw = sig(144 + k);
            if (tb == 0 || tw == 0 || nw == 0) du = 0;
            tong_bang += tb; tong_wgt += tw; tong_word += nw;
            std::printf("    ho %d: bang %6u ck · trong so %6u ck / %4u tu 64b "
                        "= %5.2f ck/tu\n", k, tb, tw, nw,
                        nw ? (double)tw / nw : 0.0);
        }
        if (!du) {
            std::printf("  FAIL  mot trong cac con so TC2 bang 0 -- khe SIG khong duoc ghi\n");
            ++apb_lech;
        } else {
            const uint32_t t_suy0 = sig(80);   // t_suy cua ho 0, luot thuan
            std::printf("    TONG bon ho: bang %ld ck · trong so %ld ck / %ld tu "
                        "= %.2f ck/tu\n", tong_bang, tong_wgt, tong_word,
                        (double)tong_wgt / tong_word);
            std::printf("    trong so / (trong so + bang) = %.1f %% cua pha doi mo hinh\n",
                        100.0 * tong_wgt / (tong_wgt + tong_bang));
            if (t_suy0)
                std::printf("    ho 0: trong so %u ck so voi suy luan %u ck = %.1f %%\n",
                            sig(140), t_suy0, 100.0 * sig(140) / t_suy0);
            // HAI CHIEN LUOC BOM, do tren CUNG bo trong so trong CUNG mot luot.
            const uint32_t s_cu = sig(148), s_goi = sig(149), s_ntu = sig(150);
            // IN CA GIA TRI KY VONG, khong chi gia tri do duoc: phien ben canh
            // vua tao ra mot cong XANH vi no in "da khai 0 dong" -- mot gia tri
            // hop le ve kieu ma chi dang ngo NEU nguoi doc biet phai co 8. Nen
            // luat la: in gia tri trung gian VA in ky vong khi co mot.
            std::printf("    hai chien luoc bom (ho 0, %u tu, ky vong %u):\n",
                        s_ntu, sig(144));
            if (s_cu == 0 || s_goi == 0 || s_ntu == 0) {
                std::printf("  FAIL  phep do hai chien luoc rong (cu=%u goi=%u ntu=%u)\n",
                            s_cu, s_goi, s_ntu);
                ++apb_lech;
            } else if (s_ntu != sig(144)) {
                std::printf("  FAIL  so tu khac lan chay chinh (%u vs %u) -- hai phep "
                            "do khong so duoc\n", s_ntu, sig(144));
                ++apb_lech;
            } else {
                std::printf("      doc STATUS moi tu : %6u ck = %5.2f ck/tu\n",
                            s_cu, (double)s_cu / s_ntu);
                std::printf("      doi RONG, day %u  : %6u ck = %5.2f ck/tu\n",
                            8u, s_goi, (double)s_goi / s_ntu);
                std::printf("      chenh %+d ck (%+.1f %%)\n", (int)s_goi - (int)s_cu,
                            100.0 * ((double)s_goi - (double)s_cu) / (double)s_cu);
                const uint32_t s_gio = sig(151);
                if (s_gio)
                    std::printf("      + chot HET GIO trong vong: %6u ck = %5.2f ck/tu "
                                "(chenh %+.2f ck/tu so voi khong chot)\n", s_gio,
                                (double)s_gio / s_ntu,
                                ((double)s_gio - (double)s_cu) / s_ntu);
                const uint32_t s_nq = sig(152), s_nqw = sig(153);
                if (s_nq) {
                    std::printf("      + chot chi tren NHANH QUAY: %6u ck = %5.2f ck/tu "
                                "(%u/%u tu, ky vong %u)\n", s_nq, (double)s_nq / s_ntu,
                                s_nqw, s_ntu, s_ntu);
                    if (s_nqw != s_ntu) {
                        std::printf("  FAIL  vong chot-nhanh-quay chi bom %u/%u tu\n",
                                    s_nqw, s_ntu);
                        ++apb_lech;
                    } else if (s_gio && s_nq < s_gio)
                        std::printf("      -> giu AN TOAN va bo %.2f ck/tu (%d ck moi lan doi mo hinh)\n",
                                    ((double)s_gio - (double)s_nq) / s_ntu,
                                    (int)s_gio - (int)s_nq);
                }
                const uint32_t s_dn = sig(154), s_dnw = sig(155);
                if (s_dn) {
                    std::printf("      + duong nhanh KHONG CO GI THEM: %6u ck = %5.2f ck/tu "
                                "(%u/%u tu)\n", s_dn, (double)s_dn / s_ntu, s_dnw, s_ntu);
                    if (s_dnw != s_ntu) {
                        std::printf("  FAIL  vong duong-nhanh-sach chi bom %u/%u tu\n",
                                    s_dnw, s_ntu); ++apb_lech;
                    } else if (s_gio)
                        std::printf("      -> so voi chot-moi-tu: %+.2f ck/tu, %+d ck moi lan "
                                    "doi mo hinh, VA giu nguyen an toan\n",
                                    ((double)s_dn - (double)s_gio) / s_ntu,
                                    (int)s_dn - (int)s_gio);
                }
                const uint32_t s_q = sig(156), s_qw = sig(157);
                if (s_qw == s_ntu) {
                    std::printf("      SO LAN FIFO DAY: %u tren %u tu (%.2f lan/tu)\n",
                                s_q, s_ntu, (double)s_q / s_ntu);
                    if (s_q < s_ntu / 10)
                        std::printf("      -> FIFO gan nhu KHONG day. Nen `+6 ck/tu` cua bien "
                                    "the (e) la BO CUC MA, khong nhanh quay nong. Suy luan "
                                    "\"vong bi chan boi rut\" SAI.\n");
                    else
                        std::printf("      -> FIFO day thuong xuyen: vong BI CHAN BOI RUT.\n");
                }
                const uint32_t s_bo = sig(158), s_er = sig(159);
                if (s_bo) {
                    std::printf("      + BO HAN phep chot: %6u ck = %5.2f ck/tu · "
                                "ERRSTAT=0x%03X (bit3 wfifo tran phai 0)\n",
                                s_bo, (double)s_bo / s_ntu, s_er);
                    if (s_er & (1u << 3)) {
                        std::printf("  FAIL  wfifo TRAN khi bo phep chot -- gia dinh so hoc SAI\n");
                        ++apb_lech;
                    } else
                        std::printf("      -> %.2f ck/tu, thu hoi %+.2f ck/tu so voi ban dang "
                                    "dung (%d ck moi lan doi mo hinh, %d ck moi nhip bon ho)\n",
                                    (double)s_bo / s_ntu,
                                    ((double)s_gio - (double)s_bo) / s_ntu,
                                    (int)s_gio - (int)s_bo,
                                    (int)(((double)s_gio - (double)s_bo) / s_ntu * 2294));
                }
                if (s_goi >= s_cu)
                    std::printf("      LUU Y: chien luoc GOI khong nhanh hon -- phep "
                                "toi uu KHONG co tac dung o cau hinh nay\n");
            }
            std::printf("  ok    TC2: co hoi cua mot AXI master co burst DO DUOC\n");
        }
    }

    // ==================== TC4 · HAI MAT PHANG DIEU KHIEN, do tren cung cong viec
    {
        const uint32_t t_x = sig(128), t_m = sig(129), nl = sig(130);
        const uint32_t x_ok = sig(131), m_ok = sig(132);
        std::printf("=== TC4 · mot mo rong ISA so voi mot khoi thanh ghi\n");
        if (nl == 0 || t_x == 0 || t_m == 0) {
            std::printf("  FAIL  phep do TC4 rong (n_layer=%u CVXIF=%u MMIO=%u)\n",
                        nl, t_x, t_m);
            ++apb_lech;
        } else if (x_ok != nl || m_ok != nl) {
            std::printf("  FAIL  khong phai duong nao cung chay het %u lop "
                        "(CVXIF %u, MMIO %u) -- hai con so khong so duoc\n",
                        nl, x_ok, m_ok);
            ++apb_lech;
        } else {
            std::printf("    %u lop cua ho 0, CUNG trang thai da nap:\n", nl);
            std::printf("    CVXIF %u ck · MMIO %u ck · chenh %+d ck "
                        "(%+.2f %% cua %u ck)\n", t_x, t_m, (int)t_m - (int)t_x,
                        100.0 * ((double)t_m - (double)t_x) / (double)t_x, t_x);
            std::printf("    moi lop: CVXIF %.1f ck · MMIO %.1f ck · chenh %+.1f ck\n",
                        (double)t_x / nl, (double)t_m / nl,
                        ((double)t_m - (double)t_x) / nl);
            if (t_m <= t_x)
                std::printf("    LUU Y: duong MMIO KHONG cham hon -- mot lenh khong "
                            "re hon hai phep ghi o cau hinh nay\n");
            std::printf("  ok    TC4: chenh mat phang dieu khien DO DUOC, khong uoc\n");
        }
    }

    if (apb_lech) {
        std::printf("tb_ecg_soc: FAIL (%d loi o pha APB -- cong APB4 cua ecg_soc "
                    "khong cham duoc thanh ghi)\n", apb_lech);
        ECG_COV_WRITE(); delete dut; return 1;
    }
    ECG_COV_WRITE();
  dut->final();   // chay cac khoi `final` cua RTL (bao cao [F03])
  delete dut;
    return loi;
}

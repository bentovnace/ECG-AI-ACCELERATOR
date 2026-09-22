// Testbench cho ecg_mmio -- ngoai vi anh xa bo nho noi bus du lieu OBI voi 34 cong
// tho cua ecg_coproc.
//
// DAC TA DUOC KIEM. Moi phep thu duoi day nham MOT dieu khoan cu the, va dieu
// khoan do lay tu MOT nguon xac dinh chu khong tu suy dien:
//
//   D1 (tu tb_ecg_coproc.cpp:180-186) -- DOC DESCRIPTOR LA TO HOP.
//      `desc_valid_o` va `desc_word_o` phai co gia tri dung trong CUNG chu ky ma
//      `desc_req_i` len, TRUOC suon dong ho. Neu lam mot chu ky tre thi bo dong
//      xu ly doc rac o chu ky dau va tinh sai ca lop. T3 kiem dieu nay bang cach
//      dat req roi doc word MA KHONG tick.
//
//   D2 (tu tb_ecg_coproc.cpp:190-200) -- DOC SCALE/BIAS LA TO HOP.
//      Hai cong nay khong co tin hieu valid nao di kem, nen khong co cach nao khac.
//      T4, T5 kiem khong tick.
//
//   D3 (tu tb_ecg_coproc.cpp:197) -- BIAS LA 9 BIT CO DAU, khong phai int8.
//      Chu thich o do ghi: mot muc bias that la -170, va ep ve int8 lam no thanh
//      +86, tuc CA MOT KENH RA sai. T5 dung DUNG gia tri -170 lam ca thu, va no
//      la mot phep thu PHAN DINH: neu cai dat ep ve int8 thi T5 doc ra +86.
//
//   D4 -- START LA MOT XUNG MOT CHU KY, khong phai mot muc giu.
//      `ecg_coproc.start_i` khoi dong mot lan suy luan; giu no cao se khoi dong
//      lai lien tuc. T2 dem so chu ky `cp_start_o` cao va doi DUNG 1.
//
//   D5 -- FIFO TRONG SO day theo HAI BUOC (ghi nua thap, roi nua cao MOI day).
//      Bus 32 bit, tu trong so 64 bit. T6 kiem ca thu tu va viec ghi nua thap MOT
//      MINH thi KHONG day gi.
//
//   D6 -- CHI THU KET QUA CUA LOP CUOI. `wr_i` no o moi phep ghi cua moi lop; neu
//      thu tat ca thi mot nhip m4 sinh hang chuc nghin phep ghi (bo dem hoat do
//      6.144 byte) va FIFO tran ngay. T7 gui hai phep ghi, MOT co last=1 va mot
//      co last=0, roi doi RESCNT = 1.
//
//   D7 -- NAM CHOT phai QUAN SAT DUOC, khong im lang:
//        bit3 FIFO trong so tran · bit4 FIFO ket qua tran · bit5 nap khi busy
//        bit6 truy cap khong phai ca tu
//      (bit0-2 la chi so ngoai bang; voi tham so mac dinh chung KHONG THE xay ra
//       vi bang phu het khong gian dia chi -- xem generate trong ecg_mmio.sv --
//       nen T11 kiem chung noi cung 0 chu khong kiem chung no.)
//
// MOI PHEP THU LA MOT PHEP PHAN DINH: no phai that bai neu cai dat sai theo dung
// cach ma dieu khoan cam. Mot phep thu chi "chay duoc" thi khong chung minh gi.

#include <cstdio>
#include <vector>
#include <cstdint>
#include "Vecg_mmio.h"
#include "verilated.h"
#include "ecg_cov.h"

static Vecg_mmio *dut;
static vluint64_t main_time = 0;
double sc_time_stamp() { return main_time; }

static int fails = 0;
// DEM NHOM tu chinh nhan `Tnn` cua tung phep kiem, thay vi KHAI mot con so.
// Chuoi PASS truoc day ghi "19 nhom" bang mot hang go tay: khi them T20 no van
// ghi 19, va neu ai XOA mot nhom no cung van ghi 19. Mot chuoi ket qua duoc
// LUONG TU HOA boi mot dai luong thi dai luong do phai duoc DO.
#include <set>
#include <string>
static std::set<std::string> nhom_thay;
static void ghi_nhom(const char *what) {
    if (what && what[0] == 'T') {
        std::string t;
        for (const char *p = what; *p && *p != ' '; ++p) t += *p;
        if (t.size() >= 2) nhom_thay.insert(t);
    }
}
static void fail(const char *what, long got, long want) {
    ghi_nhom(what);
    std::printf("  FAIL  %-52s duoc %ld, ky vong %ld\n", what, got, want);
    ++fails;
}
static void ok(const char *what) { ghi_nhom(what); std::printf("  ok    %s\n", what); }

static void tick() {
    dut->clk_i = 0; dut->eval(); ++main_time;
    dut->clk_i = 1; dut->eval(); ++main_time;
}

// ---- ban do dia chi (phai khop dau ecg_mmio.sv) ----
enum {
    A_CTRL = 0x000, A_STATUS = 0x004, A_LAYER = 0x008, A_NLAYERS = 0x00C,
    A_INLEN = 0x010, A_DMALEN = 0x014, A_RESCNT = 0x018, A_RESRD = 0x01C,
    A_WFIFO_L = 0x020, A_WFIFO_H = 0x024, A_PRE = 0x028, A_ERR = 0x02C,
    A_PREADDR = 0x030, A_PRE4 = 0x034,
    A_DESC = 0x400, A_SCALE = 0x800, A_BIAS = 0x1000
};

static void bus_idle() { dut->req_i = 0; dut->we_i = 0; dut->be_i = 0xF; }

static void wr32(uint32_t addr, uint32_t data, uint8_t be = 0xF) {
    dut->req_i = 1; dut->we_i = 1; dut->be_i = be;
    dut->addr_i = addr; dut->wdata_i = data;
    tick();
    bus_idle();
}

static uint32_t rd32(uint32_t addr) {
    dut->req_i = 1; dut->we_i = 0; dut->be_i = 0xF; dut->addr_i = addr;
    tick();                 // giao dich duoc chap nhan
    bus_idle();
    tick();                 // rvalid + rdata o chu ky sau
    // rvalid_o len o chu ky nay; doc rdata_o ngay.
    return dut->rdata_o;
}

// Descriptor 128 bit: Verilator phoi thanh mang bon tu 32 bit.
static uint32_t desc_word(int i) { return dut->desc_word_o[i]; }

static void reset() {
    dut->rst_ni = 0; bus_idle();
    dut->desc_req_i = 0; dut->desc_idx_i = 0;
    dut->s_off_i = 0; dut->b_off_i = 0;
    dut->cp_busy_i = 0; dut->cp_done_i = 0;
    dut->dma_busy_i = 0; dut->dma_done_i = 0;
    dut->ws_ready_i = 0;
    dut->wr_i = 0; dut->wr_buf_i = 0; dut->wr_off_i = 0; dut->wr_data_i = 0;
    dut->wr_last_layer_i = 0;
    dut->cp_desc_illegal_i = 0;
    for (int i = 0; i < 5; ++i) tick();
    dut->rst_ni = 1;
    tick();
}

int main(int argc, char **argv) {
    Verilated::commandArgs(argc, argv);
    dut = new Vecg_mmio;
    reset();

    // ---- T1: thanh ghi cau hinh di va ve nguyen ven -----------------------
    wr32(A_LAYER, 37);
    wr32(A_NLAYERS, 13);
    wr32(A_INLEN, 256);
    wr32(A_DMALEN, 4664);
    if (dut->cp_layer_o != 37) fail("T1 cp_layer_o", dut->cp_layer_o, 37);
    else if (dut->cp_n_layers_o != 13) fail("T1 cp_n_layers_o", dut->cp_n_layers_o, 13);
    else if (dut->cp_in_len_o != 256) fail("T1 cp_in_len_o", dut->cp_in_len_o, 256);
    else if (dut->dma_len_o != 4664) fail("T1 dma_len_o", dut->dma_len_o, 4664);
    else if (rd32(A_LAYER) != 37) fail("T1 doc lai LAYER", rd32(A_LAYER), 37);
    else ok("T1 thanh ghi cau hinh: ghi -> cong coproc va doc lai khop");

    // ---- T2 (D4): start la MOT XUNG, khong phai muc giu -------------------
    {
        int cao = 0;
        dut->req_i = 1; dut->we_i = 1; dut->be_i = 0xF;
        dut->addr_i = A_CTRL; dut->wdata_i = 0x1;   // bit0 = start
        tick();
        bus_idle();
        for (int i = 0; i < 6; ++i) { if (dut->cp_start_o) ++cao; tick(); }
        if (cao != 1) fail("T2 (D4) so chu ky cp_start_o cao", cao, 1);
        else ok("T2 (D4) start la mot xung MOT chu ky, khong giu");
    }

    // ---- T3 (D1): doc descriptor la TO HOP, khong tick --------------------
    {
        // Ghi bon tu vao ban ghi lop 5.
        const uint32_t w[4] = {0x11111111u, 0x22222222u, 0x33333333u, 0xDEADBEEFu};
        for (int i = 0; i < 4; ++i) wr32(A_DESC + 5 * 16 + 4 * i, w[i]);
        // Dat req va idx roi DANH GIA MA KHONG TICK -- day la dieu khoan.
        dut->desc_req_i = 1; dut->desc_idx_i = 5;
        dut->eval();
        bool khop = dut->desc_valid_o == 1;
        for (int i = 0; i < 4 && khop; ++i) khop = (desc_word(i) == w[i]);
        if (!khop) {
            fail("T3 (D1) desc to hop: valid", dut->desc_valid_o, 1);
            for (int i = 0; i < 4; ++i)
                std::printf("        tu %d: duoc %08x, ky vong %08x\n", i, desc_word(i), w[i]);
        } else {
            ok("T3 (D1) doc descriptor TO HOP trong cung chu ky voi desc_req_i");
        }
        // Doc mot ban ghi KHAC, van khong tick: chi so phai doi ngay.
        dut->desc_idx_i = 6; dut->eval();
        if (desc_word(3) == 0xDEADBEEFu)
            fail("T3 (D1) doi idx phai doi du lieu ngay", 1, 0);
        else ok("T3 (D1) doi desc_idx_i doi du lieu ngay, khong can chu ky");
        dut->desc_req_i = 0; dut->eval();
    }

    // ---- T4 (D2): bang scale to hop, tach nhan/dich dung ------------------
    {
        // Mot muc: nhan 11 bit o [10:0], dich 5 bit o [15:11].
        const uint32_t mult = 0x5A3;    // 1443, vua 11 bit
        const uint32_t shift = 19;      // vua 5 bit
        const uint32_t packed = (shift << 11) | mult;
        wr32(A_SCALE + 4 * 77, packed);       // MOT muc moi tu: offset = muc*4
        dut->s_off_i = 77; dut->eval();
        if (dut->s_mult_o != mult) fail("T4 (D2) s_mult_o", dut->s_mult_o, mult);
        else if (dut->s_shift_o != shift) fail("T4 (D2) s_shift_o", dut->s_shift_o, shift);
        else ok("T4 (D2) bang scale to hop, tach nhan 11 bit / dich 5 bit dung");
    }

    // ---- T5 (D3): bias 9 BIT CO DAU -- phep thu PHAN DINH -----------------
    {
        // -170 trong 9 bit hai bu = 0x156. Neu cai dat ep ve int8 thi ket qua
        // doc ra la +86 (0x56), va phep thu nay BAT duoc dung cho do.
        const int want = -170;
        wr32(A_BIAS + 4 * 300, 0x156);
        dut->b_off_i = 300; dut->eval();
        int got = static_cast<int>(dut->s_bias_o);
        if (got >= 256) got -= 512;           // Verilator phoi 9 bit khong dau
        if (got != want) {
            fail("T5 (D3) bias 9 bit co dau (-170)", got, want);
            if (got == 86) std::printf("        +86 = dau hieu cua viec EP VE int8\n");
        } else {
            ok("T5 (D3) bias 9 bit CO DAU: -170 giu nguyen, khong thanh +86");
        }
    }

    // ---- T6 (D5): FIFO trong so, day theo hai buoc -------------------------
    {
        dut->ws_ready_i = 0;
        // Ghi NUA THAP mot minh: khong duoc day gi.
        wr32(A_WFIFO_L, 0xAAAABBBBu);
        if (dut->ws_valid_o != 0) fail("T6 (D5) nua thap mot minh khong duoc day", 1, 0);
        else ok("T6 (D5) ghi nua thap MOT MINH: FIFO van rong");
        // Ghi nua cao: day mot tu 64 bit.
        wr32(A_WFIFO_H, 0xCCCCDDDDu);
        if (dut->ws_valid_o != 1) {
            fail("T6 (D5) sau nua cao ws_valid_o", dut->ws_valid_o, 1);
        } else {
            uint64_t got = dut->ws_data_o;
            uint64_t want = (uint64_t(0xCCCCDDDDu) << 32) | 0xAAAABBBBu;
            if (got != want) {
                std::printf("  FAIL  T6 (D5) ws_data_o duoc %016llx, ky vong %016llx\n",
                            (unsigned long long)got, (unsigned long long)want);
                ++fails;
            } else {
                ok("T6 (D5) nua cao DAY mot tu 64 bit, thu tu thap/cao dung");
            }
        }
        // Rut: ws_valid_o phai ha khi rong.
        dut->ws_ready_i = 1; tick(); dut->ws_ready_i = 0; dut->eval();
        if (dut->ws_valid_o != 0) fail("T6 (D5) sau khi rut, FIFO phai rong", dut->ws_valid_o, 0);
        else ok("T6 (D5) rut mot tu -> FIFO rong, ws_valid_o ha");
    }

    // ---- T7 (D6): chi thu ket qua cua LOP CUOI -----------------------------
    {
        wr32(A_CTRL, 1u << 5);   // xoa ERRSTAT truoc khi do
        // mot phep ghi lop GIUA (last = 0): khong duoc thu
        dut->wr_i = 1; dut->wr_buf_i = 2; dut->wr_off_i = 100;
        dut->wr_data_i = 33; dut->wr_last_layer_i = 0;
        tick();
        // mot phep ghi lop CUOI (last = 1): phai thu
        dut->wr_buf_i = 3; dut->wr_off_i = 7; dut->wr_data_i = -5;
        dut->wr_last_layer_i = 1;
        tick();
        dut->wr_i = 0; dut->wr_last_layer_i = 0; dut->eval();
        uint32_t cnt = rd32(A_RESCNT);
        if (cnt != 1) fail("T7 (D6) RESCNT sau 1 giua + 1 cuoi", cnt, 1);
        else {
            ok("T7 (D6) chi thu lop CUOI: 2 phep ghi -> RESCNT = 1");
            uint32_t r = rd32(A_RESRD);
            const int ABITS = 13;
            int data = static_cast<int8_t>(r & 0xFF);
            int off  = (r >> 8) & ((1 << ABITS) - 1);
            int buf  = (r >> (8 + ABITS)) & 0xF;
            int last = (r >> (8 + ABITS + 4)) & 1;
            if (data != -5 || off != 7 || buf != 3 || last != 1) {
                std::printf("  FAIL  T7 (D6) RESRD giai ma: data=%d off=%d buf=%d last=%d"
                            " (ky vong -5 / 7 / 3 / 1)\n", data, off, buf, last);
                ++fails;
            } else {
                ok("T7 (D6) RESRD tra dung {last, buf, off, data} co dau");
            }
            if (rd32(A_RESCNT) != 0) fail("T7 (D6) RESCNT sau khi rut", rd32(A_RESCNT), 0);
            else ok("T7 (D6) rut mot ket qua -> RESCNT ve 0");
        }
    }

    // ---- T8 (D7 bit5): nap khi busy phai dat co ----------------------------
    {
        wr32(A_CTRL, 1u << 5);       // xoa ERRSTAT
        dut->cp_busy_i = 1; dut->eval();
        wr32(A_PRE, (3u << 21) | (11u << 8) | 0x7Fu);
        uint32_t e = rd32(A_ERR);
        bool co_pre = dut->pre_we_o != 0;
        dut->cp_busy_i = 0; dut->eval();
        if (!(e & (1u << 5))) fail("T8 (D7) nap khi busy phai dat ERRSTAT bit5", e, 32);
        else if (co_pre) fail("T8 (D7) nap khi busy KHONG duoc ghi thuc", 1, 0);
        else ok("T8 (D7) nap khi busy: dat co bit5 VA khong ghi thuc");
    }

    // ---- T9: nap khi RANH thi ghi thuc, va giai ma truong dung -------------
    {
        wr32(A_CTRL, 1u << 5);
        // {buf=5, off=1234, data=-3}
        uint32_t w = (5u << 21) | (1234u << 8) | 0xFDu;
        dut->req_i = 1; dut->we_i = 1; dut->be_i = 0xF;
        dut->addr_i = A_PRE; dut->wdata_i = w;
        tick();
        bus_idle();
        int d = static_cast<int8_t>(dut->pre_data_o);
        if (!dut->pre_we_o) fail("T9 nap khi ranh phai co pre_we_o", 0, 1);
        else if (dut->pre_buf_o != 5) fail("T9 pre_buf_o", dut->pre_buf_o, 5);
        else if (dut->pre_off_o != 1234) fail("T9 pre_off_o", dut->pre_off_o, 1234);
        else if (d != -3) fail("T9 pre_data_o co dau", d, -3);
        else ok("T9 nap khi ranh: pre_we_o mot xung, buf/off/data giai ma dung");
        tick();
        if (dut->pre_we_o) fail("T9 pre_we_o phai la MOT xung", 1, 0);
        else ok("T9 pre_we_o la mot xung mot chu ky");
    }

    // MUC APB-04 (danh gia 2026-09-05): do anh huong len THANH GHI THAT, khong
    // chi kiem `be_o`. Ca nay dat nen LAYER = 9 truoc, roi doi CA HAI: co bit6
    // duoc dat VA LAYER van la 9. Kiem mot minh co bit6 se xanh voi mot thiet ke
    // dat co roi VAN ghi -- do dung la loi F08 da co that.
    // ---- T10 (D7 bit6): truy cap khong phai ca tu -- dat co VA KHONG GHI ---
    //
    // W1-D (F07+F08): ban truoc chi kiem CO. Nhung dat co ma van ghi thi trang
    // thai VAN doi trong khi co bao loi -- va do dung la lo hong. Nen phep kiem
    // phai doi CA HAI: co len, VA thanh ghi giu nguyen.
    //
    // Nen la mot phep ghi DAY DU gia tri 9, chu khong phai gia tri mac dinh:
    // mot phep ghi mot phan bi tu choi va mot phep ghi khong lam gi cho ra CUNG
    // trang thai, nen nen phai la mot gia tri PHAN BIET DUOC.
    {
        wr32(A_CTRL, 1u << 5);
        wr32(A_LAYER, 9);            // nen day du, phan biet duoc
        if (rd32(A_LAYER) != 9) fail("T10 khong dat duoc nen LAYER = 9", rd32(A_LAYER), 9);
        wr32(A_LAYER, 4, 0x1);       // be = 0001 -> mot byte
        uint32_t e = rd32(A_ERR);
        if (!(e & (1u << 6))) fail("T10 (D7) truy cap nua tu phai dat ERRSTAT bit6", e, 64);
        else if (rd32(A_LAYER) != 9)
            fail("T10 (D7) ghi mot phan VAN doi thanh ghi", rd32(A_LAYER), 9);
        else ok("T10 (D7) truy cap khong phai ca tu: dat co bit6 VA khong ghi");
    }

    // ---- T10b (MUC APB-08): xoa ERRSTAT KHONG duoc nuot mot loi den CUNG chu ky
    // Muc APB-08 doi: "firmware doc/xoa loi khong lam mat loi moi cung chu ky".
    // `ecg_mmio.sv` dat cac bit loi o muc tren cua `always_ff` (dong 486-489, 529)
    // con lenh xoa `err_q <= '0` nam SAU trong cung khoi (dong 556). Voi gan khong
    // chan thi phep gan CUOI thang, nen mot loi noi len dung chu ky firmware ghi
    // CTRL bit5 se bi lenh xoa GHI DE -- va mat im lang.
    // `cp_desc_illegal_i` la mot cong VAO nen ca nay dung duoc mot chu ky trung that,
    // khong phai mot mo phong.
    {
        wr32(A_CTRL, 1u << 5);                  // nen sach
        if (rd32(A_ERR) != 0) fail("T10b khong xoa duoc ERRSTAT lam nen", rd32(A_ERR), 0);
        // loi MOI va lenh XOA roi vao CUNG mot canh len
        dut->cp_desc_illegal_i = 1;
        dut->req_i = 1; dut->we_i = 1; dut->be_i = 0xF;
        dut->addr_i = A_CTRL; dut->wdata_i = (1u << 5);
        tick();
        bus_idle();
        dut->cp_desc_illegal_i = 0;
        uint32_t e = rd32(A_ERR);
        if (!(e & (1u << 8)))
            fail("T10b (APB-08) loi den CUNG chu ky voi lenh xoa bi NUOT", e, 256);
        else ok("T10b (APB-08) loi moi cung chu ky voi lenh xoa VAN duoc giu");
    }

    // ---- T11 (D7 bit4): FIFO ket qua tran phai dat co ---------------------
    {
        wr32(A_CTRL, 1u << 5);
        dut->wr_i = 1; dut->wr_last_layer_i = 1; dut->wr_buf_i = 1;
        for (int i = 0; i < 40; ++i) {     // sau hon RFIFO_DEPTH = 32
            dut->wr_off_i = i; dut->wr_data_i = i & 0x7F;
            tick();
        }
        dut->wr_i = 0; dut->wr_last_layer_i = 0; dut->eval();
        uint32_t e = rd32(A_ERR);
        uint32_t cnt = rd32(A_RESCNT);
        if (!(e & (1u << 4))) fail("T11 (D7) FIFO ket qua tran phai dat bit4", e, 16);
        else if (cnt != 32) fail("T11 so ket qua giu duoc", cnt, 32);
        else ok("T11 (D7) FIFO ket qua tran: dat co bit4, giu dung 32 muc");
        // bit0-2 (chi so ngoai bang) phai la 0 voi tham so mac dinh: bang phu het
        // khong gian dia chi nen KHONG THE ngoai bang. Neu chung len thi do la
        // loi cat bit ma verilator tung bat (6'(64) = 0).
        if (e & 0x7) fail("T11 co chi so ngoai bang phai la 0 (bang phu het)", e & 7, 0);
        else ok("T11 co chi so ngoai bang = 0 voi tham so mac dinh (bang phu het)");
    }

    // ---- T12: STATUS dinh va CTRL xoa -------------------------------------
    {
        dut->cp_done_i = 1; tick(); dut->cp_done_i = 0; tick();
        uint32_t st = rd32(A_STATUS);
        if (!(st & 0x2)) fail("T12 done phai DINH trong STATUS bit1", st, 2);
        else {
            ok("T12 done dinh trong STATUS bit1 sau khi cp_done_i da ha");
            wr32(A_CTRL, 1u << 3);   // xoa co done
            st = rd32(A_STATUS);
            if (st & 0x2) fail("T12 CTRL bit3 phai xoa co done", st, 0);
            else ok("T12 CTRL bit3 xoa co done");
        }
    }

    // ---- T14: OBI doi rvalid cho CA phep GHI ------------------------------
    {
        // Hoi quy cho mot loi THAT: ban dau ecg_mmio chi phat rvalid_o cho phep
        // DOC, va loi CV32E40X dung vinh vien o lenh `sw` dau tien cua main --
        // do duoc trong mo phong cap he: PC dung o 0xf8 = `sw s0,8(sp)` suot 2
        // trieu chu ky. OBI doi DUNG MOT dap ung cho MOI giao dich duoc nhan.
        dut->req_i = 1; dut->we_i = 1; dut->be_i = 0xF;
        dut->addr_i = A_LAYER; dut->wdata_i = 21;
        tick();                      // giao dich duoc nhan; rvalid chot o suon nay
        bus_idle();
        dut->eval();
        if (!dut->rvalid_o) fail("T14 phep GHI phai co rvalid_o", 0, 1);
        else ok("T14 phep GHI co rvalid_o (OBI: mot dap ung cho MOI giao dich)");
        tick();
        if (dut->rvalid_o) fail("T14 rvalid_o phai la MOT xung", 1, 0);
        else ok("T14 rvalid_o la mot xung, khong giu");
    }

    // ---- T15: DAY va RUT trong CUNG chu ky -> so dem GIU NGUYEN --------------
    {
        // Hoi quy cho mot loi THAT da lam ket qua suy luan sai. Ban truoc cap nhat
        // so dem o HAI cho long nhau, nen khi day va rut trung chu ky thi so dem
        // GIAM thay vi giu nguyen. So dem troi xuong -> ws_valid_o sai -> DMA doc
        // mot tu CU -> trong so lech -> diem lop sai. Va no PHU THUOC THOI GIAN,
        // nen trieu chung trong nhu bat dinh.
        reset();
        dut->ws_ready_i = 0;
        // Day ba tu de FIFO khong rong.
        for (int i = 0; i < 3; ++i) {
            wr32(A_WFIFO_L, 0x1000u + i);
            wr32(A_WFIFO_H, 0x2000u + i);
        }
        // Ghi nua THAP truoc, VOI ws_ready = 0: mot phep ghi vao 0x20 khong day gi,
        // nhung neu de ws_ready = 1 o day thi co MOT lan rut ma phep thu khong tinh
        // -- va do dung la loi ma ban truoc cua phep thu nay mac.
        wr32(A_WFIFO_L, 0xAAAA0000u);
        // Nay moi cho trung: day (0x24) VA rut trong CUNG chu ky.
        dut->ws_ready_i = 1;
        dut->req_i = 1; dut->we_i = 1; dut->be_i = 0xF;
        dut->addr_i = A_WFIFO_H; dut->wdata_i = 0xBBBB0000u;
        tick();                       // day (0x24) VA rut (ws_ready) cung luc
        bus_idle();
        dut->ws_ready_i = 0; dut->eval();
        // Da day 4, rut 1 -> phai con 3. Rut het roi dem.
        int rut = 0;
        dut->ws_ready_i = 1;
        for (int i = 0; i < 12 && dut->ws_valid_o; ++i) { tick(); ++rut; dut->eval(); }
        dut->ws_ready_i = 0; dut->eval();
        if (rut != 3) fail("T15 so tu con lai sau day+rut cung chu ky", rut, 3);
        else ok("T15 day VA rut cung chu ky: so dem GIU NGUYEN (con dung 3 tu)");
        uint32_t e = rd32(A_ERR);
        if (e & (1u << 3)) fail("T15 khong duoc bat co FIFO tran", 8, 0);
        else ok("T15 khong co co FIFO trong so tran");
    }

    // ---- T17: chi so NGOAI BANG tu duong BUS -------------------------------
    {
        // Ba chot desc_oob/s_oob/b_oob canh duong CO PROC. Duong BUS la mot cong
        // KHAC va no KHONG duoc canh -- mot ra soat ngoai bat duoc.
        //
        //   desc_mem[50]  chi so a[9:4]  6 bit -> 0..63   -> 14 chi so ngoai bang
        //   bias_mem[562] chi so a[11:2] 10 bit -> 0..1023 -> 462 chi so ngoai bang
        //   scale_mem[256] chi so a[9:2] 8 bit -> 0..255   -> VUA KHIT, khong ngoai
        //
        // PHEP THU PHAI PHAN DINH BA DIEU, khong chi "co dat co":
        //   1. trong bang van ghi/doc duoc  (mot chot luon no thi vo dung)
        //   2. ngoai bang KHONG lam hong muc trong bang  (phep thu chong lan)
        //   3. doc ngoai bang tra MAU PHAN BIET, khong tra 0 -- vi mot muc bang
        //      hop le rat co the la 0, nen 0 khong phan biet duoc hai truong hop
        const uint32_t E_OOB = 1u << 7;
        reset();

        // (1) trong bang: ghi muc descriptor 49 (muc cuoi hop le) va bias 561.
        for (int i = 0; i < 4; ++i) wr32(A_DESC + 49 * 16 + 4 * i, 0xA5000000u + i);
        wr32(A_BIAS + 4 * 561, 0x0FF);
        uint32_t e = rd32(A_ERR);
        if (e & E_OOB) fail("T17 (1) trong bang KHONG duoc dat co ngoai bang", 1, 0);
        else ok("T17 (1) chi so trong bang: khong dat co");
        if (rd32(A_DESC + 49 * 16) != 0xA5000000u)
            fail("T17 (1) doc lai descriptor 49", rd32(A_DESC + 49 * 16), 0xA5000000u);
        else ok("T17 (1) descriptor 49 (muc cuoi hop le) di va ve nguyen ven");
        if ((rd32(A_BIAS + 4 * 561) & 0x1FF) != 0x0FF)
            fail("T17 (1) doc lai bias 561", rd32(A_BIAS + 4 * 561) & 0x1FF, 0x0FF);
        else ok("T17 (1) bias 561 (muc cuoi hop le) di va ve nguyen ven");

        // (2) ngoai bang: GHI descriptor 50 va bias 562, roi kiem muc trong bang
        //     KHONG doi. Neu phan cung quan chi so thi 50 -> 0 hay 562 -> 50, va
        //     phep kiem duoi day bat duoc.
        wr32(A_DESC + 50 * 16, 0xDEAD0001u);
        wr32(A_BIAS + 4 * 562, 0x1AA);
        wr32(A_BIAS + 4 * 1023, 0x155);
        e = rd32(A_ERR);
        if (!(e & E_OOB)) fail("T17 (2) ghi ngoai bang phai dat co bit7", 0, 1);
        else ok("T17 (2) ghi ngoai bang: dat co ERRSTAT bit7");
        if (rd32(A_DESC + 49 * 16) != 0xA5000000u)
            fail("T17 (2) ghi ngoai bang lam HONG descriptor 49",
                 rd32(A_DESC + 49 * 16), 0xA5000000u);
        else ok("T17 (2) descriptor 49 khong bi ghi ngoai bang lam hong");
        if (rd32(A_DESC) != 0)
            fail("T17 (2) chi so 50 quan ve 0 -> ghi de descriptor 0", rd32(A_DESC), 0);
        else ok("T17 (2) chi so 50 KHONG quan ve descriptor 0");
        if ((rd32(A_BIAS + 4 * 561) & 0x1FF) != 0x0FF)
            fail("T17 (2) ghi ngoai bang lam HONG bias 561",
                 rd32(A_BIAS + 4 * 561) & 0x1FF, 0x0FF);
        else ok("T17 (2) bias 561 khong bi ghi ngoai bang lam hong");

        // (3) DOC ngoai bang -> mau phan biet.
        wr32(A_CTRL, 1u << 5);              // xoa ERRSTAT
        uint32_t d = rd32(A_DESC + 63 * 16);
        if (d != 0xDEADBEEFu) fail("T17 (3) doc ngoai bang phai tra 0xDEADBEEF", d, (long)0xDEADBEEFu);
        else ok("T17 (3) doc ngoai bang tra 0xDEADBEEF (khong tra 0)");
        if (!(rd32(A_ERR) & E_OOB)) fail("T17 (3) doc ngoai bang phai dat co", 0, 1);
        else ok("T17 (3) doc ngoai bang: dat co ERRSTAT bit7");

        // scale VUA KHIT o cau hinh bon-bo-thuong-tru: khong dia chi nao ngoai bang.
        wr32(A_CTRL, 1u << 5);
        wr32(A_SCALE + 4 * 255, 0x1234);
        if (rd32(A_ERR) & E_OOB)
            fail("T17 scale muc 255 (vua khit) KHONG duoc la ngoai bang", 1, 0);
        else ok("T17 scale muc 255 vua khit: khong dat co");
    }

    // ---- T16: FIFO KET QUA -- DAY va RUT trong CUNG chu ky -----------------
    {
        // HOI QUY cho mot loi THAT, va no la cung mot loi voi T15 o mot FIFO KHAC.
        // Ban truoc sua FIFO TRONG SO bang bon ca tuong minh roi de FIFO KET QUA
        // nguyen cau truc hai nhanh long nhau -- sua mot THE HIEN va de nguyen mot
        // LOP. Mot ra soat ben ngoai bat duoc dieu do.
        //
        // Loi cu: nhanh day gan `rf_cnt_q + 1`, nhanh rut gan `- 1` nhung chan bang
        // `if (!rf_push)`. Khi day va rut trung chu ky thi nhanh rut khong gan, nen
        // bo dem TANG mot trong khi no phai GIU.
        //
        // PHEP THU PHAI PHAN DINH. Day BA muc, roi day muc thu tu DONG THOI voi mot
        // phep doc. Sau do RESCNT phai la 3 (day 4, rut 1). Voi loi cu no la 4:
        //   day 4 muc -> cnt = 4, nhung chu ky trung lam cnt tang thay vi giu -> 4
        // Ba muc con lai phai doc ra DUNG ba gia tri da day, dung thu tu -- vi mot
        // so dem sai co the che mot con tro sai, va nguoc lai.
        reset();
        const int V0 = 11, V1 = 22, V2 = 33, V3 = 44;
        for (int v : {V0, V1, V2}) {
            dut->wr_i = 1; dut->wr_last_layer_i = 1;
            dut->wr_buf_i = 1; dut->wr_off_i = 0; dut->wr_data_i = v;
            tick();
            dut->wr_i = 0; dut->wr_last_layer_i = 0; dut->eval();
        }
        if (rd32(A_RESCNT) != 3) fail("T16 dat truoc: RESCNT sau ba lan day",
                                      rd32(A_RESCNT), 3);

        // Chu ky TRUNG: mot phep DOC 0x1C duoc chap nhan CUNG luc coproc day muc 4.
        dut->wr_i = 1; dut->wr_last_layer_i = 1;
        dut->wr_buf_i = 1; dut->wr_off_i = 0; dut->wr_data_i = V3;
        dut->req_i = 1; dut->we_i = 0; dut->be_i = 0xF; dut->addr_i = A_RESRD;
        tick();                       // day (wr_i) VA rut (doc 0x1C) cung luc
        dut->wr_i = 0; dut->wr_last_layer_i = 0;
        bus_idle();
        tick();                       // rvalid cua phep doc
        const uint32_t d0 = dut->rdata_o;

        const uint32_t cnt = rd32(A_RESCNT);
        if (cnt != 3) fail("T16 RESCNT sau day+rut cung chu ky (giu nguyen)", cnt, 3);
        else ok("T16 FIFO ket qua: day VA rut cung chu ky -> so dem GIU NGUYEN");
        if ((d0 & 0xFF) != (uint32_t)V0)
            fail("T16 muc rut o chu ky trung phai la muc DAU", d0 & 0xFF, V0);
        else ok("T16 muc rut o chu ky trung dung la muc dau (con tro doc dung)");

        // Ba muc con lai, dung thu tu: V1, V2, V3.
        const int cho[3] = {V1, V2, V3};
        int sai = 0;
        for (int i = 0; i < 3; ++i) {
            uint32_t r = rd32(A_RESRD) & 0xFF;
            if (r != (uint32_t)cho[i]) { fail("T16 muc con lai sai thu tu", r, cho[i]); ++sai; }
        }
        if (!sai) ok("T16 ba muc con lai ra dung thu tu (khong mat, khong lap)");
        if (rd32(A_RESCNT) != 0) fail("T16 RESCNT sau khi rut het", rd32(A_RESCNT), 0);
        else ok("T16 RESCNT ve 0 sau khi rut het bon muc");
        uint32_t e = rd32(A_ERR);
        if (e & (1u << 4)) fail("T16 khong duoc bat co FIFO ket qua tran", 16, 0);
        else ok("T16 khong co co FIFO ket qua tran");
    }

    // ---- T13: bit chon nguon dieu khien -----------------------------------
    {
        // Mac dinh phai la 0 = dieu khien tu LENH CUSTOM. Neu mac dinh la 1 thi
        // duong cua luan van (mo rong ISA qua CV-X-IF) bi tat ngam.
        reset();
        if (dut->ctrl_sel_o != 0) fail("T13 ctrl_sel_o sau reset", dut->ctrl_sel_o, 0);
        else ok("T13 mac dinh ctrl_sel_o = 0: dieu khien tu LENH custom");
        wr32(A_CTRL, 1u << 6);
        if (dut->ctrl_sel_o != 1) fail("T13 CTRL bit6 phai dat ctrl_sel_o", dut->ctrl_sel_o, 1);
        else ok("T13 CTRL bit6 chuyen dieu khien sang MMIO");
        wr32(A_CTRL, 0);
        if (dut->ctrl_sel_o != 0) fail("T13 CTRL bit6 = 0 phai tra ve lenh custom", 1, 0);
        else ok("T13 CTRL bit6 = 0 tra dieu khien ve lenh custom");
    }

    // ---- T18: DOC LAI moi thanh ghi va moi bang -------------------------
    //
    // VI SAO NHOM NAY TON TAI, va vi sao no den muon. `make coverage` (moi co)
    // do bao phu dong cua `ecg_mmio.sv` la **77,8 %** -- thap nhat trong 14
    // khoi -- va phan ra ra DUNG tam dong khong bao gio chay:
    //   :513  `default: ;`   ghi vao mot dia chi thanh ghi LA
    //   :544  doc NLAYERS      :545 doc INLEN      :546 doc DMALEN
    //   :558  doc lai bit chon ctrl
    //   :559  `default:`     doc mot dia chi thanh ghi LA
    //   :569  doc bang SCALE   :571 doc bang BIAS
    // Tuc 17 nhom truoc do GHI cac thanh ghi nay ma khong DOC LAI cai nao.
    // Mot loi go o chieu DOC -- `8'h10` tra `dma_len_q` thay vi `in_len_q` --
    // se di qua toan bo 17 nhom ma khong ai thay: chieu ghi van dung, chieu doc
    // khong ai hoi. Day dung la lop loi "dat ma khong doc" ma kho nay da gap
    // mot lan; lan nay bao phu chi ra CHO no con lai.
    {
        reset();
        // Ba thanh ghi cau hinh: ghi mot GIA TRI PHAN BIET cho tung cai, roi doc
        // het CA BA. Gia tri phai khac nhau -- neu ghi cung 5 vao ca ba thi mot
        // giai ma tro sai thanh ghi van tra 5 va phep kiem DAT oan.
        wr32(A_NLAYERS, 37);
        wr32(A_INLEN,   259);
        wr32(A_DMALEN,  91);
        struct { const char *ten; uint32_t addr, want; } reg3[] = {
            {"NLAYERS", A_NLAYERS, 37}, {"INLEN", A_INLEN, 259},
            {"DMALEN", A_DMALEN, 91},
        };
        for (auto &r : reg3) {
            uint32_t got = rd32(r.addr);
            if (got != r.want) fail("T18 doc lai thanh ghi", got, r.want);
            else { char b[64]; std::snprintf(b, sizeof b,
                     "T18 doc lai %s = %u", r.ten, r.want); ok(b); }
        }
        // Bit chon ctrl doc lai duoc (dong :558). Bai bao dung bit nay de noi
        // duong CV-X-IF khong bi tat ngam, nen doc lai duoc no la mot phep kiem
        // ve TUYEN BO, khong chi ve thanh ghi.
        wr32(A_CTRL, 1u << 6);
        uint32_t c = rd32(A_CTRL);
        if (((c >> 6) & 1u) != 1u) fail("T18 doc lai CTRL bit6", (c >> 6) & 1u, 1);
        else ok("T18 doc lai CTRL bit6 = 1");
        wr32(A_CTRL, 0);
        c = rd32(A_CTRL);
        if (((c >> 6) & 1u) != 0u) fail("T18 doc lai CTRL bit6 = 0", (c >> 6) & 1u, 0);
        else ok("T18 doc lai CTRL bit6 = 0");

        // Bang SCALE (:569) va BIAS (:571): ghi roi doc lai. Dung gia tri KHONG
        // PHAI 0 -- mot muc bang bang 0 khong phan biet duoc voi "khong ghi gi".
        wr32(A_SCALE + 4 * 3, 0x1234);
        uint32_t sv = rd32(A_SCALE + 4 * 3);
        if ((sv & 0xFFFFu) != 0x1234u) fail("T18 doc lai SCALE[3]", sv, 0x1234);
        else ok("T18 doc lai bang SCALE[3] = 0x1234");
        wr32(A_BIAS + 4 * 5, 0x0AB);
        uint32_t bv = rd32(A_BIAS + 4 * 5);
        if ((bv & 0x1FFu) != 0x0ABu) fail("T18 doc lai BIAS[5]", bv, 0x0AB);
        else ok("T18 doc lai bang BIAS[5] = 0x0AB");

        // Dia chi thanh ghi LA: GHI (:513) khong duoc dat co loi nao, va DOC
        // (:559) phai tra 0. Ca hai la lua chon thiet ke co y -- mot dia chi la
        // trong vung thanh ghi khong phai loi cua thiet ke nay -- nen phai co
        // mot phep kiem noi ra, khong thi lua chon do khong phan biet duoc voi
        // mot cho bo sot.
        uint32_t e0 = rd32(A_ERR);
        wr32(0x030, 0xDEAD);                 // trong sel_reg ma khong khop case
        uint32_t e1 = rd32(A_ERR);
        if (e1 != e0) fail("T18 ghi dia chi thanh ghi la KHONG duoc dat co", e1, e0);
        else ok("T18 ghi dia chi thanh ghi la: khong co loi nao duoc dat");
        uint32_t rl = rd32(0x030);
        if (rl != 0u) fail("T18 doc dia chi thanh ghi la phai tra 0", rl, 0);
        else ok("T18 doc dia chi thanh ghi la tra 0");

        // Dia chi NGOAI MOI VUNG -- khac han "dia chi la trong vung thanh ghi".
        // Giai ma: sel_reg = 0x000-0x0FF · sel_desc = 0x400-0x7FF ·
        // sel_scale = 0x800-0xBFF · sel_bias = 0x1000-0x1FFF. Nen 0x200 va 0xC00
        // khong thuoc vung nao, va chung di vao nhanh `else` cuoi cung tra 0.
        // Bao phu chi ra dung cho nay: sau khi them bay phep tren, `ecg_mmio.sv`
        // con **mot** diem chua cham, va no la nhanh `else` cua dong 571 -- khong
        // phai mot DONG chua chay ma mot NHANH chua chay cua mot dong DA chay.
        for (uint32_t addr : {0x200u, 0xC00u}) {
            uint32_t e_truoc = rd32(A_ERR);
            uint32_t v = rd32(addr);
            uint32_t e_sau = rd32(A_ERR);
            char b[80];
            if (v != 0u) {
                std::snprintf(b, sizeof b, "T18 doc 0x%03X ngoai moi vung phai tra 0", addr);
                fail(b, v, 0);
            } else if (e_sau != e_truoc) {
                std::snprintf(b, sizeof b, "T18 doc 0x%03X KHONG duoc dat co loi", addr);
                fail(b, e_sau, e_truoc);
            } else {
                std::snprintf(b, sizeof b,
                    "T18 doc 0x%03X ngoai moi vung: tra 0, khong co loi", addr);
                ok(b);
            }
        }
    }

    // ==================================================== T19 · nap GOI (PRE4)
    // Duong nay ton tai de GIAM SO PHEP GHI: 259 mau qua cong PRE la 259 phep
    // ghi x 10,05 chu ky = 2.603,7 chu ky moi nhip, con qua PRE4 la 65 phep ghi.
    // Nen phep kiem manh nhat khong phai "PRE4 co chay khong" ma la TUONG DUONG:
    // N mau qua PRE4 phai cho DUNG day (buf, off, data) nhu N phep ghi PRE don.
    // Mot duong nhanh hon ma cho mot day KHAC thi khong phai mot phep toi uu.
    {
        struct Mau { int buf, off, data; };
        auto rai = [&](int n_ck) {
            std::vector<Mau> ra;
            for (int i = 0; i < n_ck; ++i) {
                tick();
                if (dut->pre_we_o)
                    ra.push_back({(int)dut->pre_buf_o, (int)dut->pre_off_o,
                                  (int)(int8_t)dut->pre_data_o});
            }
            return ra;
        };

        // --- 1. mot phep ghi PRE4 = BON xung, offset lien tiep, byte 0 truoc
        wr32(A_CTRL, 0);                       // bo dong xu ly ranh
        wr32(A_PREADDR, (5u << 13) | 100u);    // buf 5, off 100  (ABITS = 13)
        wr32(A_PRE4, 0x7F81027Fu);             // byte: 7F 02 81 7F  = 127 2 -127 127
        auto g = rai(8);
        const int cho[4] = {0x7F, 0x02, (int8_t)0x81, 0x7F};
        if (g.size() != 4) {
            fail("T19 mot PRE4 phai cho DUNG bon xung nap", (int)g.size(), 4);
        } else {
            bool tot = true;
            for (int i = 0; i < 4; ++i) {
                if (g[i].buf != 5)        { fail("T19 buf", g[i].buf, 5); tot = false; break; }
                if (g[i].off != 100 + i)  { fail("T19 off tu tang", g[i].off, 100 + i); tot = false; break; }
                if (g[i].data != cho[i])  { fail("T19 thu tu byte", g[i].data, cho[i]); tot = false; break; }
            }
            if (tot) ok("T19 mot PRE4 = bon xung, off tu tang, byte 0 la mau dau, co dau dung");
        }

        // --- 2. con tro TU TANG qua nhieu phep ghi PRE4
        wr32(A_PRE4, 0x04030201u);
        auto g2 = rai(8);
        if (g2.size() != 4) fail("T19 PRE4 thu hai: so xung", (int)g2.size(), 4);
        else if (g2[0].off != 104) fail("T19 con tro khong tu tang qua hai PRE4", g2[0].off, 104);
        else if (g2[3].off != 107) fail("T19 off cuoi cua PRE4 thu hai", g2[3].off, 107);
        else ok("T19 con tro tu tang 4 qua tung phep ghi PRE4 (100..103 roi 104..107)");

        // --- 3. TUONG DUONG voi duong PRE don. Day la chot chinh cua nhom nay.
        const uint32_t goi[2] = {0xF1E2D3C4u, 0x1A2B3C4Du};
        wr32(A_PREADDR, (7u << 13) | 200u);
        std::vector<Mau> qua_goi;
        for (int k = 0; k < 2; ++k) {
            wr32(A_PRE4, goi[k]);
            auto t = rai(6);
            qua_goi.insert(qua_goi.end(), t.begin(), t.end());
        }
        std::vector<Mau> qua_don;
        for (int k = 0; k < 2; ++k) {
            for (int i = 0; i < 4; ++i) {
                const uint32_t by = (goi[k] >> (8 * i)) & 0xFF;
                wr32(A_PRE, (7u << 21) | ((200u + 4 * k + i) << 8) | by);
                if (dut->pre_we_o)
                    qua_don.push_back({(int)dut->pre_buf_o, (int)dut->pre_off_o,
                                       (int)(int8_t)dut->pre_data_o});
                tick();
            }
        }
        if (qua_goi.size() != 8 || qua_don.size() != 8) {
            fail("T19 tuong duong: so mau hai duong", (int)qua_goi.size(), 8);
            fail("T19 tuong duong: so mau duong don", (int)qua_don.size(), 8);
        } else {
            int lech = 0;
            for (int i = 0; i < 8; ++i)
                if (qua_goi[i].buf != qua_don[i].buf || qua_goi[i].off != qua_don[i].off
                    || qua_goi[i].data != qua_don[i].data) ++lech;
            if (lech) fail("T19 TUONG DUONG: so mau lech giua hai duong", lech, 0);
            else ok("T19 TUONG DUONG: 8 mau qua PRE4 cho DUNG day (buf,off,data) nhu 8 ghi PRE don");
        }

        // --- 4. TRAN bo rai: ghi PRE4 khi con dang rai -> err bit9, KHONG im lang
        wr32(A_CTRL, 1u << 5);                 // xoa ERRSTAT
        wr32(A_PREADDR, (1u << 13) | 300u);
        wr32(A_PRE4, 0x11223344u);
        tick();                                // bo rai dang chay
        uint32_t st = rd32(A_STATUS);
        if (!(st & (1u << 8))) fail("T19 STATUS bit8 phai bao bo rai dang chay", 0, 1);
        else ok("T19 STATUS bit8 = bo rai dang chay");
        // Bat MOI xung tu day, roi ghi PRE4 thu hai o giua chuoi rai.
        // MOT LOI PHEP DO CUA TOI o ban truoc, ghi vi no la mot mau: toi ky vong
        // con xung SAU phep ghi thu hai, nhung dem chu ky ra thi `rd32(A_STATUS)`
        // ton HAI tick, nen den luc ghi thi bo rai chi con xung cuoi -- va xung
        // do ban CUNG CANH voi phep ghi. Nen "0 xung sau do" la DUNG, khong phai
        // mat du lieu. Phep do dung la dem TONG so xung tren ca cua so.
        std::vector<Mau> tat_ca;
        auto bat = [&](int n) {
            for (int i = 0; i < n; ++i) {
                tick();
                if (dut->pre_we_o)
                    tat_ca.push_back({(int)dut->pre_buf_o, (int)dut->pre_off_o,
                                      (int)(int8_t)dut->pre_data_o});
            }
        };
        // phep ghi PRE4 thu hai roi vao GIUA chuoi rai cua goi thu nhat
        dut->req_i = 1; dut->we_i = 1; dut->be_i = 0xF;
        dut->addr_i = A_PRE4; dut->wdata_i = 0xAABBCCDDu;
        bat(1);            // canh ghi -- va la canh xung cuoi cua goi thu nhat
        bus_idle();
        bat(10);
        uint32_t er = rd32(A_ERR);
        if (!(er & (1u << 9))) fail("T19 TRAN bo rai phai dat err bit9", 0, 1);
        else ok("T19 ghi PRE4 khi dang rai: dat err bit9, khong bo qua IM LANG");
        // Goi THU NHAT phai ra du BON mau; goi thu hai bi tu choi nen KHONG ra
        // mau nao. Tuc tong tren ca cua so (ke ca cac xung da bat truoc do) la 4.
        const int tong = 3 + (int)tat_ca.size();   // 3 xung da di qua rd32/tick
        if (tong != 4)
            fail("T19 goi dang bay phai ra du 4 mau, goi bi tu choi ra 0", tong, 4);
        else ok("T19 tran: goi dang bay ra du 4 mau, goi bi tu choi ra 0 mau");

        // --- 5. PRE4 khi bo dong xu ly BUSY phai bi tu choi (nhu PRE)
        wr32(A_CTRL, 1u << 5);
        dut->cp_busy_i = 1;
        wr32(A_PRE4, 0x55667788u);
        auto g5 = rai(8);
        dut->cp_busy_i = 0;
        er = rd32(A_ERR);
        if (!g5.empty()) fail("T19 PRE4 khi busy phai KHONG nap", (int)g5.size(), 0);
        else if (!(er & (1u << 5))) fail("T19 PRE4 khi busy phai dat err bit5", 0, 1);
        else ok("T19 PRE4 khi bo dong xu ly busy: khong nap, dat err bit5");
    }

    // ---- T20: thanh ghi UART, va mot dia chi KHONG dung ---------------------
    //
    // VI SAO CAN. `make coverage` cho `ecg_mmio` line 40/44 va bon dong chua chay
    // la 597 / 605 / 650 / 654 -- tuc CA BON duong UART cua khoi nay chua he duoc
    // testbench DON VI nay chay. Chung CO duoc kiem o muc HE (`sim-soc` chay
    // loopback 256/256 byte), va do la ly do lo nay khong lo ra som hon: mot phep
    // kiem muc he di qua duong do nen no "hoat dong", nhung mot bit ket hay mot
    // phep giai ma dia chi lech chi lo ra o day, re hon nhieu bac.
    //
    // Dong 605 (`default: ;`) khai mot HANH VI, khong mot cho trong: mot dia chi
    // khong dung phai bi BO QUA va KHONG dat co. Neu ai doi no thanh dat co thi
    // firmware se thay ERRSTAT bat vi mot phep ghi vo hai. Phep kiem cho no phai
    // so ERRSTAT TRUOC va SAU, khong chi doc mot lan.
    {
        reset();
        const uint32_t A_UARTRX = 0x038, A_UARTTX = 0x03C;
        const int f20 = fails;

        // 1. GUI khi bo gui SAN SANG
        dut->u_tx_ready_i = 1;
        wr32(A_UARTTX, 0xA5u);
        dut->eval();
        if (dut->u_tx_data_o != 0xA5) fail("T20 u_tx_data_o", dut->u_tx_data_o, 0xA5);
        if (!dut->u_tx_valid_o) fail("T20 u_tx_valid_o phai len mot chu ky", 0, 1);

        // 2. GUI khi bo gui CHUA san sang -> TRAN GUI, va KHONG duoc phat
        wr32(A_CTRL, 1u << 5);           // xoa ERRSTAT
        dut->u_tx_ready_i = 0;
        wr32(A_UARTTX, 0x3Cu);
        dut->eval();
        if (dut->u_tx_valid_o) fail("T20 ghi khi chua san sang phai KHONG phat", 1, 0);
        uint32_t er20 = rd32(A_ERR);
        if (!(er20 & (1u << 11))) fail("T20 tran gui phai dat err bit11", 0, 1);
        dut->u_tx_ready_i = 1;

        // 3. DOC UART_TX = trang thai san sang cua bo gui
        if (rd32(A_UARTTX) != 1u) fail("T20 doc UART_TX khi san sang", rd32(A_UARTTX), 1);
        dut->u_tx_ready_i = 0;
        if (rd32(A_UARTTX) != 0u) fail("T20 doc UART_TX khi chua san sang", rd32(A_UARTTX), 0);
        dut->u_tx_ready_i = 1;

        // 4. NHAN mot byte, doc la RUT
        wr32(A_CTRL, 1u << 5);
        dut->u_rx_data_i = 0x5Au; dut->u_rx_valid_i = 1;
        tick();
        dut->u_rx_valid_i = 0;
        uint32_t v = rd32(A_UARTRX);
        if ((v & 0xFFu) != 0x5Au) fail("T20 UART_RX byte", v & 0xFF, 0x5A);
        if (!(v & (1u << 8))) fail("T20 UART_RX bit8 (co byte)", 0, 1);
        uint32_t v2 = rd32(A_UARTRX);
        if (v2 & (1u << 8)) fail("T20 doc lan hai: bit8 phai HA (doc la RUT)", 1, 0);

        // 5. TRAN NHAN: hai byte, khong doc giua hai lan
        wr32(A_CTRL, 1u << 5);
        dut->u_rx_data_i = 0x11u; dut->u_rx_valid_i = 1; tick();
        dut->u_rx_data_i = 0x22u;                        tick();
        dut->u_rx_valid_i = 0;
        er20 = rd32(A_ERR);
        if (!(er20 & (1u << 10))) fail("T20 tran nhan phai dat err bit10", 0, 1);
        // va byte DAU phai duoc giu, khong bi de len
        v = rd32(A_UARTRX);
        if ((v & 0xFFu) != 0x11u) fail("T20 tran nhan phai GIU byte dau", v & 0xFF, 0x11);

        // 6. LOI KHUNG dinh o bit9
        wr32(A_CTRL, 1u << 5);
        dut->u_rx_frame_err_i = 1; tick(); dut->u_rx_frame_err_i = 0;
        v = rd32(A_UARTRX);
        if (!(v & (1u << 9))) fail("T20 loi khung phai dat bit9 cua UART_RX", 0, 1);

        // 7. DIA CHI KHONG DUNG: bo qua, va KHONG dat co nao.
        //    PHEP PHAN DINH la so TRUOC/SAU chu khong doc mot lan: ERRSTAT o day
        //    khong the gia dinh la 0 (cac buoc tren da dat bit10/bit11 va CTRL
        //    bit5 chi xoa khi duoc ghi), nen "doc thay 0" khong phan biet duoc
        //    "khong dat co" voi "co von da 0".
        wr32(A_CTRL, 1u << 5);
        const uint32_t truoc = rd32(A_ERR);
        wr32(0x044u, 0xDEADBEEFu);       // trong dai thanh ghi, khong mot ca nao
        wr32(0x0F0u, 0x12345678u);
        const uint32_t sau = rd32(A_ERR);
        if (sau != truoc)
            fail("T20 ghi dia chi khong dung phai KHONG doi ERRSTAT", (long)sau, (long)truoc);

        if (fails == f20)
            ok("T20 UART: gui/nhan, tran ca hai chieu, loi khung, va dia chi la khong dat co");
    }

    // ── T21: QUET TOAN-MOT / TOAN-KHONG tren cac duong DU LIEU ───────────
    //
    // VI SAO NHOM NAY TON TAI. `ecg_mmio` do 68,7 % toggle, va phan dinh
    // (90-results/tables/coverage-phandinh.csv) cho thay **0** diem thuoc loai
    // "khong the cham" -- 442 diem la LO THAT. Sau tin hieu giu gan het:
    //     desc_word_o 116/256 · ws_data_o 28/128 · wf_lo_q 24/64
    //     s_off_i 20/28 · cp_in_len_o 14/20 · addr_i 42/64 (38 la be rong BUS)
    // Ca sau la DUONG DU LIEU DI QUA, va chung chua cham vi cac nhom tren dung
    // vai mau co dinh (`0xA5000000+i`, `0xAAAABBBB`, `0x1000+i`).
    //
    // KHONG can di tung bit. Toggle doi mot bit doi CA HAI chieu, nen mot cap
    // toan-mot / toan-khong tren moi duong la du -- 5 cap thay vi hang nghin
    // phep ghi. Va moi buoc KIEM dau ra that doi: mot phep thu chi LAI ma khong
    // doi chieu thi no nang do phu ma khong noi gi.
    {
        const int f21 = fails;
        // 1. desc_word_o -- 128 bit, doc tu desc_mem[desc_idx_i]
        for (int i = 0; i < 4; ++i) wr32(A_DESC + 7 * 16 + 4 * i, 0xFFFFFFFFu);
        dut->desc_req_i = 1; dut->desc_idx_i = 7; dut->eval();
        for (int i = 0; i < 4; ++i)
            if (desc_word(i) != 0xFFFFFFFFu)
                fail("T21 desc_word_o toan-mot", desc_word(i), 0xFFFFFFFFu);
        for (int i = 0; i < 4; ++i) wr32(A_DESC + 7 * 16 + 4 * i, 0x00000000u);
        dut->desc_req_i = 1; dut->desc_idx_i = 7; dut->eval();
        for (int i = 0; i < 4; ++i)
            if (desc_word(i) != 0u)
                fail("T21 desc_word_o toan-khong", desc_word(i), 0);
        dut->desc_req_i = 0; dut->eval();

        // 2. wf_lo_q (32 bit) va ws_data_o (64 bit) -- nua thap roi nua cao,
        //    roi RUT bang ws_ready_i de con tro doc tien.
        dut->ws_ready_i = 0; dut->eval();
        wr32(A_WFIFO_L, 0xFFFFFFFFu);          // wf_lo_q <- toan mot
        wr32(A_WFIFO_H, 0xFFFFFFFFu);          // day {hi, lo} vao FIFO
        dut->eval();
        if (!dut->ws_valid_o) fail("T21 ws_valid_o sau khi day tu toan-mot", 0, 1);
        else if (dut->ws_data_o != 0xFFFFFFFFFFFFFFFFull)
            fail("T21 ws_data_o toan-mot", (long)dut->ws_data_o, -1);
        dut->ws_ready_i = 1; tick(); dut->ws_ready_i = 0; dut->eval();
        wr32(A_WFIFO_L, 0x00000000u);          // wf_lo_q <- toan khong
        wr32(A_WFIFO_H, 0x00000000u);
        dut->eval();
        if (!dut->ws_valid_o) fail("T21 ws_valid_o sau khi day tu toan-khong", 0, 1);
        else if (dut->ws_data_o != 0ull)
            fail("T21 ws_data_o toan-khong", (long)dut->ws_data_o, 0);
        dut->ws_ready_i = 1; tick(); dut->ws_ready_i = 0; dut->eval();

        // 3. s_off_i -- 14 bit. Toan-mot (16383) VUOT N_SCALE nen no cung chay
        //    nhanh `s_oob`: mot cap gia tri, hai muc dich.
        // `sc_word` la tin hieu NOI BO, khong mot cong -- toi doan sai ten mot
        // lan va bo bien dich bat ngay. Cai phoi ra la `s_mult_o`/`s_shift_o`.
        dut->s_off_i = 0x3FFF; dut->eval();
        const uint32_t m_oob = dut->s_mult_o, sh_oob = dut->s_shift_o;
        dut->s_off_i = 0; dut->eval();
        if (m_oob != 0u || sh_oob != 0u)
            fail("T21 s_off_i ngoai dai phai cho s_mult_o=0 va s_shift_o=0",
                 (long)m_oob, 0);

        // 4. cp_in_len_o -- 10 bit tu in_len_q (thanh ghi 0x10)
        wr32(0x010u, 0x3FFu);
        if (dut->cp_in_len_o != 0x3FF)
            fail("T21 cp_in_len_o toan-mot", dut->cp_in_len_o, 0x3FF);
        wr32(0x010u, 0x000u);
        if (dut->cp_in_len_o != 0)
            fail("T21 cp_in_len_o toan-khong", dut->cp_in_len_o, 0);

        // 5. b_off_i -- cung ho voi s_off_i, va bang phan dinh khong liet no nen
        //    day la mot phep KIEM CHUNG: neu no cung chua cham thi bang thieu
        //    mot dong, va neu da cham thi cap nay vo hai.
        dut->b_off_i = 0x3FFF; dut->eval();
        dut->b_off_i = 0; dut->eval();

        if (fails == f21)
            ok("T21 quet toan-mot/toan-khong: desc_word_o, ws_data_o, wf_lo_q, "
               "s_off_i (ke ca nhanh ngoai dai), cp_in_len_o, b_off_i");
    }

    // ── T22: cung ky thuat, sau tin hieu con lai sau T21 ─────────────────
    //
    // Sau T21 do lai: ecg_mmio 68,7 -> 83,8 %, lo THAT 442 -> 210. Sau tin hieu
    // con giu phan lon, va ca sau lai la duong DU LIEU:
    //     sc_word 14 · u_tx_data_o 12 · dma_len_o 12 · dma_len_q 12
    //     wr_off_i 12 · addr_i 42 (38 la be rong BUS, da phan dinh la CAU TRUC)
    // Nen T22 khong mot ky thuat moi -- no la CUNG mot cap toan-mot/toan-khong
    // ap cho phan con lai. Ghi rieng thay vi noi vao T21 de mot luot chay noi
    // duoc nhom nao dong bao nhieu.
    {
        const int f22 = fails;
        // 1. sc_word (16 bit: nhan 11 + dich 5), qua bang scale
        wr32(A_SCALE + 4 * 3, 0xFFFFu);
        dut->s_off_i = 3; dut->eval();
        if (dut->s_mult_o != 0x7FFu)
            fail("T22 s_mult_o toan-mot (11 bit)", dut->s_mult_o, 0x7FF);
        if (dut->s_shift_o != 0x1Fu)
            fail("T22 s_shift_o toan-mot (5 bit)", dut->s_shift_o, 0x1F);
        wr32(A_SCALE + 4 * 3, 0x0000u);
        dut->s_off_i = 3; dut->eval();
        if (dut->s_mult_o != 0u || dut->s_shift_o != 0u)
            fail("T22 sc_word toan-khong", dut->s_mult_o, 0);

        // 2. u_tx_data_o -- 8 bit, thanh ghi UART_TX
        wr32(0x03Cu, 0xFFu);
        if (dut->u_tx_data_o != 0xFFu)
            fail("T22 u_tx_data_o toan-mot", dut->u_tx_data_o, 0xFF);
        tick(); tick();
        wr32(0x03Cu, 0x00u);
        if (dut->u_tx_data_o != 0u)
            fail("T22 u_tx_data_o toan-khong", dut->u_tx_data_o, 0);

        // 3. dma_len_q / dma_len_o -- WBITS+1 bit, thanh ghi 0x14. Ghi toan-mot
        //    32 bit roi doc lai: phep DOC noi be rong THAT thay vi de toi doan.
        wr32(0x014u, 0xFFFFFFFFu);
        const uint32_t len_max = rd32(0x014u);
        if (len_max == 0u)
            fail("T22 dma_len sau khi ghi toan-mot phai khac 0", 0, 1);
        if (dut->dma_len_o != len_max)
            fail("T22 dma_len_o phai bang gia tri doc lai", dut->dma_len_o,
                 (long)len_max);
        wr32(0x014u, 0x00000000u);
        if (dut->dma_len_o != 0u)
            fail("T22 dma_len_o toan-khong", dut->dma_len_o, 0);

        // 4. wr_off_i -- cong VAO cua duong ket qua. Lai toan-mot roi toan-khong
        //    kem mot phep DAY that, khong thi gia tri chi ngoi o cong.
        dut->wr_i = 0; dut->eval();
        dut->wr_off_i = 0x1FFF; dut->wr_data_i = 0x7F; dut->wr_buf_i = 1;
        dut->wr_last_layer_i = 1; dut->wr_i = 1; tick();
        dut->wr_off_i = 0x0000; dut->wr_data_i = 0x00; dut->wr_buf_i = 0;
        dut->wr_last_layer_i = 0; tick();
        dut->wr_i = 0; dut->eval();

        if (fails == f22)
            ok("T22 quet toan-mot/toan-khong: sc_word, u_tx_data_o, dma_len_q/o, "
               "wr_off_i");
    }

    // ── T23 -- MUC APB-06, ve GHI VAO THANH GHI CHI DOC ──────────────────
    // Ban do khai `0x004 STATUS  R` va `0x02C ERRSTAT  R`. Truoc luot nay khong
    // phep thu nao ghi vao chung, nen chu `R` la mot LOI KHAI chua ai kiem.
    //
    // Phep thu hoi MOT dieu RONG HON "gia tri do co doi khong": mot phep ghi vao
    // dia chi CHI DOC phai la MOT PHEP KHONG LAM GI voi CA TEP THANH GHI.
    // Ly do la mot ca do duoc: khi cho WRITE decode nhan them 0x04 lam CTRL, phep
    // ghi LANH THAT -- no dat `ctrl_sel_q` -- nhung STATUS (0x004) KHONG DOI, vi
    // STATUS ghep tu co phan cung ma mot phep ghi CTRL khong lam dich trong ngu
    // canh nay. Mot phep thu chi nhin dung o vua ghi se bao XANH cho mot phep
    // ghi da lanh o cho khac.
    {
        const uint32_t MAU = 0xFFFFFFFFu;
        // 0x01C bi loai: doc no RUT mot ket qua khoi FIFO, tuc chinh phep doc co
        // tac dung phu va khong dung lam anh chup duoc.
        const uint32_t DOC_DUOC[] = {A_CTRL, A_STATUS, A_LAYER, A_NLAYERS,
                                     0x010u, 0x014u, 0x018u, A_ERR};
        const int ND = (int)(sizeof(DOC_DUOC) / sizeof(DOC_DUOC[0]));
        struct { const char *ten; uint32_t addr; } ro[] = {
            {"STATUS", A_STATUS}, {"ERRSTAT", A_ERR}};
        for (const auto &r : ro) {
            uint32_t truoc[8], sau[8];
            for (int i = 0; i < ND; ++i) truoc[i] = rd32(DOC_DUOC[i]);
            if (rd32(r.addr) == MAU) {
                std::printf("  FAIL  T23 (%s) CHONG RONG: gia tri dang co da bang "
                            "mau ghi 0x%08x\n", r.ten, MAU);
                ++fails; ghi_nhom("T23"); continue;
            }
            wr32(r.addr, MAU);
            for (int i = 0; i < ND; ++i) sau[i] = rd32(DOC_DUOC[i]);
            int lech = 0;
            for (int i = 0; i < ND; ++i) {
                if (sau[i] != truoc[i]) {
                    std::printf("  FAIL  T23 (%s) la R nhung phep GHI DA LANH: "
                                "0x%03x doi 0x%08x -> 0x%08x\n",
                                r.ten, DOC_DUOC[i], truoc[i], sau[i]);
                    ++fails; ++lech;
                }
            }
            if (!lech)
                std::printf("  T23 (%s) chi doc: ghi 0x%08x khong doi mot thanh "
                            "ghi nao trong %d thanh ghi doc duoc\n",
                            r.ten, MAU, ND);
            ghi_nhom("T23");
        }
    }

    // CHOT SAN: so nhom khong duoc TUT. 20 la so nhom DO DUOC luc viet chot nay
    // (T1..T20, ke ca cac bien the T5a/T5b neu co) -- mot lan chay thay it hon
    // nghia la mot nhom da bien mat, va no phai DO chu khong im lang.
    const int NHOM_TOI_THIEU = 23;   // T21,T22 2026-09-04 · T23 (APB-06) 2026-09-06
    if ((int)nhom_thay.size() < NHOM_TOI_THIEU) {
        std::printf("tb_ecg_mmio: FAIL -- chi %zu nhom chay, san la %d. Mot nhom da "
                    "bien mat.\n", nhom_thay.size(), NHOM_TOI_THIEU);
        ECG_COV_WRITE();
        delete dut;
        return 1;
    }
    if (fails == 0)
        std::printf("tb_ecg_mmio: PASS (%zu nhom DO DUOC: thanh ghi, xung start, ba bang"
                    " to hop, hai FIFO, nam chot, doc lai moi thanh ghi va bang, nap GOI"
                    " PRE4, va UART hai chieu)\n", nhom_thay.size());
    else
        std::printf("tb_ecg_mmio: FAIL %d\n", fails);
    ECG_COV_WRITE();
  delete dut;
    return fails ? 1 : 0;
}

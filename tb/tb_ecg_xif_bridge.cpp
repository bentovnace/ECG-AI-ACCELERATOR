// Testbench cho ecg_xif_bridge -- cay cau giua CV-X-IF cua loi va shim ecg_cvxif.
//
//   40-rtl/build/xifbr/tb_xifbr
//
// LUAT HANDSHAKE lay tu dau chu thich cua 40-rtl/src/soc/ecg_xif_bridge.sv, KHONG
// tu mot dac ta CV-X-IF: kho nay khong co dac ta nao, va tep interface cua nha
// cung cap (cv32e40x_if_xif.sv) khong ghi so phien ban. Ba dieu duoi day la thu
// cay cau that su dua vao, nen chung la dac ta hanh dung:
//
//   1. `result_valid` phai GIU cho toi khi `result_ready` -- day la ly do co skid.
//   2. `issue_valid` phai GIU cho toi khi `issue_ready`.
//   3. shim hanh dong o thoi diem ISSUE, nen mot lenh bi huy sau do khong lui lai
//      duoc -- vi vay co `kill_seen_o` thay vi mot co che lui con tro.
//
// PHEP THU TRONG TAM la T2, hoi quy cho mot loi P0 that: dieu 2 duoc dua vao o
// phia loi (`issue_ready` bi chan boi `!skid_full_q`) nhung ban dau KHONG duoc
// dua vao o phia shim (`iss_valid_o` khong bi chan). Khi do, trong luc duong ket
// qua bi chan, loi giu `issue_valid`, cau van day `iss_valid_o = 1`, va shim day
// CUNG MOT LENH vao FIFO moi chu ky. T2 dem so lan bat tay va doi DUNG 1.

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <vector>
#include "Vtb_xif_bridge_wrap.h"
#include "verilated.h"
#include "ecg_cov.h"

static Vtb_xif_bridge_wrap *dut;
static vluint64_t main_time = 0;
double sc_time_stamp() { return main_time; }

static int fails = 0;

static void fail(const char *what, long got, long want) {
    std::printf("  FAIL  %-46s duoc %ld, ky vong %ld\n", what, got, want);
    ++fails;
}
static void ok(const char *what) { std::printf("  ok    %s\n", what); }

// MUC XIF-02: quan sat HAI truong ngoai le tren MOI ket qua tra ve.
//
// DO DUOC TRUOC KHI VIET: `ecg_cvxif` KHONG CO cong loi/ngoai le nao. Toan bo
// cong ra cua no la iss_ready/iss_accept/iss_wb · res_valid/res_rd/res_data ·
// cp_start/cp_single/cp_layer · dma_start/dma_len. Va `err_q` cua `ecg_coproc`
// khong co cong nao dan ra duong XIF. Nen KHONG CO NGUON NGOAI LE tren duong
// nay, va `result.exc = 0` la HANH VI DUNG chu khong phai mot cho quen.
//
// Cai thieu khong phai mot truong -- mot truong chi de mang hang so 0 la trang
// tri. Cai thieu la: dieu do chi dung NHO mot phep gan `'0` toan khoi ma khong
// ai kiem. Neu mai ai do lai duong ket qua va dat `exc` mot cach vo tinh, khong
// gi bat. Nen day la mot phep quan sat, khong phai mot truong moi.
//
// CHONG RONG o cuoi main: neu `n_res_quan_sat == 0` thi phep kiem nay KHONG HE
// CHAY, va mot phep kiem khong chay thi im lang y het mot phep kiem da dat.
static long n_res_quan_sat = 0;

static void quan_sat_ket_qua() {
    if (!dut->result_valid_o) return;
    ++n_res_quan_sat;
    if (dut->result_exc_o)
        fail("XIF-02 result.exc phai 0 (khong co nguon ngoai le)",
             dut->result_exc_o, 0);
    if (dut->result_exccode_o)
        fail("XIF-02 result.exccode phai 0 (khong co nguon ngoai le)",
             dut->result_exccode_o, 0);
}

// Mot chu ky: danh gia o muc thap, len cao, danh gia lai.
static void tick() {
    dut->clk_i = 0; dut->eval(); ++main_time;
    dut->clk_i = 1; dut->eval(); ++main_time;
    quan_sat_ket_qua();
}

static void idle_inputs() {
    dut->issue_valid_i = 0;
    dut->issue_instr_i = 0;
    dut->issue_id_i    = 0;
    dut->issue_rs1_i   = 0;
    dut->issue_rs2_i   = 0;
    dut->commit_valid_i = 0;
    dut->commit_id_i    = 0;
    dut->commit_kill_i  = 0;
    dut->result_ready_i = 0;
    dut->iss_ready_i    = 1;   // shim san sang: `!full`
    dut->iss_accept_i   = 1;   // lenh thuoc khong gian custom-0
    dut->iss_wb_i       = 1;
    dut->res_valid_i    = 0;
    dut->res_rd_i       = 0;
    dut->res_data_i     = 0;
}

static void reset() {
    idle_inputs();
    dut->rst_ni = 0;
    for (int i = 0; i < 4; ++i) tick();
    dut->rst_ni = 1;
    tick();
}

int main(int argc, char **argv) {
    Verilated::commandArgs(argc, argv);
    dut = new Vtb_xif_bridge_wrap;

    // ── T1: mot lenh, khong bi chan -> dung MOT bat tay ──────────────────
    reset();
    dut->issue_valid_i = 1;
    dut->issue_instr_i = 0x0000200bu;   // custom-0, ECG_OP=0
    dut->issue_id_i    = 3;
    int hs = 0;
    for (int c = 0; c < 8; ++c) {
        dut->eval();
        if (dut->iss_valid_o && dut->iss_ready_i) ++hs;
        // Loi ha valid khi thay ready -- dung dieu 2.
        if (dut->issue_ready_o) { tick(); dut->issue_valid_i = 0; }
        else tick();
    }
    if (hs != 1) fail("T1 mot lenh, khong chan: so bat tay", hs, 1);
    else ok("T1 mot lenh, khong chan -> dung 1 bat tay");

    // ── T2: HOI QUY P0 -- duong ket qua bi chan, loi giu valid ───────────
    // Dung dieu 2: loi GIU `issue_valid` cho toi khi `issue_ready`. Trong luc
    // skid con day, cau phai KHONG day lenh xuong shim.
    reset();
    // 1) mot ket qua tu shim lam skid day
    dut->res_valid_i = 1; dut->res_rd_i = 7; dut->res_data_i = 0xdeadbeefu;
    tick();
    dut->res_valid_i = 0;
    dut->eval();
    if (!dut->result_valid_o) fail("T2 skid phai day sau res_valid", 0, 1);

    // 2) loi phat mot lenh va GIU valid; result_ready giu 0 nen skid khong rut
    dut->issue_valid_i = 1;
    dut->issue_instr_i = 0x0000200bu;
    dut->issue_id_i    = 5;
    dut->result_ready_i = 0;
    hs = 0;
    int valid_high = 0;
    for (int c = 0; c < 12; ++c) {
        dut->eval();
        if (dut->iss_valid_o) ++valid_high;
        if (dut->iss_valid_o && dut->iss_ready_i) ++hs;
        if (dut->issue_ready_o) fail("T2 issue_ready phai 0 khi skid day", 1, 0);
        tick();
    }
    if (valid_high != 0) fail("T2 iss_valid_o phai 0 suot khi skid day", valid_high, 0);
    else ok("T2 skid day -> iss_valid_o giu 0, khong day lenh xuong shim");
    if (hs != 0) fail("T2 so bat tay trong luc bi chan", hs, 0);

    // 3) rut skid roi lenh moi duoc di qua, dung MOT lan
    dut->result_ready_i = 1;
    tick();
    dut->result_ready_i = 0;
    hs = 0;
    for (int c = 0; c < 8; ++c) {
        dut->eval();
        if (dut->iss_valid_o && dut->iss_ready_i) ++hs;
        if (dut->issue_ready_o) { tick(); dut->issue_valid_i = 0; }
        else tick();
    }
    if (hs != 1) fail("T2 sau khi rut skid: so bat tay", hs, 1);
    else ok("T2 sau khi rut skid -> dung 1 bat tay");

    // ── T3: id cua ket qua phai la id cua lenh da bat tay ────────────────
    reset();
    dut->issue_valid_i = 1;
    dut->issue_instr_i = 0x0000200bu;
    dut->issue_id_i    = 9;
    while (true) { dut->eval(); if (dut->issue_ready_o) break; tick(); }
    tick();                       // bat tay xay ra o canh nay
    dut->issue_valid_i = 0;
    dut->res_valid_i = 1; dut->res_rd_i = 11; dut->res_data_i = 0x1234u;
    tick();
    dut->res_valid_i = 0;
    dut->eval();
    // Dem RIENG cho T3. Ban dau dung `if (!fails)` -- tuc trang thai bao cao cua
    // T3 phu thuoc vao phep thu KHAC: khi T2 do (tren ban co loi P0), T3 im lang
    // du chinh no dat. Mot phep thu khong duoc lay ket qua cua no tu phep thu khac.
    const int f3 = fails;
    if (!dut->result_valid_o) fail("T3 result_valid sau ket qua", 0, 1);
    if (dut->result_id_o != 9)  fail("T3 result.id", dut->result_id_o, 9);
    if (dut->result_rd_o != 11) fail("T3 result.rd", dut->result_rd_o, 11);
    if (dut->result_data_o != 0x1234u)
        fail("T3 result.data", dut->result_data_o, 0x1234);
    if (fails == f3) ok("T3 id/rd/data cua ket qua khop lenh da bat tay");

    // ── T4: result_valid GIU cho toi khi result_ready (dieu 1) ───────────
    int held = 0;
    for (int c = 0; c < 6; ++c) { dut->eval(); if (dut->result_valid_o) ++held; tick(); }
    if (held != 6) fail("T4 result_valid phai giu khi result_ready=0", held, 6);
    else ok("T4 result_valid giu cho toi khi result_ready");
    dut->result_ready_i = 1; tick(); dut->result_ready_i = 0; dut->eval();
    if (dut->result_valid_o) fail("T4 result_valid phai ha sau khi rut", 1, 0);

    // ── T5: commit_kill dat kill_seen_o CHI khi id khop lenh DA NHAN ─────
    //
    // BAN TRUOC CUA PHEP THU NAY KIEM SAI HOP DONG. No gui mot commit_kill voi
    // id = 2 ma KHONG phat lenh nao truoc, roi doi co bat. Cai dat cu chot MOI
    // phep huy nen no dat -- va do la mot LOI, phat hien bang mo phong cap he:
    //
    //   mot firmware toi thieu KHONG CO MOT lenh custom nao van lam kill_seen_o
    //   bat sau 37 chu ky. Ly do: CV32E40X chao MOI lenh tren giao dien issue va
    //   gui commit cho tung lenh, nen mot phep huy cua mot lenh THUONG (xa
    //   pipeline luc boot, mot nhanh duoc lay) cung lam co bat.
    //
    // Mot co LUON BAT thi khong mang tin. Y nghia da ghi o dau ecg_xif_bridge.sv
    // la "mot lenh DA NHAN bi huy", nen phep thu phai co HAI NUA:
    //   T5a  huy mot lenh DA NHAN (id khop)      -> co PHAI bat
    //   T5b  huy mot id KHONG khop lenh nao      -> co PHAI giu 0
    // Nua thu hai la nua phan dinh, va ban truoc khong co no.
    reset();
    if (dut->kill_seen_o) fail("T5 kill_seen sau reset", 1, 0);

    // T5b truoc: huy mot id chua he duoc phat.
    dut->commit_valid_i = 1; dut->commit_kill_i = 1; dut->commit_id_i = 2;
    tick();
    dut->commit_valid_i = 0; dut->commit_kill_i = 0;
    dut->eval();
    if (dut->kill_seen_o)
        fail("T5b huy mot id CHUA phat khong duoc dat co", 1, 0);
    else ok("T5b huy mot id chua he duoc phat -> co giu 0 (phan dinh)");

    // ── T5c-OK (F11): duong `cmt_ok_o`, phan ma tep nay CHUA TUNG cham ──
    //
    // Do phu sau F11 chi ra dieu do bang mot con so: mot muoi diem toggle moi
    // xuat hien o `ecg_xif_bridge`, SAU duoc phu (`cmt_khop`, `cmt_kill_o`) va
    // BON khong -- `cmt_ok_o` ca hai chieu. Ly do co nghia: kich thich cua tep
    // nay chi lai `commit_kill = 1`, nen no do duong HUY va khong bao gio do
    // duong OK. Phep kiem duoi day KHONG phai de chay theo do phu: no kiem phep
    // PHAN GIAI THEO ID tren duong OK, thu ma cho toi nay chi duong huy co.
    {
        const int ID = 9;
        // (1) commit KHONG kill cho mot id CHUA phat -> khong nhip nao
        dut->commit_valid_i = 1; dut->commit_kill_i = 0; dut->commit_id_i = 3;
        dut->eval();
        if (dut->cmt_ok_o)
            fail("T5c-OK commit mot id CHUA phat lam cmt_ok_o no", 1, 0);
        else ok("T5c-OK commit mot id chua phat -> cmt_ok_o giu 0 (phan dinh)");
        tick();
        dut->commit_valid_i = 0; dut->eval();

        // (2) phat mot lenh, roi commit DUNG id do KHONG kill -> dung mot nhip
        dut->issue_valid_i = 1; dut->issue_instr_i = 0x0000000B; dut->issue_id_i = ID;
        dut->iss_ready_i = 1; dut->iss_accept_i = 1;
        tick();
        dut->issue_valid_i = 0; dut->eval();
        dut->commit_valid_i = 1; dut->commit_kill_i = 0; dut->commit_id_i = ID;
        dut->eval();
        const int ok_no  = dut->cmt_ok_o ? 1 : 0;
        const int kill_no = dut->cmt_kill_o ? 1 : 0;
        if (!ok_no)   fail("T5c-OK commit lenh DA NHAN phai lam cmt_ok_o no", 0, 1);
        else if (kill_no)
            fail("T5c-OK commit KHONG kill ma cmt_kill_o cung no", 1, 0);
        else ok("T5c-OK commit lenh DA NHAN (id khop, khong kill) -> chi cmt_ok_o no");
        tick();
        dut->commit_valid_i = 0; dut->eval();
        // (3) hai nhip LOAI TRU NHAU: khong chu ky nao ca hai cung len
        if (dut->cmt_ok_o || dut->cmt_kill_o)
            fail("T5c-OK nhip phai la MOT chu ky", 1, 0);
        else ok("T5c-OK cmt_ok_o la mot nhip MOT chu ky");
        // `kill_seen_o` KHONG duoc bat: mot lenh duoc commit binh thuong khong
        // phai mot phep huy, va co dinh do la co cua duong HUY.
        if (dut->kill_seen_o)
            fail("T5c-OK commit khong kill ma kill_seen_o bat", 1, 0);
        else ok("T5c-OK commit khong kill -> kill_seen_o giu 0");
        reset();
    }

    // T5a: phat mot lenh (duoc nhan), roi huy DUNG id do.
    {
        const int ID = 7;
        dut->issue_valid_i = 1; dut->issue_instr_i = 0x0000000B; dut->issue_id_i = ID;
        dut->iss_ready_i = 1; dut->iss_accept_i = 1;
        tick();                                   // bat tay phat lenh hoan tat
        dut->issue_valid_i = 0; dut->eval();
        dut->commit_valid_i = 1; dut->commit_kill_i = 1; dut->commit_id_i = ID;
        tick();
        dut->commit_valid_i = 0; dut->commit_kill_i = 0;
        dut->eval();
        if (!dut->kill_seen_o) fail("T5a huy lenh DA NHAN phai dat co", 0, 1);
        else {
            ok("T5a huy mot lenh DA NHAN (id khop) -> co bat");
            for (int c = 0; c < 5; ++c) tick();
            dut->eval();
            if (!dut->kill_seen_o) fail("T5a co phai DINH", 0, 1);
            else ok("T5a co kill_seen_o DINH qua nhieu chu ky");
        }
    }

    // ── T5c/T5d: CO LAP tung dieu kien cua chot ─────────────────────────
    // T5a/T5b bat duoc khi CA HAI dieu kien bi bo, nhung KHONG bat duoc tung cai
    // rieng. Do duoc bang cach pha RTL theo ba chieu:
    //
    //   A  bo so sanh id, giu id_pend_valid_q  -> 0 loi   KHONG bat duoc
    //   B  bo id_pend_valid_q, giu so sanh id  -> 0 loi   KHONG bat duoc
    //   C  bo CA HAI (loi goc)                 -> 1 loi   T5b bat duoc
    //
    // Nen mot trong hai dieu kien co the bi xoa trong mot lan refactor ma khong
    // phep thu nao bao. Hai phep thu duoi day co lap tung cai:
    //
    //   T5c  co lenh dang cho, huy mot id KHAC  -> bat duoc chieu A
    //   T5d  khong co lenh nao, huy id = 0      -> bat duoc chieu B
    //        (0 la gia tri reset cua id_pend_q, nen chi `id_pend_valid_q` chan)

    // T5c: phat mot lenh id=7 (nen id_pend_valid_q = 1), roi huy id=3.
    reset();
    dut->issue_valid_i = 1;
    dut->issue_instr_i = 0x0000200bu;
    dut->issue_id_i    = 7;
    for (int c = 0; c < 8 && !dut->issue_ready_o; ++c) { dut->eval(); tick(); }
    dut->eval();
    tick();                       // bat tay -> id_pend_q = 7, id_pend_valid_q = 1
    dut->issue_valid_i = 0;
    dut->commit_valid_i = 1; dut->commit_kill_i = 1; dut->commit_id_i = 3;
    tick();
    dut->commit_valid_i = 0; dut->commit_kill_i = 0;
    dut->eval();
    if (dut->kill_seen_o)
        fail("T5c co lenh dang cho, huy id KHAC -> co phai giu 0", 1, 0);
    else ok("T5c huy mot id KHAC id dang cho -> co giu 0");

    // T5d: khong phat lenh nao, huy id = 0 = gia tri reset cua id_pend_q.
    reset();
    dut->commit_valid_i = 1; dut->commit_kill_i = 1; dut->commit_id_i = 0;
    tick();
    dut->commit_valid_i = 0; dut->commit_kill_i = 0;
    dut->eval();
    if (dut->kill_seen_o)
        fail("T5d khong co lenh nao, huy id=0 -> co phai giu 0", 1, 0);
    else ok("T5d huy id=0 khi khong co lenh nao -> co giu 0");

    // ── T6: commit KHONG kill thi khong dat co ───────────────────────────
    reset();
    dut->commit_valid_i = 1; dut->commit_kill_i = 0; dut->commit_id_i = 4;
    tick();
    dut->commit_valid_i = 0; dut->eval();
    if (dut->kill_seen_o) fail("T6 commit khong kill khong duoc dat co", 1, 0);
    else ok("T6 commit khong kill -> co giu 0");


    // -- T7: duong du lieu issue -> shim, chong BAT CHEO -------------------
    //
    // VI SAO CAN: `iss_rs1_o`/`iss_rs2_o`/`iss_instr_o` la DAY tran (dong
    // 112-114 cua ecg_xif_bridge.sv, ba `assign` khong dieu kien). Truoc phep
    // thu nay, testbench de `issue_rs1_i = issue_rs2_i = 0` suot ca lan chay
    // (dong 51-52) va `issue_instr_i` chi nhan hai gia tri, nen do phu toggle
    // do duoc cua module la 25,6 %: 128/128 bit cua iss_rs1_o va 128/128 cua
    // iss_rs2_o CHUA HE lat. Mot dieu do nghia la: neu hai dong 113 va 114 bi
    // doi cho cho nhau thi khong phep thu nao trong tep bao.
    //
    // DIEU LAM CHO PHEP THU MANG TIN: rs1 va rs2 phai mang gia tri KHAC NHAU
    // TAI CUNG MOT THOI DIEM. Neu cap cung mot mau cho ca hai (du la mau di
    // dong day du 32 bit) thi toggle len 100 % ma mot day bat cheo VAN vo hinh
    // -- do phu se xanh trong khi phep thu khong phan dinh duoc gi. Vi vay
    // rs1 = pat, rs2 = ~pat, instr = pat ^ 0xA5A5A5A5: ba gia tri doi mot khac
    // nhau o MOI buoc, nen moi phep doi cho trong ba day deu bi bat.
    //
    // Mau `pat = 1 << i` chay i = 0..31 lat MOI bit theo CA HAI chieu: bit k
    // cua rs1 di 0 -> 1 (tai i = k) -> 0, con cua rs2 di 1 -> 0 -> 1.
    //
    // instr duoc cap CA 32 bit ke ca truong opcode: hop le, vi `instr` chi xuat
    // hien DUNG MOT LAN trong ecg_xif_bridge.sv (dong 112) -- cay cau khong he
    // giai ma no, nen khong co gia tri nao la "khong hop le" doi voi phep noi day.
    reset();
    dut->issue_valid_i  = 1;
    dut->issue_id_i     = 5;
    dut->iss_ready_i    = 1;
    dut->res_valid_i    = 0;
    dut->result_ready_i = 1;
    int n_kiem_t7 = 0;
    int sinh_bang_nhau = 0;      // chot len CHINH bo sinh kich thich
    for (int i = 0; i < 32; ++i) {
        const uint32_t pat = 1u << i;
        const uint32_t v1  = pat;
        const uint32_t v2  = ~pat;
        const uint32_t vi  = pat ^ 0xA5A5A5A5u;
        if (v1 == v2 || v1 == vi || v2 == vi) ++sinh_bang_nhau;
        dut->issue_rs1_i   = v1;
        dut->issue_rs2_i   = v2;
        dut->issue_instr_i = vi;
        dut->eval();
        if ((uint32_t)dut->iss_rs1_o != v1)
            fail("T7 iss_rs1_o khong bang issue_rs1_i", (long)(uint32_t)dut->iss_rs1_o, (long)v1);
        if ((uint32_t)dut->iss_rs2_o != v2)
            fail("T7 iss_rs2_o khong bang issue_rs2_i", (long)(uint32_t)dut->iss_rs2_o, (long)v2);
        if ((uint32_t)dut->iss_instr_o != vi)
            fail("T7 iss_instr_o khong bang issue_instr_i", (long)(uint32_t)dut->iss_instr_o, (long)vi);
        ++n_kiem_t7;
        tick();
    }
    dut->issue_valid_i = 0;
    idle_inputs();
    if (n_kiem_t7 != 32)
        fail("T7 so buoc kich thich", n_kiem_t7, 32);
    else if (sinh_bang_nhau != 0)
        // Neu ba gia tri trung nhau o mot buoc nao thi buoc do KHONG phan dinh
        // duoc bat cheo, va mot phep thu khong phan dinh thi khong mang tin.
        fail("T7 bo sinh cho hai gia tri TRUNG nhau (mat tinh phan dinh)", sinh_bang_nhau, 0);
    else ok("T7 rs1/rs2/instr qua cau bit-chinh-xac, 32 buoc, ba gia tri khac nhau moi buoc");


    // -- T8: duong KET QUA shim -> loi, chong bat cheo va bit ket ----------
    //
    // T7 lo duong DI. Duong VE con mot lo cung lop: T3 (dong 155) co kiem
    // `result_data_o` nhung chi voi HAI gia tri, 0xdeadbeef va 0x1234. Hop cua
    // hai gia tri do la 0xDEADBEFF, tuc bay bit -- 29, 24, 22, 20, 17, 14, 8 --
    // KHONG BAO GIO len 1. Do phu do duoc khop dung so hoc do: `res_data_i` con
    // 28/128 bit chua lat = 7 bit x 2 chieu x 2 phan cap. Mot bit bi ket 0 o mot
    // trong bay vi tri do se qua duoc ca T3.
    //
    // PHA LAY MAU: duong ve co THANH GHI, khong nhu T7. `rd_q`/`data_q` duoc
    // chot o dong 161-163 khi `res_valid_i`, roi `result.data = data_q` (dong
    // 196). Nen phai TICK truoc khi doc `result_data_o`; doc truoc canh len se
    // thay gia tri cua nhip TRUOC va phep kiem thanh rong -- day dung la loi
    // vacuous da mac ba lan trong kho nay.
    //
    // BA DAI LUONG PHAI KHAC NHAU cung luc, cung ly do nhu T7: data = 1 << i,
    // rd = ~i & 0x1F, id = i & 0xF. rd va id KHONG BAO GIO bang nhau, va do la
    // mot dieu chung minh duoc chu khong may man: rd <= 15 doi hoi bit4 cua ~i
    // bang 0 tuc i >= 16, va khi do rd = ~(i&0xF) & 0xF con id = i & 0xF -- mot
    // gia tri 4 bit khong the bang bu cua chinh no. Phep thu chot lai dieu do.
    reset();
    dut->result_ready_i = 0;
    int n_kiem_t8 = 0;
    int t8_trung  = 0;
    for (int i = 0; i < 32; ++i) {
        const uint32_t d  = 1u << i;
        const uint32_t rd = (~(uint32_t)i) & 0x1Fu;
        const uint32_t id = (uint32_t)i & 0xFu;
        if (rd == id) ++t8_trung;

        // phat mot lenh de nap id_pend_q
        dut->issue_valid_i = 1;
        dut->issue_instr_i = 0x0000200bu;
        dut->issue_id_i    = id;
        int canh = 0;
        while (canh < 8) { dut->eval(); if (dut->issue_ready_o) break; tick(); ++canh; }
        tick();                       // bat tay -> id_pend_q = id
        dut->issue_valid_i = 0;

        // shim tra ket qua; canh nay chot rd_q/data_q/skid_full_q
        dut->res_valid_i = 1; dut->res_rd_i = rd; dut->res_data_i = d;
        tick();
        dut->res_valid_i = 0;
        dut->eval();                  // GIO moi doc duoc

        if (!dut->result_valid_o)
            fail("T8 result_valid sau khi shim tra ket qua", 0, 1);
        if ((uint32_t)dut->result_data_o != d)
            fail("T8 result_data_o khong bang res_data_i", (long)(uint32_t)dut->result_data_o, (long)d);
        if ((uint32_t)dut->result_rd_o != rd)
            fail("T8 result_rd_o khong bang res_rd_i", (long)(uint32_t)dut->result_rd_o, (long)rd);
        if ((uint32_t)dut->result_id_o != id)
            fail("T8 result_id_o khong bang issue_id_i", (long)(uint32_t)dut->result_id_o, (long)id);
        ++n_kiem_t8;

        // rut skid de vong sau phat duoc lenh (skid_full_q chan issue_ready)
        dut->result_ready_i = 1; tick(); dut->result_ready_i = 0; dut->eval();
        if (dut->result_valid_o)
            fail("T8 result_valid phai ha sau khi rut skid", 1, 0);
    }
    idle_inputs();
    if (n_kiem_t8 != 32)
        fail("T8 so buoc kich thich", n_kiem_t8, 32);
    else if (t8_trung != 0)
        fail("T8 rd va id TRUNG nhau (mat tinh phan dinh)", t8_trung, 0);
    else ok("T8 data/rd/id duong ve bit-chinh-xac, 32 buoc, ba dai luong khac nhau");

    // ── T9 (W1-A / F01+F02): HAI lenh lien tiep, quet khoang cach 0..3 ───
    //
    // F01: `id_pend_q` duoc ghi luc BAT TAY, con `skid_full_q` chi len MOT chu
    // ky sau. Trong dung chu ky do, dieu kien nhan (`!skid_full_q`) van cho
    // lenh 2 di qua, va no GHI DE `id_pend_q` -- ket qua cua lenh 1 tra ve voi
    // id cua lenh 2. `id_pend_valid_q` DA ton tai va DA duoc set, no chi khong
    // nam trong dieu kien nhan.
    //
    // F02: neu mot ket qua thu hai toi trong luc skid con day va chua duoc rut
    // thi `rd_q`/`data_q` bi ghi de -- payload doi trong luc `result_valid` con
    // len.
    //
    // Phep do: phat id 1 roi id 2 cach nhau 0/1/2/3 chu ky, GIU `result_ready`
    // xuong mot so chu ky, va ghi lai DAY id THAT SU tra ve. Ba dai luong khac
    // nhau (khong dem xung ket qua): so lenh NHAN, so ket qua TRA, va day id.
    {
        int t9_loi = 0;
        std::printf("  T9: hai lenh lien tiep -- quet khoang cach, giu result_ready\n");
        std::printf("      %-9s %-8s %-8s %-14s %-10s\n", "khoang", "nhan",
                    "tra", "day id", "payload");
        for (int kc = 0; kc <= 3; ++kc) {
            reset();
            int nhan = 0, tra = 0, doi_payload = 0;
            int day[4] = {0, 0, 0, 0};
            int cho_res = -1;            // dem nguoc toi luc shim tra ket qua
            uint32_t last_data = 0;
            int last_valid = 0;
            const int GIU = 3 + kc;      // so chu ky giu result_ready = 0

            for (int c = 0; c < 40; ++c) {
                // phat lenh: id 1 o chu ky 0, id 2 o chu ky kc+1
                if (c == 0)        { dut->issue_valid_i = 1; dut->issue_id_i = 1; }
                else if (c == kc + 1 && nhan >= 1) { dut->issue_valid_i = 1; dut->issue_id_i = 2; }
                dut->issue_instr_i = 0x0000200bu;
                dut->iss_ready_i = 1; dut->iss_accept_i = 1; dut->iss_wb_i = 1;
                // shim tra ket qua DUNG MOT chu ky sau bat tay
                dut->res_valid_i = (cho_res == 0) ? 1 : 0;
                dut->res_rd_i    = 5;
                dut->res_data_i  = 0xA0000000u + uint32_t(nhan);
                dut->result_ready_i = (c >= GIU) ? 1 : 0;
                dut->eval();

                // F02: payload phai ON DINH trong luc valid & !ready
                if (last_valid && dut->result_valid_o && !dut->result_ready_i
                    && dut->result_data_o != last_data) ++doi_payload;
                last_valid = dut->result_valid_o ? 1 : 0;
                last_data  = dut->result_data_o;

                // THU TU: lay mau -> TICK -> moi doi tin hieu lai. Ha `valid`
                // TRUOC `tick()` thi canh dong ho khong thay bat tay va
                // `id_pend_q` khong he chot -- ban dau toi lam the va bang bao
                // "nhan=2" trong khi RTL chua nhan gi, id tra ve toan 0.
                const int bt  = (dut->issue_valid_i && dut->issue_ready_o) ? 1 : 0;
                const int rut = (dut->result_valid_o && dut->result_ready_i) ? 1 : 0;
                const int id_ra = int(dut->result_id_o);
                if (cho_res >= 0) --cho_res;
                tick();
                if (bt) {
                    ++nhan; dut->issue_valid_i = 0;
                    if (cho_res < 0) cho_res = 1;
                }
                if (rut) {
                    if (tra < 4) day[tra] = id_ra;
                    ++tra;
                    if (nhan > tra && cho_res < 0) cho_res = 1;
                }
            }
            idle_inputs();
            std::printf("      %-9d %-8d %-8d %d,%d,%d,%-8s %-10s\n", kc, nhan, tra,
                        day[0], day[1], day[2],
                        tra > 3 ? "..." : "", doi_payload ? "DOI" : "on dinh");
            // CHOT 1 (F01): moi lenh NHAN phai co dung mot ket qua TRA
            if (nhan != tra) {
                std::printf("      CHOT F01: khoang=%d nhan=%d ma tra=%d -- %d "
                            "ket qua bi MAT\n", kc, nhan, tra, nhan - tra);
                ++t9_loi;
            }
            // CHOT 2 (F01): day id phai la 1 roi 2, khong 2 roi 2
            if (tra >= 2 && !(day[0] == 1 && day[1] == 2)) {
                std::printf("      CHOT F01: khoang=%d day id tra ve la %d,%d "
                            "-- ky vong 1,2 (id bi GHI DE)\n", kc, day[0], day[1]);
                ++t9_loi;
            }
            if (tra >= 1 && day[0] != 1) {
                std::printf("      CHOT F01: khoang=%d ket qua DAU tra id %d, "
                            "ky vong 1\n", kc, day[0]);
                ++t9_loi;
            }
            // CHOT 3 (F02): payload khong duoc doi trong luc valid & !ready
            if (doi_payload) {
                std::printf("      CHOT F02: khoang=%d payload DOI %d lan trong "
                            "luc result_valid & !result_ready\n", kc, doi_payload);
                ++t9_loi;
            }
            // CHOT 4 chong-rong: phai nhan duoc it nhat mot lenh, khong thi phep
            // do khong cham vao duong issue va mot bang toan 0 doc nhu "an toan"
            if (nhan < 1) {
                std::printf("      CHOT: khoang=%d khong nhan duoc lenh nao -- "
                            "phep do rong\n", kc);
                ++t9_loi;
            }
        }
        if (t9_loi) fails += t9_loi;
        else ok("T9 hai lenh lien tiep: nhan == tra, day id 1 roi 2, payload on dinh");
    }

    // ── T10 (MUC XIF-06): ba canh muc doi ma T1..T9 khong cham ────────────
    //
    // Muc doi BON canh: ID wrap · rd = x0 · nhieu lenh lien tiep · xen
    // compute/control. Canh thu ba DA co (T9). Ba canh con lai o day.
    //
    // VI SAO `rd = 0` LA MOT CANH RIENG. x0 cua RISC-V la thanh ghi treo cung
    // khong. Mot cau noi "toi uu" co the coi `rd == 0` la "khong writeback" va
    // bo ket qua di -- luc do ket qua KHONG bao gio ve va loi treo cho no. Cau
    // nay KHONG duoc phep dien giai `rd`; no chi chuyen tiep. Phep thu ghim dieu do.
    //
    // VI SAO `id` WRAP LA MOT CANH RIENG. `X_ID_WIDTH = 4` nen id chay 0..15 va
    // 15 -> 0 la mot buoc LUI ve mat so hoc. Bat ky phep so sanh nao dung `>` hay
    // `<` tren id (thay vi `==`) se dut o dung buoc do.
    //
    // XEN COMPUTE/CONTROL o muc CAU noi la: hai tu lenh KHAC NHAU di lien tiep,
    // va ket qua cua tung cai phai mang dung id/rd/data cua no -- cau khong giai
    // ma lenh, nen phep thu hoi "co lan du lieu giua hai loai giao dich khong".
    {
        struct Ca { const char *ten; int id; int rd; uint32_t instr; uint32_t data; };
        // fn3 = 0 -> COMPUTE, fn3 = 1 -> CTRL (bit 14:12 cua tu lenh).
        const uint32_t I_COMPUTE = 0x0000000Bu;
        const uint32_t I_CTRL    = 0x0000100Bu;
        const Ca ca[] = {
            {"id 15 (truoc wrap), rd=7",  15, 7, I_COMPUTE, 0xC0FFEE01u},
            {"id 0  (SAU wrap),   rd=7",   0, 7, I_COMPUTE, 0xC0FFEE02u},
            {"rd = x0 tren COMPUTE",       3, 0, I_COMPUTE, 0xC0FFEE03u},
            {"CTRL xen ngay sau COMPUTE",  4, 9, I_CTRL,    0xC0FFEE04u},
            {"COMPUTE xen ngay sau CTRL",  5, 0, I_COMPUTE, 0xC0FFEE05u},
            {"CTRL voi rd = x0",           6, 0, I_CTRL,    0xC0FFEE06u},
        };
        const int n_ca = int(sizeof(ca) / sizeof(ca[0]));
        int t10_loi = 0, t10_chay = 0;
        std::printf("  T10 (XIF-06): id wrap, rd=x0, xen compute/control\n");
        for (int i = 0; i < n_ca; ++i) {
            ++t10_chay;
            reset();
            dut->issue_valid_i = 1;
            dut->issue_instr_i = ca[i].instr;
            dut->issue_id_i    = ca[i].id;
            dut->iss_ready_i = 1; dut->iss_accept_i = 1; dut->iss_wb_i = 1;
            while (true) { dut->eval(); if (dut->issue_ready_o) break; tick(); }
            // Tu lenh phai di qua cau BIT-CHINH-XAC ngay o chu ky bat tay.
            dut->eval();
            if (dut->iss_instr_o != ca[i].instr) {
                fail("T10 iss_instr_o khong bang issue_instr_i",
                     long(dut->iss_instr_o), long(ca[i].instr));
                ++t10_loi;
            }
            tick();
            dut->issue_valid_i = 0;
            dut->res_valid_i = 1;
            dut->res_rd_i    = ca[i].rd;
            dut->res_data_i  = ca[i].data;
            tick();
            dut->res_valid_i = 0;
            dut->result_ready_i = 0;
            dut->eval();
            // Ket qua phai LEN, va mang dung ba dai luong -- ke ca khi rd = 0.
            if (!dut->result_valid_o) {
                std::printf("  FAIL  T10 %-30s result_valid KHONG len\n", ca[i].ten);
                ++fails; ++t10_loi;
            } else {
                if (int(dut->result_id_o) != ca[i].id) {
                    std::printf("  FAIL  T10 %-30s id duoc %d, ky vong %d\n",
                                ca[i].ten, int(dut->result_id_o), ca[i].id);
                    ++fails; ++t10_loi;
                }
                if (int(dut->result_rd_o) != ca[i].rd) {
                    std::printf("  FAIL  T10 %-30s rd duoc %d, ky vong %d\n",
                                ca[i].ten, int(dut->result_rd_o), ca[i].rd);
                    ++fails; ++t10_loi;
                }
                if (dut->result_data_o != ca[i].data) {
                    std::printf("  FAIL  T10 %-30s data duoc %08x, ky vong %08x\n",
                                ca[i].ten, dut->result_data_o, ca[i].data);
                    ++fails; ++t10_loi;
                }
            }
        }
        // CHOT CHONG RONG: mot vong lap khong chay se in "0 loi" va doc len nhu dat.
        if (t10_chay != n_ca) {
            std::printf("  FAIL  T10 chi chay %d/%d ca\n", t10_chay, n_ca);
            ++fails;
        } else if (t10_loi == 0) {
            std::printf("  ok    T10 %d/%d ca: id wrap 15->0, rd=x0, xen COMPUTE/CTRL\n",
                        t10_chay, n_ca);
        }
    }

    // ── T11 (MUC XIF-08): BACKPRESSURE NGAU NHIEN DAI ─────────────────────
    // T2 va T4 chan bang mot khoi DO DAI CO DINH do tay dat, nen chung do duoc
    // "co giu duoc khong" chu khong do duoc "giu duoc BAO LAU va qua bao nhieu
    // hinh thai chan". Muc doi chan NGAU NHIEN DAI.
    //
    // Bo sinh TAT DINH va hat giong duoc IN RA: mot lan do that bai phai do LAI
    // duoc. `ECG_XIF08_SEED` doi hat giong de quet nhieu hinh thai qua nhieu lan
    // chay ma khong lam phep thu thanh bap benh trong CI.
    //
    // Bat bien: moi vong phat DUNG MOT lenh, va no phai duoc NHAN o bien tren
    // DUNG MOT lan va DAY XUONG shim DUNG MOT lan -- khong mat, khong nhan doi,
    // du bi chan bao lau.
    {
        reset();
        uint32_t seed = 0xC0FFEEu;
        if (const char *e = std::getenv("ECG_XIF08_SEED"))
            seed = (uint32_t)std::strtoul(e, nullptr, 0);
        uint32_t st = seed ? seed : 1u;
        auto rnd = [&]() {
            st ^= st << 13; st ^= st >> 17; st ^= st << 5; return st;
        };

        const int N_VONG = 40;
        int tong_nhan = 0, tong_day = 0, tong_chan = 0, chan_dai_nhat = 0;
        int vong_hong = 0;
        for (int v = 0; v < N_VONG; ++v) {
            // Lam skid DAY: mot ket qua ve trong khi `result_ready_i` = 0.
            dut->result_ready_i = 0;
            dut->res_valid_i = 1;
            dut->res_rd_i = (v % 15) + 1;
            dut->res_data_i = 0x1000u + (uint32_t)v;
            tick();
            dut->res_valid_i = 0;

            const int dai = (int)(rnd() % 41);       // 0..40 chu ky bi chan
            if (dai > chan_dai_nhat) chan_dai_nhat = dai;

            // Loi phat MOT lenh va GIU valid cho toi khi thay ready (dieu 2).
            dut->issue_valid_i = 1;
            dut->issue_instr_i = 0x0000200bu;
            dut->issue_id_i    = (uint8_t)(v % 16);
            int nhan = 0, day = 0;

            for (int c = 0; c < dai; ++c) {
                dut->eval();
                if (dut->issue_valid_i && dut->issue_ready_o) {
                    ++nhan; dut->issue_valid_i = 0;
                }
                if (dut->iss_valid_o && dut->iss_ready_i) ++day;
                ++tong_chan;
                tick();
            }
            // Tha ra: skid rut, lenh phai di qua.
            dut->result_ready_i = 1;
            for (int c = 0; c < 10; ++c) {
                dut->eval();
                if (dut->issue_valid_i && dut->issue_ready_o) {
                    ++nhan; dut->issue_valid_i = 0;
                }
                if (dut->iss_valid_o && dut->iss_ready_i) ++day;
                tick();
            }
            dut->issue_valid_i = 0;
            dut->result_ready_i = 0;
            tong_nhan += nhan; tong_day += day;
            if (nhan != 1 || day != 1) {
                std::printf("  FAIL  T11 vong %d (chan %d chu ky): nhan=%d day=%d, "
                            "moi cai phai la 1\n", v, dai, nhan, day);
                ++vong_hong;
            }
        }
        std::printf("  T11 hat giong 0x%08x · %d vong · chan tong %d chu ky · "
                    "chan dai nhat %d · nhan %d · day xuong %d\n",
                    seed, N_VONG, tong_chan, chan_dai_nhat, tong_nhan, tong_day);
        if (vong_hong) fail("T11 so vong hong", vong_hong, 0);

        // CHONG RONG 1: phai co chu ky bi chan THAT. Neu bo sinh cho toan 0 thi
        // bang tren doc nhu mot phep thu backpressure trong khi khong ai bi chan.
        if (tong_chan <= 0)
            fail("T11 CHONG RONG: tong so chu ky bi chan", tong_chan, 1);
        // CHONG RONG 2: muc doi chan DAI. Mot loat chan 1-2 chu ky van cho
        // `tong_chan > 0` nhung khong tra loi duoc cau muc hoi.
        else if (chan_dai_nhat < 10)
            fail("T11 CHONG RONG: doan chan DAI NHAT (muc doi chan dai)",
                 chan_dai_nhat, 10);
        else if (!vong_hong)
            ok("T11 backpressure ngau nhien dai: khong lenh nao mat hay nhan doi");
    }

    dut->final();
    ECG_COV_WRITE();
  delete dut;
    // MUC XIF-02 CHONG RONG: mot phep quan sat 0 lan thi im lang y het mot
    // phep quan sat da dat. Neu duong ket qua doi hinh va `result_valid_o`
    // khong con len, cho nay phai DO chu khong duoc bao PASS.
    if (n_res_quan_sat == 0)
        fail("XIF-02 chong rong: khong quan sat duoc mot ket qua nao",
             n_res_quan_sat, 1);
    else
        std::printf("  ok    XIF-02 exc/exccode = 0 tren %ld ket qua quan sat duoc\n",
                    n_res_quan_sat);

    if (fails) { std::printf("tb_ecg_xif_bridge: FAIL (%d loi)\n", fails); return 1; }
    std::printf("tb_ecg_xif_bridge: PASS (bat tay, chan doi xung, id, giu ket qua, kill)\n");
    return 0;
}

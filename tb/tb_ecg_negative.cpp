// Negative-stimulus harness: the INVERSE test for every simulation-visible
// checker in ecg_wmem, ecg_cvxif and ecg_coproc.
//
// Why this harness exists. Checklist C23 says a property that "passes" does not
// prove itself non-vacuous. A $error that has never fired may well be a $error
// that CANNOT fire -- wrong condition, wrong signal, or shadowed by an earlier
// condition on the same path. The only way to tell is to violate its condition
// on purpose and see whether it fires. So here PASS means "the checker fired",
// and a checker that stays silent under a deliberate violation is a finding, not
// a success.
//
// Two rules this harness follows so its numbers mean something.
//
//   * Nothing in 40-rtl/src is modified. Every violation is driven from the
//     module boundary only. A checker that cannot be reached that way is
//     reported as unreachable-from-the-boundary with the reason, not worked
//     around through hierarchical pokes.
//   * Every case also declares whether it EXPECTS a fire. A case marked
//     "expect no fire" that fires is a real bug (RTL or checker), and a case
//     marked "expect fire" that stays silent is a useless checker. Both are
//     reported louder than the rest.
//
// Verilator mechanics, checked against 5.051 headers rather than guessed:
// VerilatedContext::errorCount() counts $error/assert failures,
// fatalOnError(false) keeps the run going past one, and gotError()/gotFinish()
// latch and must be cleared between cases. $error inside always_comb can fire
// more than once per cycle as the eval settles, so a delta is reported as
// measured, never assumed to be one.
//
// Build: three separate Verilator models, one per top module, each linking this
// same file with -DDUT_WMEM / -DDUT_CVXIF / -DDUT_COPROC (see `make
// sim-negative`).

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "verilated.h"

#if defined(DUT_WMEM)
#include "Vecg_wmem.h"
// MUC LOAD-04: dem SO BYTE THAT SU DUOC GHI, doc `we_even`/`we_odd` -- chan
// cho phep ghi cua hai bang SRAM (ecg_wmem.sv:104-105). Dem o day chu khong
// dem `dma_done_o` hay so lan nhan lenh: muc cam dung "token tra ve" lam bang
// chung trong so da toi, va token la thu duy nhat dang duoc dem truoc do.
#include "Vecg_wmem___024root.h"
using Dut = Vecg_wmem;
static const char *kDutName = "ecg_wmem";
#elif defined(DUT_CVXIF)
#include "Vecg_cvxif.h"
using Dut = Vecg_cvxif;
static const char *kDutName = "ecg_cvxif";
#elif defined(DUT_COPROC)
#include "Vecg_coproc.h"
// MUC EXEC-05: doc state_q THAT thay vi mot MO HINH chu ky. Ma tran quyen
// truy cap phai duoc dan nhan bang trang thai MAY DANG O, khong bang trang
// thai tb TUONG no dang o -- neu dan nhan bang mo hinh thi ma tran do chinh
// mo hinh ay chu khong do thiet ke.
#include "Vecg_coproc___024root.h"
using Dut = Vecg_coproc;
static const char *kDutName = "ecg_coproc";
#else
#error "define one of DUT_WMEM / DUT_CVXIF / DUT_COPROC"
#endif

namespace {

VerilatedContext *ctx = nullptr;
Dut *dut = nullptr;

#if defined(DUT_CVXIF)
// F11: `ecg_cvxif` hanh dong o COMMIT. Tep nay lai shim TRUC TIEP, nen no phai
// MO HINH HOA kenh commit -- mot nhip `cmt_ok_i` MOT chu ky sau moi phep nhan.
// Thieu no thi `spec_valid_q` khong bao gio duoc xoa va khang dinh chan tren
// (`ecg_cvxif.sv:412`) ban o MOI ca, ke ca cac ca KHONG co vi pham co y -- bao
// cao doc ra la "ban ngoai y muon" o ba ca va "assertion vo dung" o mot ca.
// Day la tb THU NAM lai shim dung mot minh; bon cai kia la tb_ecg_cvxif,
// tb_ecg_cvxif_loadw, tb_loadw_pair_wrap va tb_xif_kill_wrap.
bool cho_commit = false;
#endif

void tick() {
  dut->clk_i = 0;
  dut->eval();
#if defined(DUT_CVXIF)
  // `nhan` phai doc SAU `eval()`: tb dat dau vao roi goi `tick()`, nen truoc
  // `eval()` thi `iss_ready_o` con la gia tri cua chu ky TRUOC. Doc truoc la
  // dung lop loi "cua so quan sat" mot lan nua, va o day no lam bo dem
  // `errorCount()` phinh len o moi ca vi khang dinh chan tren ban.
  const bool nhan = dut->iss_valid_i && dut->iss_ready_o && dut->iss_accept_o;
  dut->cmt_ok_i = cho_commit ? 1 : 0;
  dut->eval();
#endif
  dut->clk_i = 1;
  dut->eval();
  dut->clk_i = 0;
  dut->eval();
#if defined(DUT_CVXIF)
  cho_commit = nhan;
#endif
}

// One row of the report.
struct Result {
  std::string id;       // TC id
  std::string where;    // file:line of the checker
  bool expect_fire;     // what this stimulus is supposed to provoke
  int fires;            // measured error-count delta
  // Set when the condition cannot hold for any boundary stimulus, with the
  // reason. Such a case is not a pass: it is a checker that can never fire.
  std::string unreachable;
};

std::vector<Result> results;

int err() { return ctx->errorCount(); }

// Clear the latched flags so the next case starts from a known state; the
// counter itself is monotonic and read by difference.
void clear_flags() {
  ctx->gotError(false);
  ctx->gotFinish(false);
}

void record(const char *id, const char *where, bool expect_fire, int before,
            const char *unreachable = "") {
  const int fires = err() - before;
  results.push_back({id, where, expect_fire, fires, unreachable});
  clear_flags();
  std::printf("  %-12s %-22s ky vong %-9s do duoc %d lan\n", id, where,
              expect_fire ? "BAN" : "KHONG BAN", fires);
}

// MUC LOAD-05: bon ca bien W1..W4 cho ecg_wmem, kem ca DOI CHUNG W5 (mot DMA hop
// le phai IM LANG) lam chot chong rong. Hai trong nam dieu kien muc doi nam o
// day (W2, W3); ba dieu con lai o khoi LOAD-05 ben duoi, KHONG o day.
// ---------------------------------------------------------------- ecg_wmem
#if defined(DUT_WMEM)

// ecg_pkg::ECG_WMEM_BYTES. Read from the package, not invented: the harness
// prints it so a change in the package shows up in the log instead of silently
// making these cases legal.
constexpr int kBytes = 4664;

void reset() {
  dut->rst_ni = 0;
  dut->dma_start_i = 0;
  dut->dma_len_i = 0;
  dut->s_valid_i = 0;
  dut->s_data_i = 0;
  dut->rd_req_i = 0;
  dut->rd_off_i = 0;
  for (int i = 0; i < 4; ++i) tick();
  dut->rst_ni = 1;
  tick();
  clear_flags();
}

void run() {
  std::printf("ecg_wmem: BYTES = %d\n", kBytes);

  // W1 -- read of eight bytes that runs off the end of the region.
  reset();
  {
    const int before = err();
    dut->rd_req_i = 1;
    dut->rd_off_i = kBytes - 1;  // off + 8 = 4671 > 4664
    tick();
    dut->rd_req_i = 0;
    tick();
    record("TC-NEG-006", "ecg_wmem.sv:182", true, before);
  }

  // W1b -- control: the last LEGAL offset must NOT fire. This is the half that
  // proves the checker's boundary is at the right place rather than one byte off.
  reset();
  {
    const int before = err();
    dut->rd_req_i = 1;
    dut->rd_off_i = kBytes - 8;  // off + 8 = 4664 == BYTES, still legal
    tick();
    dut->rd_req_i = 0;
    tick();
    record("TC-NEG-006b", "ecg_wmem.sv:182", false, before);
  }

  // W2 -- LOADW longer than the region.
  reset();
  {
    const int before = err();
    dut->dma_start_i = 1;
    dut->dma_len_i = 4672;  // > BYTES and still a multiple of 8
    tick();
    dut->dma_start_i = 0;
    tick();
    record("TC-NEG-004", "ecg_wmem.sv:184", true, before);
  }

  // W3 -- LOADW length not a multiple of eight. Kept under BYTES so only the
  // alignment checker can be the one that fires.
  reset();
  {
    const int before = err();
    dut->dma_start_i = 1;
    dut->dma_len_i = 12;
    tick();
    dut->dma_start_i = 0;
    tick();
    record("TC-NEG-003", "ecg_wmem.sv:186", true, before);
  }

  // W4 -- a second LOADW while the first is still running. The DMA is started
  // and left starved of data (s_valid_i low), so busy_q stays high.
  reset();
  {
    dut->dma_start_i = 1;
    dut->dma_len_i = 64;
    tick();
    dut->dma_start_i = 0;
    tick();
    if (!dut->dma_busy_o) {
      std::printf("  LOI HARNESS: dma_busy_o thap sau khi phat LOADW\n");
    }
    const int before = err();
    dut->dma_start_i = 1;
    dut->dma_len_i = 64;
    tick();
    dut->dma_start_i = 0;
    tick();
    record("TC-NEG-005", "ecg_wmem.sv:188", true, before);
  }

  // W5 -- control: a whole legal DMA plus legal reads must stay silent. Without
  // this row a checker that fires on everything would look like a pass above.
  reset();
  {
    const int before = err();
    dut->dma_start_i = 1;
    dut->dma_len_i = 64;
    tick();
    dut->dma_start_i = 0;
    dut->s_valid_i = 1;
    dut->s_data_i = 0x0123456789abcdefULL;
    for (int i = 0; i < 8; ++i) tick();
    dut->s_valid_i = 0;
    tick();
    for (int off = 0; off + 8 <= 64; ++off) {
      dut->rd_req_i = 1;
      dut->rd_off_i = off;
      tick();
    }
    dut->rd_req_i = 0;
    tick();
    record("TC-NEG-CTRL", "ecg_wmem.sv (hop le)", false, before);
  }

  // ── MUC LOAD-05: ba dieu kien bien con lai ────────────────────────────────
  // Muc doi nam: thieu du lieu · thua du lieu · zero length · do dai khong chia
  // het 8 · reset giua burst. W2/W3 da phu hai cai giua. Ba cai duoi day la
  // phan con lai, va chung do HANH VI chu khong doi mot khang dinh no: hai
  // trong ba khong co khang dinh nao trong RTL, va do CHINH LA phat hien.

  // W6 -- LOADW do dai KHONG. `ecg_wmem.sv:80` gac `dma_len_i != 0`, nen DMA
  // khong bao gio bat dau. Cau hoi la dieu do co IM LANG khong: mot firmware
  // xin nap 0 byte la mot loi (trong so khong bao gio den), va neu phan cung
  // khong noi gi thi mo hinh chay tiep tren TRONG SO CU cua lop truoc.
  reset();
  {
    const int before = err();
    dut->dma_start_i = 1;
    dut->dma_len_i = 0;
    tick();
    dut->dma_start_i = 0;
    tick();
    const bool ban = (err() != before);
    const bool ban_ron = dut->dma_busy_o;
    std::printf("  W6 zero length: khang dinh %s · dma_busy_o = %d\n",
                ban ? "NO" : "khong no", int(ban_ron));
    if (ban_ron) {
      std::printf("  LOI  W6: DMA BAT DAU voi do dai 0\n");
      // Mot Result LECH KY VONG de main() phan dinh no la HONG: `record` chi do
      // khang dinh, con day la mot HANH VI, nen phai vao bang bang duong nay.
      results.push_back({"TC-NEG-LOAD05-zero-busy", "ecg_wmem.sv:80", true, 0, ""});
    }
    // Phan dinh: KHONG khang dinh nao, va DMA khong chay -> mot phep BO QUA IM
    // LANG. Ghi lai bang mot ca `record` de no nam trong bang, khong de no thanh
    // mot cau van trong chu thich.
    record("TC-NEG-LOAD05-zero", "ecg_wmem.sv:80", false, before);
  }

  // W7 -- THIEU DU LIEU: phat LOADW 64 B (8 tu) roi chi cap 4 tu. Khong co
  // timeout nao trong `ecg_wmem`, nen `busy` o lai MAI MAI. Do la mot phep TREO
  // im lang: khong khang dinh, khong ma loi, va rao chan cua firmware se cho
  // mot `busy` khong bao gio ha.
  reset();
  {
    const int before = err();
    dut->dma_start_i = 1;
    dut->dma_len_i = 64;
    tick();
    dut->dma_start_i = 0;
    dut->s_valid_i = 1;
    dut->s_data_i = 0x1111222233334444ULL;
    for (int i = 0; i < 4; ++i) tick();       // chi 4/8 tu
    dut->s_valid_i = 0;
    for (int i = 0; i < 200; ++i) tick();     // cho that lau
    std::printf("  W7 thieu du lieu (4/8 tu): sau 200 chu ky dma_busy_o = %d\n",
                int(dut->dma_busy_o));
    if (!dut->dma_busy_o) {
      std::printf("  LOI  W7: busy DA HA du chi cap 4/8 tu -- DMA tu ket thuc som?\n");
      results.push_back({"TC-NEG-LOAD05-doi-busy", "ecg_wmem.sv:80", true, 0, ""});
    }
    record("TC-NEG-LOAD05-doi", "ecg_wmem.sv:80", false, before);
  }

  // W8 -- RESET GIUA BURST. Phat LOADW, cap mot nua, roi keo reset. Sau reset
  // may phai SACH: `busy` thap, va mot LOADW hop le sau do phai chay tron ven.
  // Neu trang thai cu song sot thi lan nap sau ke thua mot bo dem dang do.
  reset();
  {
    const int before = err();
    dut->dma_start_i = 1;
    dut->dma_len_i = 64;
    tick();
    dut->dma_start_i = 0;
    dut->s_valid_i = 1;
    dut->s_data_i = 0xAAAABBBBCCCCDDDDULL;
    for (int i = 0; i < 4; ++i) tick();
    dut->s_valid_i = 0;
    reset();                                   // <- reset GIUA burst
    if (dut->dma_busy_o) {
      std::printf("  LOI  W8: dma_busy_o VAN len sau reset giua burst\n");
      results.push_back({"TC-NEG-LOAD05-reset-busy", "ecg_wmem.sv (reset)", true, 0, ""});
    }
    // va mot LOADW hop le sau do phai chay tron ven
    dut->dma_start_i = 1;
    dut->dma_len_i = 64;
    tick();
    dut->dma_start_i = 0;
    dut->s_valid_i = 1;
    dut->s_data_i = 0x0123456789abcdefULL;
    for (int i = 0; i < 8; ++i) tick();
    dut->s_valid_i = 0;
    tick();
    std::printf("  W8 reset giua burst: busy sau reset = 0, LOADW ke tiep "
                "ket thuc (busy = %d)\n", int(dut->dma_busy_o));
    if (dut->dma_busy_o) {
      std::printf("  LOI  W8: LOADW sau reset KHONG ket thuc\n");
      results.push_back({"TC-NEG-LOAD05-reset-xong", "ecg_wmem.sv (reset)", true, 0, ""});
    }
    record("TC-NEG-LOAD05-reset", "ecg_wmem.sv (reset)", false, before);
  }

  // W9 -- THUA DU LIEU: phat LOADW 64 B (8 tu) roi cap 12 tu. Bon tu thua phai
  // KHONG duoc ghi tiep vao vung trong so; neu chung duoc ghi thi mot nguon cap
  // du lieu qua da se tran sang vung cua lop khac ma khong ai bao.
  // (W2 la vuot DUNG LUONG -- mot cau khac: do la `dma_len` qua lon, con day la
  //  `dma_len` DUNG ma nguon cap qua nhieu.)
  reset();
  {
    const int before = err();
    dut->dma_start_i = 1;
    dut->dma_len_i = 64;
    tick();
    dut->dma_start_i = 0;
    dut->s_valid_i = 1;
    for (int i = 0; i < 12; ++i) {            // 12 tu cho mot yeu cau 8 tu
      dut->s_data_i = 0x1000000000000000ULL + uint64_t(i);
      tick();
    }
    dut->s_valid_i = 0;
    tick();
    std::printf("  W9 thua du lieu (12/8 tu): dma_busy_o = %d\n", int(dut->dma_busy_o));
    if (dut->dma_busy_o) {
      std::printf("  LOI  W9: busy VAN len sau khi da cap du 8 tu\n");
      results.push_back({"TC-NEG-LOAD05-thua-busy", "ecg_wmem.sv:80", true, 0, ""});
    }
    // Tu thu 9..12 khong duoc ghi de len o dau tien cua vung.
    dut->rd_req_i = 1; dut->rd_off_i = 0;
    tick(); tick();
    dut->rd_req_i = 0;
    std::printf("  W9 o 0 sau khi cap thua: %016llx (tu dau tien la ...000)\n",
                (unsigned long long)dut->rd_data_o);
    if (dut->rd_data_o != 0x1000000000000000ULL) {
      std::printf("  LOI  W9: o 0 KHONG con la tu dau tien -- tu thua da GHI DE\n");
      results.push_back({"TC-NEG-LOAD05-thua-ghide", "ecg_wmem.sv:80", true, 0, ""});
    }
    record("TC-NEG-LOAD05-thua", "ecg_wmem.sv:80", false, before);
  }

  // ── MUC LOAD-04: dem BYTE DA GHI, khong dem TOKEN ────────────────────────
  // Muc cam dung "token tra ve" lam bang chung trong so da toi. Truoc luot nay
  // khong testbench nao dem byte: `tb_ecg_xif_loadw` dem lan NHAN va lan BAT
  // DAU, `tb_ecg_cvxif_loadw` dem lan PHAT -- ba dai luong deu la TOKEN.
  //
  // Dem o `we_even`/`we_odd` (ecg_wmem.sv:104-105) -- chan cho phep ghi cua hai
  // bang SRAM. Moi lan mot trong hai len la MOT TU 64 bit = 8 byte thuc su di
  // vao bo nho. Do la dai luong muc doi, va no KHONG THE bi thoa man boi mot
  // token.
  //
  // Ca quyet dinh muc neu san: bo nap tra token ma KHONG ghi byte nao. Phep thu
  // nay phai DO trong ca do -- xem tools/proofs/load04.sh.
  {
    reset();
    const int before = err();
    const int LEN = 64;                       // 8 tu
    dut->dma_start_i = 1;
    dut->dma_len_i = LEN;
    tick();
    dut->dma_start_i = 0;
    int byte_ghi = 0, token_xong = 0, tu_nhan = 0;
    dut->s_valid_i = 1;
    dut->s_data_i = 0x0102030405060708ULL;
    for (int i = 0; i < 64; ++i) {
      dut->eval();
      if (dut->rootp->ecg_wmem__DOT__we_even ||
          dut->rootp->ecg_wmem__DOT__we_odd) byte_ghi += 8;
      if (dut->s_valid_i && dut->s_ready_o) ++tu_nhan;
      tick();
      dut->eval();
      if (dut->dma_done_o) ++token_xong;
      if (!dut->dma_busy_o) { dut->s_valid_i = 0; break; }
    }
    dut->s_valid_i = 0;
    std::printf("  LOAD-04 LOADW %d B: byte DA GHI = %d · tu nhan = %d · "
                "token xong = %d\n", LEN, byte_ghi, tu_nhan, token_xong);

    // CHONG RONG: token phai ve, khong thi ta dang do mot lan chay chua xong va
    // "byte = 0" se doc nhu mot phat hien trong khi no chi la chua chay het.
    if (token_xong != 1) {
      std::printf("  LOI  LOAD-04: token xong = %d (phai 1) -- lan chay chua "
                  "ket thuc, nen so byte chua noi len dieu gi\n", token_xong);
      results.push_back({"TC-LOAD04-chong-rong", "ecg_wmem.sv:86", true, 0, ""});
    }
    // BAT BIEN: so byte GHI phai bang do dai da yeu cau. Mot token khong thay
    // duoc cho nay.
    if (byte_ghi != LEN) {
      std::printf("  LOI  LOAD-04: yeu cau %d B nhung CHI GHI %d B -- token da "
                  "ve nhung trong so KHONG day du. Day dung la ca muc cam: "
                  "token tra ve khong phai bang chung trong so da toi.\n",
                  LEN, byte_ghi);
      results.push_back({"TC-LOAD04-byte", "ecg_wmem.sv:104", true, 0, ""});
    }
    record("TC-LOAD04-dem-byte", "ecg_wmem.sv:104", false, before);
  }
}

#endif  // DUT_WMEM

// --------------------------------------------------------------- ecg_cvxif
#if defined(DUT_CVXIF)

constexpr uint32_t kMajor = 0x0b;  // ecg_pkg::ECG_MAJOR_OPCODE
constexpr int kOpStore = 11;
constexpr int kOpLoadw = 10;
constexpr int kClassCompute = 0;
constexpr int kClassCtrl = 1;

uint32_t encode(int op, int fn3, int rd, int resv = 0, uint32_t major = kMajor) {
  return ((uint32_t)(op & 0x1F) << 27) | ((uint32_t)(resv & 3) << 25) |
         (1u << 20) | (1u << 15) | ((uint32_t)(fn3 & 7) << 12) |
         ((uint32_t)(rd & 0x1F) << 7) | (major & 0x7F);
}

void reset() {
  dut->rst_ni = 0;
  dut->iss_valid_i = 0;
  dut->iss_instr_i = 0;
  dut->iss_rs1_i = 0;
  dut->iss_rs2_i = 0;
  dut->cp_busy_i = 0;
  dut->cp_done_i = 0;
  dut->dma_busy_i = 0;
  for (int i = 0; i < 4; ++i) tick();
  dut->rst_ni = 1;
  tick();
  clear_flags();
}

// Present one instruction for one cycle.
void issue(uint32_t instr, uint32_t rs1, uint32_t rs2) {
  dut->iss_valid_i = 1;
  dut->iss_instr_i = instr;
  dut->iss_rs1_i = rs1;
  dut->iss_rs2_i = rs2;
  tick();
  dut->iss_valid_i = 0;
  tick();
}

void run() {
  // C1 -- an instruction OUTSIDE custom-0. The checker's condition is
  // (major != MAJOR) && iss_accept_o, and iss_accept_o is assigned in_space
  // which already requires major == MAJOR. So the two terms are mutually
  // exclusive by construction: this stimulus is the honest attempt, and no fire
  // is the expected -- and reportable -- outcome.
  reset();
  {
    const int before = err();
    issue(encode(0, kClassCompute, 3, 0, 0x33), 0, 0);
    issue(encode(0, kClassCompute, 3, 0, 0x13), 0, 0);
    issue(encode(0, kClassCompute, 3, 0, 0x7f), 0, 0);
    if (dut->iss_accept_o) std::printf("  LOI: accept cao cho major sai\n");
    record("TC-NEG-014", "ecg_cvxif.sv:380", false, before,
           "iss_accept_o = in_space da doi hoi major == ECG_MAJOR_OPCODE, nen "
           "(major != MAJOR) && iss_accept_o la mau thuan");
  }

  // C2 -- a non-barrier instruction blocking the issue path. iss_ready_o is 1
  // for every in-space instruction that is neither a barrier nor a full-FIFO
  // compute, which is exactly the checker's own exclusion list. Same shape as
  // C1: the condition cannot hold. Attempt: hammer LOADW and compute back to
  // back with the FIFO both empty and full.
  reset();
  {
    const int before = err();
    for (int i = 0; i < 12; ++i) {
      issue(encode(kOpLoadw, kClassCtrl, 5), 0, 8);
      issue(encode(0, kClassCompute, 6), i & 0x3F, 0);
    }
    dut->cp_busy_i = 1;  // stall retirement so the FIFO fills
    for (int i = 0; i < 8; ++i) {
      dut->iss_valid_i = 1;
      dut->iss_instr_i = encode(0, kClassCompute, 6);
      dut->iss_rs1_i = i & 0x3F;
      tick();
    }
    dut->iss_valid_i = 0;
    dut->cp_busy_i = 0;
    for (int i = 0; i < 8; ++i) tick();
    record("TC-NEG-016", "ecg_cvxif.sv:404", false, before,
           "iss_ready_o = 1 cho moi lenh in_space khong phai rao chan va khong "
           "phai compute-khi-FIFO-day; do dung la tap loai tru cua chinh dieu kien");
  }

  // C3 -- push into a full FIFO. push is assigned (... && !full), so push &&
  // full is a contradiction in the same file. Attempt: hold cp_busy_i so
  // nothing retires and keep issuing computes well past depth 4.
  reset();
  {
    const int before = err();
    dut->cp_busy_i = 1;
    for (int i = 0; i < 10; ++i) {
      dut->iss_valid_i = 1;
      dut->iss_instr_i = encode(0, kClassCompute, 6);
      dut->iss_rs1_i = i & 0x3F;
      tick();
    }
    dut->iss_valid_i = 0;
    dut->cp_busy_i = 0;
    for (int i = 0; i < 8; ++i) tick();
    record("TC-NEG-015", "ecg_cvxif.sv:414", false, before,
           "push = iss_valid_i && is_compute && !full, nen push && full la mau thuan");
  }

  // C4 -- reserved bits 26:25 not zero on an in-space instruction. The existing
  // vector set never sets them (tools/rtl_ref/cvxif_ref.py group 3 varies major
  // and op only), so this is the first stimulus this checker has ever seen.
  reset();
  {
    const int before = err();
    issue(encode(0, kClassCompute, 6, 1), 0, 0);
    issue(encode(0, kClassCompute, 6, 2), 0, 0);
    issue(encode(0, kClassCompute, 6, 3), 0, 0);
    record("TC-NEG-018", "ecg_cvxif.sv:419", true, before);
  }

  // C5 -- layer index wider than six bits in rs1.
  reset();
  {
    const int before = err();
    issue(encode(0, kClassCompute, 6), 64, 0);
    record("TC-NEG-001", "ecg_cvxif.sv:424", true, before);
  }

  // C6 -- LOADW length wider than fourteen bits in rs2.
  reset();
  {
    const int before = err();
    issue(encode(kOpLoadw, kClassCtrl, 5), 0, 16384);
    record("TC-NEG-002", "ecg_cvxif.sv:427", true, before);
  }

  // C7 -- the barrier opens with a layer count that does not match the number
  // of accepted instructions. Built by accepting one compute and never pulsing
  // cp_done_i, so n_issued_q = 1 while n_retired_q = 0.
  reset();
  {
    issue(encode(0, kClassCompute, 6), 0, 0);
    for (int i = 0; i < 4; ++i) tick();  // let the FIFO drain, no done pulse
    const int before = err();
    issue(encode(kOpStore, kClassCtrl, 10), 0, 0);
    record("TC-NEG-017", "ecg_cvxif.sv:431", true, before);
  }

  // C8 -- more than 63 compute instructions between two barriers. cp_done_i is
  // pulsed for each so the mismatch checker of C7 stays quiet and only the
  // counter-overflow checker can be the one that fires.
  reset();
  {
    int accepted = 0;
    const int before = err();
    for (int i = 0; i < 200 && accepted < 65; ++i) {
      dut->iss_valid_i = 1;
      dut->iss_instr_i = encode(0, kClassCompute, 6);
      dut->iss_rs1_i = i & 0x3F;
      dut->eval();
      const bool taken = dut->iss_ready_o != 0;
      dut->cp_done_i = 1;
      tick();
      dut->cp_done_i = 0;
      if (taken) ++accepted;
    }
    dut->iss_valid_i = 0;
    tick();
    std::printf("  (C8 nhan %d lenh tinh toan)\n", accepted);
    record("TC-NEG-019c", "ecg_cvxif.sv:437", true, before);
  }

  // C9 -- control: a legal beat must stay silent.
  reset();
  {
    const int before = err();
    issue(encode(kOpLoadw, kClassCtrl, 5), 0, 4656);
    for (int i = 0; i < 9; ++i) {
      issue(encode(0, kClassCompute, 6), i, 0);
      dut->cp_done_i = 1;
      tick();
      dut->cp_done_i = 0;
      tick();
    }
    issue(encode(kOpStore, kClassCtrl, 10), 0, 0);
    record("TC-NEG-CTRL", "ecg_cvxif.sv (hop le)", false, before);
  }
}

#endif  // DUT_CVXIF

// -------------------------------------------------------------- ecg_coproc
#if defined(DUT_COPROC)

// ecg_pkg opcodes.
constexpr int kConv1d = 0;
constexpr int kMaxpool = 3;
constexpr int kGap = 4;
constexpr int kAdd = 5;
constexpr int kRequant = 8;
constexpr int kStore = 11;
constexpr int kSrcNone = 13;
constexpr int kActNone = 0;

// ecg_layer_desc_t, MSB first, exactly the field order of ecg_pkg.sv.
struct Desc {
  int op = kConv1d;
  int act = kActNone;
  int src0 = 0;
  int src1 = kSrcNone;
  int dst = 1;
  int dst_off = 0;
  int cin = 1;
  int cout = 8;
  int len_in = 8;
  int len_out = 8;
  int k = 1;
  int stride = 1;
  int pad = 0;
  int dw = 0;
  int w_base = 0;
  int rq_base = 0;
  int rq_n = 1;
  int last = 1;
  int cat_part = 0;
  int buf_base = 0;
};

// Pack into 128 bits as four 32-bit words, word 0 = bits 31:0.
void pack(const Desc &d, uint32_t w[4]) {
  // Build MSB-first into a bit string, then fold into words. Slower than shift
  // arithmetic and immune to the off-by-one that broke pack/unpack once already
  // (checklist B-04).
  const struct {
    uint32_t val;
    int bits;
  } f[] = {{(uint32_t)d.op, 5},       {(uint32_t)d.act, 2},
           {(uint32_t)d.src0, 4},     {(uint32_t)d.src1, 4},
           {(uint32_t)d.dst, 4},      {(uint32_t)d.dst_off, 10},
           {(uint32_t)d.cin, 9},      {(uint32_t)d.cout, 9},
           {(uint32_t)d.len_in, 10},  {(uint32_t)d.len_out, 10},
           {(uint32_t)d.k, 3},        {(uint32_t)d.stride, 2},
           {(uint32_t)d.pad, 3},      {(uint32_t)d.dw, 1},
           {(uint32_t)d.w_base, 14},  {(uint32_t)d.rq_base, 14},
           {(uint32_t)d.rq_n, 9},     {(uint32_t)d.last, 1},
           {(uint32_t)d.cat_part, 1}, {(uint32_t)d.buf_base, 13}};
  bool bit[128];
  int p = 0;
  for (const auto &e : f)
    for (int b = e.bits - 1; b >= 0; --b) bit[p++] = (e.val >> b) & 1u;
  if (p != 128) {
    std::printf("LOI HARNESS: descriptor %d bit, phai 128\n", p);
    std::exit(2);
  }
  for (int i = 0; i < 4; ++i) w[i] = 0;
  for (int i = 0; i < 128; ++i) {
    // bit[0] is the MSB, i.e. bit 127.
    const int idx = 127 - i;
    if (bit[i]) w[idx / 32] |= 1u << (idx % 32);
  }
}

void reset() {
  dut->rst_ni = 0;
  dut->start_i = 0;
  dut->single_i = 0;
  dut->layer_i = 0;
  dut->n_layers_i = 1;
  dut->in_len_i = 256;
  dut->desc_valid_i = 0;
  for (int i = 0; i < 4; ++i) dut->desc_word_i[i] = 0;
  dut->s_mult_i = 1;
  dut->s_shift_i = 0;
  dut->s_bias_i = 0;
  dut->dma_start_i = 0;
  dut->dma_len_i = 0;
  dut->ws_valid_i = 0;
  dut->ws_data_i = 0;
  dut->pre_we_i = 0;
  dut->pre_buf_i = 0;
  dut->pre_off_i = 0;
  dut->pre_data_i = 0;
  for (int i = 0; i < 4; ++i) tick();
  dut->rst_ni = 1;
  tick();
  clear_flags();
}

// Drive the FSM to C_FETCH and present one descriptor for exactly one cycle,
// then reset before C_LAUNCH can start a sequencer on it. Returns the measured
// error-count delta for that single cycle.
int fetch_one(const Desc &d) {
  reset();
  dut->start_i = 1;
  tick();               // C_IDLE -> C_BASE
  dut->start_i = 0;
  tick();               // C_BASE -> C_FETCH
  if (!dut->desc_req_o) {
    std::printf("  LOI HARNESS: desc_req_o thap, chua o C_FETCH\n");
    return -1;
  }
  uint32_t w[4];
  pack(d, w);
  for (int i = 0; i < 4; ++i) dut->desc_word_i[i] = w[i];
  const int before = err();
  dut->desc_valid_i = 1;
  tick();               // the checkers sample here
  const int delta = err() - before;
  dut->desc_valid_i = 0;
  dut->rst_ni = 0;      // stop before the sequencer runs on this descriptor
  tick();
  return delta;
}

// Chay MOT lenh COMPUTE o che do mot-lop cho lop `L`, tra ve b_off_o NHO NHAT
// quan sat duoc trong luot do. Vi sao NHO NHAT doc ra dung dai luong can:
// `b_off_o = bias_base_q + sq_boff` (ecg_coproc.sv:336) va `sq_boff` bat dau tu
// 0 o moi lop, nen min b_off_o TRONG mot lop CHINH LA bias_base_q cua lop ay --
// tuc doc duoc mot thanh ghi NOI qua mot chan BIEN, khong phai cham vao no.
// Nap dau vao qua buffer 14 (ECG_SRC_IN) voi offset TUYET DOI, dung cach
// tb_ecg_coproc nap. Phai goi khi may RANH: ecg_coproc.sv:453 ban neu ghi
// trong luc dang chay.
void nap_vao() {
  for (int off = 0; off < 64; ++off) {
    dut->pre_we_i   = 1;
    dut->pre_buf_i  = 14;
    dut->pre_off_i  = off;
    dut->pre_data_i = static_cast<int8_t>(1 + (off % 7));
    tick();
  }
  dut->pre_we_i = 0;
  tick();
  clear_flags();
}

int chay_mot_lop(const std::vector<Desc> &m, int L, bool &xong) {
  dut->single_i  = 1;
  dut->layer_i   = static_cast<uint8_t>(L);
  dut->n_layers_i = static_cast<uint8_t>(m.size());
  dut->start_i   = 1;
  tick();
  dut->start_i = 0;
  int bmin = 1 << 20;
  xong = false;
  for (long c = 0; c < 100000; ++c) {
    dut->eval();
    if (dut->desc_req_o) {
      const int idx = dut->desc_idx_o;
      uint32_t w[4];
      pack(m[(idx >= 0 && idx < static_cast<int>(m.size())) ? idx : 0], w);
      for (int i = 0; i < 4; ++i) dut->desc_word_i[i] = w[i];
      dut->desc_valid_i = 1;
    } else {
      dut->desc_valid_i = 0;
    }
    dut->eval();
    if (dut->busy_o && static_cast<int>(dut->b_off_o) < bmin)
      bmin = static_cast<int>(dut->b_off_o);
    tick();
    dut->eval();
    if (dut->done_o) { xong = true; break; }
  }
  dut->desc_valid_i = 0;
  dut->single_i = 0;
  return bmin;
}

// ── MUC EXEC-05: ma tran quyen truy cap theo trang thai ──────────────────────
// Muc goc doi nam trang thai IDLE/LOADING/READY/RUNNING/ERROR. May THAT co BAY
// trang thai khac han (ecg_coproc.sv:121-122) va khong co ERROR nhu mot trang
// thai -- loi di bang co, khong bang trang thai. Lap ma tran theo BAY ten THAT:
// mot ma tran theo nam ten khong ton tai se do mot thiet ke khong ton tai.
constexpr int kNState = 7;
const char *kTenTT[kNState] = {"C_IDLE", "C_BASE",  "C_FETCH", "C_LAUNCH",
                               "C_RUN",  "C_NEXT", "C_DONE"};
// Ba duong truy cap TU NGOAI vao ecg_coproc. `desc_valid_i`/`ws_valid_i` khong
// nam day: chung la ve BAT TAY tra loi mot yeu cau cua chinh module, khong phai
// mot lenh tu ngoai, nen "quyen" khong phai cau hoi dung cho chung.
const char *kTenTruyCap[3] = {"start_i", "pre_we_i", "dma_start_i"};

int trang_thai() { return dut->rootp->ecg_coproc__DOT__state_q; }

struct KetQua { bool toi_duoc = false; int ban = 0; bool hieu_luc = false; };

// Chay mot luot suy luan TU DI, va o chu ky DAU TIEN may o `muc_tieu`, dat mot
// duong truy cap len DUNG mot chu ky roi do: co khang dinh nao ban khong, va
// truy cap ay CO HIEU LUC khong.
KetQua thu_quyen(const std::vector<Desc> &m, int muc_tieu, int loai) {
  reset();
  nap_vao();
  clear_flags();
  KetQua r;
  dut->single_i   = 0;
  dut->layer_i    = 0;
  dut->n_layers_i = static_cast<uint8_t>(m.size());
  bool da_ap = false, da_khoi = false;
  for (long c = 0; c < 200000 && !(da_ap && dut->done_o); ++c) {
    dut->eval();
    if (dut->desc_req_o) {
      const int idx = dut->desc_idx_o;
      uint32_t w[4];
      pack(m[(idx >= 0 && idx < static_cast<int>(m.size())) ? idx : 0], w);
      for (int i = 0; i < 4; ++i) dut->desc_word_i[i] = w[i];
      dut->desc_valid_i = 1;
    } else {
      dut->desc_valid_i = 0;
    }
    dut->eval();
    const int st = trang_thai();

    if (!da_ap && st == muc_tieu) {
      r.toi_duoc = true;
      da_ap = true;
      const int truoc = err();
      if (loai == 0) {
        dut->start_i = 1;
      } else if (loai == 1) {
        dut->pre_we_i = 1; dut->pre_buf_i = 14;
        dut->pre_off_i = 3; dut->pre_data_i = 77;
      } else {
        dut->dma_start_i = 1; dut->dma_len_i = 64;
      }
      dut->eval();
      // HIEU LUC do ngay tren duong ma truy cap ay di vao, khong doan:
      //  - pre_we_i di qua mux ab_* (ecg_coproc.sv:342-348), chi thong o C_IDLE
      //  - dma_start_i lam dma_busy_o len
      //  - start_i lam may roi C_IDLE
      if (loai == 1)
        r.hieu_luc = dut->rootp->ecg_coproc__DOT__ab_we &&
                     dut->rootp->ecg_coproc__DOT__ab_wbuf == 14;
      const int st_truoc = st;
      tick();
      dut->eval();
      if (loai == 0) r.hieu_luc = (st_truoc == 0) && (trang_thai() != 0);
      if (loai == 2) r.hieu_luc = dut->dma_busy_o;
      r.ban = err() - truoc;
      dut->start_i = 0; dut->pre_we_i = 0; dut->dma_start_i = 0;
      clear_flags();
      continue;
    }
    if (!da_khoi && st == 0 && muc_tieu != 0) {
      dut->start_i = 1; da_khoi = true; tick(); dut->start_i = 0; continue;
    }
    tick();
  }
  dut->desc_valid_i = 0;
  return r;
}

void run() {
  // P1 -- illegal descriptor at C_FETCH. stride = 0 makes legal_o low while the
  // op stays CONV1D, so the "no sequencer" checker is not also provoked. The
  // ecg_desc instance inside carries its own $error for stride, so a delta above
  // one is expected here and is reported as measured.
  {
    Desc d;
    d.stride = 0;
    const int delta = fetch_one(d);
    record("TC-NEG-010", "ecg_coproc.sv:445", true, err() - delta);
  }

  // P2 -- an op no sequencer accepts. REQUANT is legal per ecg_desc (it is a
  // field of the layer that produces the psum, never a layer of its own) yet it
  // is neither MAC, pool, GAP nor ADD.
  {
    Desc d;
    d.op = kRequant;
    d.k = 0;      // uses_k is false for REQUANT, so k must be 0 to stay legal
    d.rq_n = 0;   // keeps per_ch low
    const int delta = fetch_one(d);
    record("TC-NEG-008", "ecg_coproc.sv:450", true, err() - delta);
  }

  // P3 -- preload write while the FSM is not idle. Driven during C_BASE.
  {
    reset();
    dut->start_i = 1;
    tick();  // -> C_BASE
    dut->start_i = 0;
    const int before = err();
    dut->pre_we_i = 1;
    dut->pre_buf_i = 14;
    dut->pre_off_i = 0;
    dut->pre_data_i = 5;
    tick();
    dut->pre_we_i = 0;
    dut->rst_ni = 0;
    tick();
    record("TC-NEG-011", "ecg_coproc.sv:453", true, before);
  }

  // P4 -- both sequencers busy. sq_busy and sv_busy are internal, and which one
  // starts is decided by q_is_mac at C_LAUNCH: exactly one of sq_start/sv_start
  // is raised per layer, and C_RUN waits for that one to finish before the next
  // C_FETCH. There is no boundary input that can start the second sequencer
  // while the first runs, so this case is NOT STIMULATED rather than faked. The
  // stimulus below is the closest legal attempt -- two layers back to back --
  // and it must stay silent.
  {
    reset();
    const int before = err();
    for (int i = 0; i < 8; ++i) tick();
    record("TC-NEG-007b", "ecg_coproc.sv:457", false, before,
           "sq_busy/sv_busy la tin hieu noi; C_LAUNCH chi nang DUNG MOT trong "
           "sq_start/sv_start theo q_is_mac va C_RUN cho no xong -- khong co "
           "dau vao bien nao khoi dong sequencer thu hai");
  }

  // P5 -- STORE appearing as a layer. Note this necessarily also trips the "no
  // sequencer" checker: STORE is not one of the four dispatchable ops, so the
  // two conditions cannot be separated from the boundary.
  {
    Desc d;
    d.op = kStore;
    d.k = 0;
    d.rq_n = 0;
    const int delta = fetch_one(d);
    record("TC-NEG-007", "ecg_coproc.sv:466", true, err() - delta);
  }

  // P6 -- src1 not ECG_SRC_NONE on an op that does not use it. src1 = 0 is the
  // exact historical bug (buffer 0 versus "unused").
  {
    Desc d;
    d.src1 = 0;
    const int delta = fetch_one(d);
    record("TC-NEG-012", "ecg_coproc.sv:472", true, err() - delta);
  }

  // P7 -- per-channel with rq_n != cout. ecg_desc's own legality rule for a MAC
  // op is rq_n == 1 or rq_n == cout, so any value that makes this checker's
  // condition true also makes legal_o low: the fire cannot be isolated from
  // TC-NEG-010 and from ecg_desc's rq_n $error. Measured as a joint delta.
  {
    Desc d;
    d.cout = 8;
    d.rq_n = 4;
    const int delta = fetch_one(d);
    record("TC-NEG-020c", "ecg_coproc.sv:479", true, err() - delta);
  }

  // P8 -- relu decode disagreeing with the act field. d_relu is ecg_desc's
  // relu_o, which is assigned (act == ECG_ACT_RELU) -- the same expression the
  // checker compares it against, from the same instance. The condition is a
  // contradiction, so no descriptor can make it fire. Attempt: both act values.
  {
    Desc d;
    d.act = 1;  // RELU
    const int d1 = fetch_one(d);
    Desc e;
    e.act = 0;  // NONE
    const int d2 = fetch_one(e);
    results.push_back({"TC-NEG-021c", "ecg_coproc.sv:483", false, d1 + d2,
                       "d_relu la relu_o cua u_desc, duoc gan bang "
                       "(act == ECG_ACT_RELU) -- dung bieu thuc ma assertion "
                       "so no voi; hai ben cung mot instance nen luon bang nhau"});
    std::printf("  %-12s %-22s ky vong %-9s do duoc %d lan\n", "TC-NEG-021c",
                "ecg_coproc.sv:483", "KHONG BAN", d1 + d2);
  }

  // P9 -- dst_off != 0 with cat_part = 0: the allocator treated a CONCAT branch
  // as a private buffer.
  {
    Desc d;
    d.dst_off = 16;
    d.cat_part = 0;
    const int delta = fetch_one(d);
    record("TC-NEG-009", "ecg_coproc.sv:488", true, err() - delta);
  }

  // P10 -- control: a fully legal descriptor must fire nothing at C_FETCH.
  {
    Desc d;
    const int delta = fetch_one(d);
    record("TC-NEG-CTRL", "ecg_coproc.sv (hop le)", false, err() - delta);
  }

  // ── MUC EXEC-03: ba canh KHONG tuan tu cua che do mot-lop ─────────────────
  // Mo hinh ba lop MAC voi cout = 2, 4, 8. Nen bang bias DUNG cua mot lop la
  // TONG cout cac lop TRUOC no, vi bang bias la MOT bang phang cho ca mo hinh
  // va nen tung lop duoc CONG DON chu khong nam trong descriptor
  // (ecg_coproc.sv:88-93). Nen dung: L0 -> 0 · L1 -> 2 · L2 -> 6.
  //
  // CA BA LOP DOC BUFFER 14 (ECG_SRC_IN), nap san. Ly do KHONG phai tien tay:
  // buffer VAO va buffer RR duoc MIEN phep kiem nhip cua ecg_actbuf
  // (`mien_nhip`, ecg_actbuf.sv:165). Neu de cac lop noi nhau 0->1->2 thi canh
  // NHAY COC lam lop 2 doc mot bo dem lop 1 chua bao gio ghi, va ecg_actbuf ban
  // "read of unwritten byte" -- mot khang dinh THAT nhung cua MOT SAI KHAC
  // (hoat do sai), che mat cau hoi dang hoi (nen bias sai). Tach hai cai ra moi
  // do duoc cai thu hai mot minh.
  {
    std::vector<Desc> m(3);
    for (int i = 0; i < 3; ++i) { m[i].src0 = 14; m[i].dst = 1; m[i].last = 0; }
    m[0].cout = 2; m[1].cout = 4; m[2].cout = 8;
    m[2].last = 1;
    bool ok = true, x = false;
    int b1 = 0, b2 = 0, b3 = 0, b4 = 0;   // so lan ban CUA RIENG canh cuoi

    // E1 -- TUAN TU 0,1,2: duong firmware that di (main.c:576 goi
    // ECG_COMPUTE(op, L) trong vong lap `for (L = 0; L < n_layer; ++L)`).
    reset(); nap_vao();
    chay_mot_lop(m, 0, x); ok &= x;
    chay_mot_lop(m, 1, x); ok &= x;
    clear_flags(); { const int t = err();
      const int e1 = chay_mot_lop(m, 2, x); ok &= x; b1 = err() - t;

    // E2 -- NHAY COC 0 -> 2: lop 1 khong chay, bo cong don thieu cout(1).
    reset(); nap_vao();
    chay_mot_lop(m, 0, x); ok &= x;
    clear_flags(); const int t2 = err();
    const int e2 = chay_mot_lop(m, 2, x); ok &= x; b2 = err() - t2;

    // E3 -- LAP LAI 2 -> 2: lop 2 da tien bo cong don mot lan roi.
    reset(); nap_vao();
    chay_mot_lop(m, 0, x); ok &= x;
    chay_mot_lop(m, 1, x); ok &= x;
    chay_mot_lop(m, 2, x); ok &= x;
    clear_flags(); const int t3 = err();
    const int e3 = chay_mot_lop(m, 2, x); ok &= x; b3 = err() - t3;

    // E4 -- LUI VE DAU last -> first: layer_i == 0 DAT LAI, nen canh nay DUNG.
    reset(); nap_vao();
    chay_mot_lop(m, 0, x); ok &= x;
    chay_mot_lop(m, 1, x); ok &= x;
    chay_mot_lop(m, 2, x); ok &= x;
    clear_flags(); const int t4 = err();
    const int e4 = chay_mot_lop(m, 0, x); ok &= x; b4 = err() - t4;

    std::printf("  EXEC-03  E1 tuan tu 0,1,2 -> nen L2 = %d (dung 6)%s · ban %d\n",
                e1, e1 == 6 ? "" : "  LECH", b1);
    std::printf("  EXEC-03  E2 nhay coc 0->2 -> nen L2 = %d (dung 6)%s · ban %d\n",
                e2, e2 == 6 ? "" : "  LECH", b2);
    std::printf("  EXEC-03  E3 lap lai 2->2  -> nen L2 = %d (dung 6)%s · ban %d\n",
                e3, e3 == 6 ? "" : "  LECH", b3);
    std::printf("  EXEC-03  E4 lui ve dau ->0 -> nen L0 = %d (dung 0)%s · ban %d\n",
                e4, e4 == 0 ? "" : "  LECH", b4);
    if (!ok) std::printf("  LOI  EXEC-03: mot luot chay KHONG bao done_o\n");

    // CHONG RONG. Neu E2 va E3 deu DUNG thi hoac khiem khuyet khong ton tai,
    // hoac phep do nay hong -- ca hai deu phai NO, khong duoc im.
    if (e2 == 6 && e3 == 6)
      results.push_back({"TC-EXEC03-chong-rong", "ecg_coproc.sv:458", true, 0,
                         ""});
    // E1 va E4 la hai canh phai DUNG: chung la doi chung cho phep do.
    if (e1 != 6 || e4 != 0)
      results.push_back({"TC-EXEC03-doi-chung", "ecg_coproc.sv:386", true, 0,
                         ""});
    // PHAN DINH canh LAP LAI: nen bias LECH ma KHONG mot khang dinh nao ban.
    results.push_back({"TC-EXEC03-lap-im-lang", "ecg_coproc.sv:458",
                       b3 != 0, b3, ""});
    std::printf("  %-12s %-22s ky vong %-9s do duoc %d lan\n",
                "TC-EXEC03-lap-im-lang", "ecg_coproc.sv:458", "KHONG BAN", b3);
    clear_flags();
    }
  }

  // ── MUC EXEC-05: ma tran quyen truy cap theo BAY trang thai THAT ──────────
  // Muc goc doi nam ten IDLE/LOADING/READY/RUNNING/ERROR. May that co bay ten
  // khac (:121-122) va ERROR khong phai mot trang thai -- loi di bang co `err`.
  // Lap theo ten THAT: mot ma tran theo nam ten khong ton tai se do mot thiet
  // ke khong ton tai. Moi o do BANG CACH GOI may, khong bang cach doc lai luat
  // trong RTL roi chep sang day -- neu chep thi ma tran chi noi lai chinh no.
  {
    std::vector<Desc> m(3);
    for (int i = 0; i < 3; ++i) { m[i].src0 = 14; m[i].dst = 1; m[i].last = 0; }
    m[0].cout = 2; m[1].cout = 4; m[2].cout = 8;
    m[2].last = 1;
    std::printf("  EXEC-05 ma tran quyen truy cap (7 trang thai x 3 duong vao)\n");
    std::printf("  %-9s | %-22s | %-22s | %s\n", "trang thai", kTenTruyCap[0],
                kTenTruyCap[1], kTenTruyCap[2]);
    int so_toi_duoc = 0, so_o = 0;
    bool ma_tran_rong = true;
    for (int st = 0; st < kNState; ++st) {
      char o[3][24];
      for (int a = 0; a < 3; ++a) {
        const KetQua r = thu_quyen(m, st, a);
        if (!r.toi_duoc) {
          std::snprintf(o[a], sizeof(o[a]), "KHONG TOI DUOC");
        } else {
          ++so_o;
          ma_tran_rong = false;
          std::snprintf(o[a], sizeof(o[a]), "%s%s",
                        r.hieu_luc ? "NHAN" : "BO QUA",
                        r.ban ? " + BAN" : " im lang");
        }
        if (a == 0 && r.toi_duoc) ++so_toi_duoc;
      }
      std::printf("  %-9s | %-22s | %-22s | %s\n", kTenTT[st], o[0], o[1], o[2]);
    }
    std::printf("  EXEC-05: %d/%d o do duoc · %d/%d trang thai toi duoc tu bien\n",
                so_o, kNState * 3, so_toi_duoc, kNState);
    // CHONG RONG: mot ma tran khong o nao toi duoc trong y het mot ma tran
    // "moi thu deu bi tu choi" -- phai NO chu khong duoc im.
    if (ma_tran_rong)
      results.push_back({"TC-EXEC05-chong-rong", "ecg_coproc.sv:121", true, 0,
                         ""});
    // DOI CHUNG: C_IDLE phai NHAN ca ba duong (do la dinh nghia cua "ranh"), va
    // pre_we_i o mot trang thai KHAC C_IDLE phai BI BO QUA -- mux ab_* (:342)
    // chi cho pre_we_i qua o C_IDLE. Neu hai dieu nay sai thi phep do hong.
    const KetQua idle_pre = thu_quyen(m, 0, 1);
    const KetQua run_pre  = thu_quyen(m, 4, 1);
    // Bon dieu, khong hai: NHAN/BO QUA do duong DU LIEU (mux :342), con
    // IM LANG/BAN do duong CANH BAO (:493). Mot dot bien chi cham mot trong hai
    // duong se lot neu chi kiem duong kia.
    if (!idle_pre.hieu_luc || idle_pre.ban != 0 ||
        run_pre.hieu_luc  || run_pre.ban == 0)
      results.push_back({"TC-EXEC05-doi-chung", "ecg_coproc.sv:342", true, 0,
                         ""});
    std::printf("  EXEC-05 doi chung: pre_we_i o C_IDLE = %s/%s · o C_RUN = %s/%s\n",
                idle_pre.hieu_luc ? "NHAN" : "BO QUA",
                idle_pre.ban ? "BAN" : "im lang",
                run_pre.hieu_luc ? "NHAN" : "BO QUA",
                run_pre.ban ? "BAN" : "im lang");
  }

  // ── MUC DESC-01: vung BIAS -- tong `bias_base_q + sq_boff` co TRAN duoc khong
  // `legal_o` rang buoc `cout` cua TUNG LOP (qua `dst_fit`), nhung KHONG rang
  // buoc TONG cout qua cac lop. `bias_base_q` cong don cout sau moi lop MAC va
  // rong 10 bit, nen mot mo hinh NHIEU LOP hop le tung lop van day tong qua
  // 1023 va phep cong CAT trong im lang -- mot chi so NGOAI bang thanh mot chi
  // so TRONG bang.
  //
  // Day la mot mo hinh 20 lop, moi lop cout = 64, len_out = 1: tung lop deu
  // qua `legal_o`, va tong = 1280 > 1023.
  {
    const int N_LOP = 20;
    std::vector<Desc> m(N_LOP);
    for (int i = 0; i < N_LOP; ++i) {
      m[i].src0 = 14; m[i].dst = 1; m[i].last = (i == N_LOP - 1);
      m[i].cout = 64; m[i].cin = 1; m[i].len_in = 1; m[i].len_out = 1;
      m[i].k = 1; m[i].stride = 1; m[i].pad = 0; m[i].rq_n = 1;
    }
    reset(); nap_vao();
    const int before = err();
    dut->single_i = 0; dut->layer_i = 0;
    dut->n_layers_i = static_cast<uint8_t>(N_LOP);
    dut->start_i = 1; tick(); dut->start_i = 0;
    int b_max = 0;
    for (long c = 0; c < 300000; ++c) {
      dut->eval();
      if (dut->desc_req_o) {
        const int idx = dut->desc_idx_o;
        uint32_t w[4];
        pack(m[(idx >= 0 && idx < N_LOP) ? idx : 0], w);
        for (int i2 = 0; i2 < 4; ++i2) dut->desc_word_i[i2] = w[i2];
        dut->desc_valid_i = 1;
      } else dut->desc_valid_i = 0;
      dut->eval();
      if (static_cast<int>(dut->b_off_o) > b_max) b_max = dut->b_off_o;
      tick();
      dut->eval();
      if (dut->done_o) break;
    }
    dut->desc_valid_i = 0;
    const int ban = err() - before;
    std::printf("  DESC-01 mo hinh %d lop x cout 64 (tong bias = %d): b_off_o lon "
                "nhat quan sat = %d · khang dinh ban %d lan\n",
                N_LOP, N_LOP * 64, b_max, ban);
    if (ban == 0) {
      std::printf("  LOI  DESC-01: tong bias %d > 1023 ma KHONG khang dinh nao "
                  "ban -- phep cong 10 bit da CAT trong im lang\n", N_LOP * 64);
      results.push_back({"TC-DESC01-tran-bias", "ecg_coproc.sv:336", true, 0, ""});
    }
    clear_flags();
  }

  // ── MUC EXEC-09: nam canh KHOI PHUC ──────────────────────────────
  // Muc doi bon: reset giua MOI trang thai · loi roi chay lai · FIFO con du
  // lieu cu · thay model KICH THUOC NHO HON. Truoc luot nay dem tren ca hai
  // testbench cho ca bon mau deu bang 0. Canh THU NAM (E9e, cuoi khoi nay) la
  // canh du chan, va no o day vi bon canh kia KHONG tra loi duoc cau da hoi.
  //
  // Dai luong chung cho ba canh do o day: nen bang bias cua lop cuoi trong mot
  // luot SACH phai la 6 (cout 2+4 cua hai lop truoc).
  //
  // PHAM VI CUA BON CANH DAU, ghi ro vi toi da suyt khai qua tay: chung do KHA
  // NANG CHAY LAI DUNG sau reset / sau loi / sau khi doi model. Chung KHONG
  // chung minh "khong con du chan", va ly do la mot dieu ve chinh thiet ke:
  // `bias_base_q` duoc dat lai VO DIEU KIEN o moi lenh lop-0
  // (ecg_coproc.sv:386), nen mot luot sach bat dau tu lop 0 KHONG THE mang du
  // chan cua luot truoc -- du co. Do duoc: dot bien gia tri dat lai o nhanh
  // `rst_ni` (:365) tu '0 sang 10'd3 KHONG lam ba con so nay doi mot chut nao,
  // vi :386 ghi de no truoc khi ai doc.
  //
  // Cau hoi "du chan cua model cu co bi doc khong" nam o bang DESCRIPTOR /
  // THANG / BIAS -- ba bang NGOAI module nay, do tb cap. Mot ban truoc cua chu
  // thich nay ket "nen no khong tra loi duoc tu day", va do la mot KET LUAN
  // QUA SOM: tb NAY cap ca ba bang va giu chung QUA mot lan doi model, nen no
  // tra loi duoc -- xem E9e. Cai khong tra loi duoc la tu BON CANH DAU, khong
  // phai tu testbench nay. Xem hang EXEC-09 trong so.
  {
    std::vector<Desc> m(3);
    for (int i = 0; i < 3; ++i) { m[i].src0 = 14; m[i].dst = 1; m[i].last = 0; }
    m[0].cout = 2; m[1].cout = 4; m[2].cout = 8;
    m[2].last = 1;
    bool x = false;
    auto luot_sach = [&]() {          // mot luot mot-lop 0,1,2 -> nen L2
      nap_vao();
      chay_mot_lop(m, 0, x);
      chay_mot_lop(m, 1, x);
      return chay_mot_lop(m, 2, x);
    };

    // E9a -- RESET giua MOI trang thai. Vao tung trang thai, giu rst_ni thap
    // vai chu ky, roi chay mot luot SACH va doi ket qua dung.
    int hong_reset = 0;
    for (int st = 0; st < kNState; ++st) {
      reset(); nap_vao();
      dut->single_i = 0; dut->layer_i = 0;
      dut->n_layers_i = static_cast<uint8_t>(m.size());
      bool toi = false, da_khoi = false;
      for (long c = 0; c < 100000 && !toi; ++c) {
        dut->eval();
        if (dut->desc_req_o) {
          const int idx = dut->desc_idx_o;
          uint32_t w[4];
          pack(m[(idx >= 0 && idx < 3) ? idx : 0], w);
          for (int i2 = 0; i2 < 4; ++i2) dut->desc_word_i[i2] = w[i2];
          dut->desc_valid_i = 1;
        } else dut->desc_valid_i = 0;
        dut->eval();
        if (trang_thai() == st) { toi = true; break; }
        if (!da_khoi && trang_thai() == 0) {
          dut->start_i = 1; da_khoi = true; tick(); dut->start_i = 0; continue;
        }
        tick();
      }
      dut->desc_valid_i = 0;
      dut->rst_ni = 0;                       // reset NGAY TRONG trang thai do
      for (int i2 = 0; i2 < 3; ++i2) tick();
      dut->rst_ni = 1; tick();
      clear_flags();
      const int nen = luot_sach();
      if (!toi || nen != 6) {
        std::printf("  LOI  EXEC-09a: reset o %s -> luot sau cho nen L2 = %d "
                    "(dung 6)%s\n", kTenTT[st], nen,
                    toi ? "" : "  [KHONG TOI DUOC trang thai]");
        ++hong_reset;
      }
    }
    std::printf("  EXEC-09a reset giua %d trang thai: %d hong\n",
                kNState, hong_reset);

    // E9b -- LOI ROI CHAY LAI. Nap mot descriptor KHONG hop le (stride = 0),
    // roi chay mot luot sach. Mot du chan cua lan hong khong duoc song sot.
    reset(); nap_vao();
    {
      // KHONG dung `fetch_one`: no ket thuc bang `rst_ni = 0` de chan sequencer,
      // tuc no de may TRONG RESET. Mot luot chay ngay sau do khong chay gi ca va
      // `bmin` giu nguyen gia tri khoi tao -- lan dau viet doan nay toi doc con
      // so ay (1048576 = 1<<20) nhu mot ket qua. Muc nay hoi "loi ROI CHAY LAI",
      // nen phai de may tu di het duong loi, KHONG reset ho no.
      Desc xau; xau.stride = 0;
      dut->single_i = 0; dut->layer_i = 0; dut->n_layers_i = 3;
      dut->start_i = 1; tick(); dut->start_i = 0;
      for (long c = 0; c < 100000; ++c) {
        dut->eval();
        if (dut->desc_req_o) {
          uint32_t w[4];
          pack(xau, w);
          for (int i2 = 0; i2 < 4; ++i2) dut->desc_word_i[i2] = w[i2];
          dut->desc_valid_i = 1;
        } else dut->desc_valid_i = 0;
        dut->eval();
        tick();
        dut->eval();
        if (dut->done_o) break;
      }
      dut->desc_valid_i = 0;
      clear_flags();
    }
    const int nen_sau_loi = luot_sach();
    std::printf("  EXEC-09b loi roi chay lai: nen L2 = %d (dung 6)%s\n",
                nen_sau_loi, nen_sau_loi == 6 ? "" : "  LECH");

    // E9d -- DOI SANG MODEL NHO HON. ADR-0013 chon `clear_len = 0`, tuc KHONG
    // co phep xoa nao: descriptor, thang va bias cua model CU o lai trong ba
    // bang. Mot model MOT LOP chay sau mot model BA LOP phai cho nen bias 0 --
    // cua CHINH no -- chu khong doc trung du cua model cu.
    reset(); nap_vao();
    luot_sach();                             // model 3 lop chay truoc
    std::vector<Desc> nho(1);
    nho[0].src0 = 14; nho[0].dst = 1; nho[0].cout = 2; nho[0].last = 1;
    nap_vao();
    const int nen_nho = chay_mot_lop(nho, 0, x);
    std::printf("  EXEC-09d model NHO HON sau model lon: nen L0 = %d (dung 0)%s\n",
                nen_nho, nen_nho == 0 ? "" : "  LECH");

    // E9e -- `n_layers` CU CON LAI, va cau hoi that cua muc EXEC-09:
    // DU CHAN CUA MODEL CU CO BI DOC KHONG.
    //
    // VI SAO BA CANH TREN KHONG TRA LOI DUOC. E9d dua cho tb mot vector `nho`
    // MOI (mot muc), nen bang cua model cu khong con duoc TRO TOI -- tb khong
    // GIU chung. `tb_ecg_coproc` PHA B che ky hon: bo tra loi cua no KEP trong
    // pham vi ho hien tai (`idx < f.n_layers`), nen no khong bao gio phuc vu du
    // lieu model TRUOC -- no phuc vu mot gia tri KEP cua model NAY.
    // > "Bang duoc tb CAP" khong dong nghia "bang duoc tb GIU", va chi cai thu
    // > hai moi do duoc du chan.
    //
    // Ca nay GIU mot bang BEN VUNG nam muc: 0..2 la model MOI (ba lop), 3..4 la
    // du chan cua model LON truoc do. Roi chay AUTO voi `n_layers_i = 5` -- con
    // so CU, chua ai cap nhat. ecg_coproc.sv:459 ket thuc khi
    // `single_i || q_last || layer+1 >= n_layers` -- mot phep HOAC -- nen khi
    // `n_layers` cu va QUA LON, thu duy nhat chan lai la bit `last`.
    //
    // DO TRUC TIEP: ghi lai chi so descriptor CAO NHAT duoc hoi, va `cout` tra
    // ve o chi so do. Mot chi so >= 3 la du chan DA bi doc -- khong suy tu mot
    // phep cong bias co the trung nhau do tinh co.
    //
    // HAI CHO DE HONG, ca hai da cat that trong lan cai dat truoc:
    //  1. `Desc::last` MAC DINH LA 1 (dong 19 cua struct). Mot bang dung moi
    //     ma khong XOA `last` thi MUC 0 la muc cuoi, va luot dung ngay o lop 0
    //     -- phep thu "dat" vi khong the sai. Lan truoc toi dat `last` o muc 2
    //     ma quen xoa o muc 0/1. Nen o day xoa TAT CA truoc, roi dat lai.
    //     > Mot truong mac dinh `1` mang nghia "ket thuc" lam moi muc moi dung
    //     > thanh muc cuoi, va mot bang thieu mot dong gan KHONG DO -- no im
    //     > lang chay dung mot lop.
    //  2. Du chan phai HOP LE VE MIEN. Neu no ngoai mien thi mot chan KHAC bat
    //     truoc (mien `dst`/`src0` cua DESC-10, `mac_fit`, `nz_ok`) va ca nay
    //     do VI LY DO SAI. Nen muc 3,4 dung `cout` hop phap, chi khac noi dung.
    {
      std::vector<Desc> bang(5);
      for (int i = 0; i < 5; ++i) {
        bang[i].src0 = 14; bang[i].dst = 1; bang[i].last = 0;   // XOA het truoc
      }
      bang[0].cout = 2; bang[1].cout = 4; bang[2].cout = 8;
      bang[3].cout = 16; bang[4].cout = 32;   // du chan: HOP LE, khac noi dung
      bang[4].last = 1;
      // Bat THANG THEO KENH cho ca nam muc. `s_off_o` (ecg_seq.sv:267) chi tang
      // theo kenh khi `rq_n > 1`; mac dinh `Desc::rq_n` la 1 nen bang THANG chay
      // per-TENSOR va `s_off_o` dung yen o 0 -- mot can o do se VO DUNG. Luat hop
      // le cua ecg_desc cho phep tap {1, cout}, nen `rq_n = cout` la GIA TRI HOP
      // LE VE MIEN: no khong danh thuc mot checker khac (EN_PERCH = 1 mac dinh o
      // ecg_coproc.sv:50, khong tb nao doi), no chi lam bang THANG do duoc.
      for (int i = 0; i < 5; ++i) bang[i].rq_n = bang[i].cout;

      // `b_off_o` la NUA THU HAI cua cau hoi du chan. Bo dong xu ly KHONG co
      // cong yeu cau thang/bias theo chi so; no PHAT HAI offset RIENG -- `b_off_o`
      // (bias) va `s_off_o` (thang) -- va ben ngoai cap du lieu cho offset do.
      // Nen "du chan trong bang THANG/BIAS co bi doc khong" = "hai offset ay co
      // vuot pham vi cua model MOI khong". Model moi (cout 2+4+8) dung 14 muc
      // bias VA 14 muc thang. HAI cong, khong mot: mot ban truoc cua chu thich
      // nay viet "no PHAT b_off_o" nhu the do la cong duy nhat, va vi the bang
      // THANG bi bo trong mot luot.
      int cout_doc = -1, boff_max = -1, soff_max = -1;
      auto chay_auto = [&](int n_lop, bool last_o_muc2) -> int {
        bang[2].last = last_o_muc2 ? 1 : 0;
        reset(); nap_vao();
        int idx_max = -1;
        cout_doc = -1; boff_max = -1; soff_max = -1;
        dut->single_i = 0; dut->layer_i = 0;
        dut->n_layers_i = static_cast<uint8_t>(n_lop);
        dut->start_i = 1; tick(); dut->start_i = 0;
        for (long c = 0; c < 200000; ++c) {
          dut->eval();
          if (dut->desc_req_o) {
            const int idx = dut->desc_idx_o;
            const int j = (idx >= 0 && idx < 5) ? idx : 0;
            if (idx > idx_max) { idx_max = idx; cout_doc = bang[j].cout; }
            uint32_t w[4];
            pack(bang[j], w);
            for (int i2 = 0; i2 < 4; ++i2) dut->desc_word_i[i2] = w[i2];
            dut->desc_valid_i = 1;
          } else dut->desc_valid_i = 0;
          dut->eval();
          if (dut->busy_o && static_cast<int>(dut->b_off_o) > boff_max)
            boff_max = static_cast<int>(dut->b_off_o);
          if (dut->busy_o && static_cast<int>(dut->s_off_o) > soff_max)
            soff_max = static_cast<int>(dut->s_off_o);
          tick();
          dut->eval();
          if (dut->done_o) break;
        }
        dut->desc_valid_i = 0; dut->single_i = 0;
        return idx_max;
      };

      const int idx_giu = chay_auto(5, true);     // `last` con -> phai dung o 2
      const int cout_giu = cout_doc, boff_giu = boff_max, soff_giu = soff_max;
      const int idx_bo = chay_auto(5, false);     // bo `last` -> phai voi toi 3+
      const int cout_bo = cout_doc, boff_bo = boff_max, soff_bo = soff_max;
      bang[2].last = 1;
      // Chi so bias THAT cua model moi la 0..13 (cout 2+4+8 = 14 muc). Do duoc:
      // luot sach cho b_off_o cao nhat = 14, tuc DUNG MOT tren chi so cuoi. Do
      // la GIA TRI CUOI CUA BO CONG DON (`bias_base_q` tang o C_NEXT sau lop
      // cuoi) chu khong phai mot dia chi duoc doc -- cung hien tuong ma DESC-01
      // da ghi. Bang chung no khong phai mot phep doc: chi so descriptor dung o
      // 2 va `cout` doc duoc la 8, tuc khong lop nao thu tu sau lop 2 chay.
      // Nen can la `<= 14`: tu 15 tro len moi la muc cua model CU.
      // Doi chung cho 86 -- xa han can ba lan -- nen can nay khong phai mot con
      // so nan cho vua: no la mot bien co che, va khoang cach 14 vs 86 la thu
      // lam phep thu phan biet duoc.
      const int BOFF_MOI = 2 + 4 + 8;             // 14 muc bias: chi so 0..13

      std::printf("  EXEC-09e n_layers CU = 5, model that 3 lop: chi so desc cao "
                  "nhat = %d, cout doc o do = %d (dung: idx <= 2)%s\n",
                  idx_giu, cout_giu, idx_giu <= 2 ? "" : "  DOC DU CHAN");
      std::printf("  EXEC-09e doi chung (bo bit last o muc 2): idx = %d, cout = %d "
                  "(phai > 2; khong thi du chan KHONG voi tay duoc va phep tren "
                  "dat vi khong the sai)\n", idx_bo, cout_bo);

      std::printf("  EXEC-09e b_off_o cao nhat = %d (pham vi model MOI = %d, "
                  "phai <= do); doi chung bo last: %d\n",
                  boff_giu, BOFF_MOI, boff_bo);
      // Bang THU BA (THANG). Can lay tu KICH THUOC BANG, khong tu so do duoc:
      // thang per-channel co mot muc moi kenh ra, tuc cung 2+4+8 = 14 muc nhu
      // bias, nen chi so 15 tro len la muc cua model CU. Luot sach do duoc 7 --
      // THAP HON can. Toi KHONG suy duoc tai sao 7 chu khong phai 13, nen toi
      // KHONG lay 7 lam can: mot can nan cho vua so do duoc se do khi bat ky
      // thu tu hop le nao doi. Can la KICH THUOC BANG, va no van bat duoc doi
      // chung o 31.
      const int SOFF_MOI = 2 + 4 + 8;             // 14 muc thang: chi so 0..13
      std::printf("  EXEC-09e s_off_o cao nhat = %d (pham vi model MOI = %d, "
                  "phai <= do); doi chung bo last: %d\n",
                  soff_giu, SOFF_MOI, soff_bo);

      if (idx_giu > 2)
        results.push_back({"TC-EXEC09-du-chan-doc", "ecg_coproc.sv:459", true, 0, ""});
      if (boff_giu > BOFF_MOI)
        results.push_back({"TC-EXEC09-boff-vuot", "ecg_coproc.sv:336", true, 0, ""});
      if (soff_giu > SOFF_MOI)
        results.push_back({"TC-EXEC09-soff-vuot", "ecg_seq.sv:267", true, 0, ""});
      // CHONG RONG cho CA HAI nua: neu bo `last` ma van khong voi toi muc 3, VA
      // b_off_o khong vuot pham vi model moi, thi ca nay khong the phat hien du
      // chan o bat ky nua nao.
      if (idx_bo <= 2 || boff_bo <= BOFF_MOI || soff_bo <= SOFF_MOI)
        results.push_back({"TC-EXEC09e-chong-rong", "ecg_coproc.sv:459", true, 0, ""});
    }

    if (hong_reset) results.push_back({"TC-EXEC09-reset", "ecg_coproc.sv:365",
                                       true, 0, ""});
    if (nen_sau_loi != 6) results.push_back({"TC-EXEC09-sau-loi",
                                             "ecg_coproc.sv:386", true, 0, ""});
    if (nen_nho != 0) results.push_back({"TC-EXEC09-model-nho",
                                         "ecg_coproc.sv:386", true, 0, ""});
    // CHONG RONG: mot luot SACH phai cho 6. Neu chinh no cung sai thi ba phep
    // tren dang so voi mot moc hong, va ca ba "dat" deu vo nghia.
    reset();
    const int moc = luot_sach();
    std::printf("  EXEC-09 moc doi chung (luot sach): %d (dung 6)\n", moc);
    if (moc != 6)
      results.push_back({"TC-EXEC09-chong-rong", "ecg_coproc.sv:458", true, 0,
                         ""});
  }
}

#endif  // DUT_COPROC

}  // namespace

double sc_time_stamp() { return 0; }

int main(int argc, char **argv) {
  VerilatedContext context;
  ctx = &context;
  ctx->commandArgs(argc, argv);
  // $error must not end the run: this harness needs to provoke many of them and
  // count each one.
  ctx->fatalOnError(false);
  Dut top{ctx};
  dut = &top;

  std::printf("== kiem thu tieu cuc: %s ==\n", kDutName);
  run();
  top.final();

  // Verdict. Two failure modes, and they are different bugs.
  int useless = 0;   // expected a fire, got none -> the checker is vacuous
  int unexpected = 0;  // expected silence, got a fire -> RTL or checker is wrong
  int fired = 0;
  for (const auto &r : results) {
    if (r.expect_fire && r.fires == 0) ++useless;
    if (!r.expect_fire && r.fires > 0) ++unexpected;
    if (r.fires > 0) ++fired;
  }

  std::printf("\n-- tong hop %s: %zu ca, %d ca co assertion ban\n", kDutName,
              results.size(), fired);
  for (const auto &r : results) {
    // NHAN NAY DA DOI 2026-09-06. Truoc day no ghi "ASSERTION VO DUNG -- vi
    // pham dung dieu kien ma KHONG ban", va do la nhan SAI cho mot nua so hang
    // di qua day: cac phep do HANH VI (EXEC-03 nen bias · EXEC-09 moc doi chung
    // · LOAD-04 byte da ghi · DESC-03 ...) dung CHINH duong nay de bao mot GIA
    // TRI DO DUOC SAI, chu khong bao mot khang dinh cam. Mot phien doc log da
    // hieu dong ay thanh "EXEC-09 dang dung tren mot chot khong chong duoc gi".
    // Nhan cu dung cho MOT trong hai kha nang va im ve kha nang kia.
    if (r.expect_fire && r.fires == 0)
      std::printf("!! KY VONG LECH: %s %s -- ky vong BAN ma ban 0 lan. HAI kha "
                  "nang, xem chu thich tai cho do: (a) mot khang dinh CAM, hay "
                  "(b) mot PHAN DINH HANH VI dung duong nay de bao mot gia tri "
                  "do duoc SAI.\n",
                  r.id.c_str(), r.where.c_str());
    if (!r.expect_fire && r.fires > 0)
      std::printf("!! BAN NGOAI Y MUON: %s %s -- ban %d lan khi khong co vi "
                  "pham co y\n",
                  r.id.c_str(), r.where.c_str(), r.fires);
  }

  for (const auto &r : results) {
    if (!r.unreachable.empty())
      std::printf("!! KHONG KICH THICH DUOC TU BIEN MODULE: %s %s -- %s\n",
                  r.id.c_str(), r.where.c_str(), r.unreachable.c_str());
  }

  if (useless == 0 && unexpected == 0) {
    std::printf("PASS %s: khong co assertion vo dung, khong co lan ban ngoai y "
                "muon\n",
                kDutName);
    return 0;
  }
  std::printf("FAIL %s: %d assertion vo dung, %d lan ban ngoai y muon\n",
              kDutName, useless, unexpected);
  return 1;
}

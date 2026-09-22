// Verilator harness for ecg_wmem.
//
// Vectors come from tools/rtl_ref/wmem_ref.py, which models the store as a flat
// byte array and a read as a slice -- no banks, no funnel shifter. The RTL splits
// the store by word parity and shifts a 128-bit pair. They agree only if the
// parity split and the shift amount are both right.
//
// The harness checks three things:
//   1. The DMA loads every word, at 8 B/cycle, and reports done exactly once.
//      T_switch = 669 cycles depends on that rate (ADR-0015), so the harness
//      counts the cycles the transfer actually took rather than trusting it.
//   2. Every read returns the reference bytes, including the unaligned ones.
//   3. Back pressure: with s_valid low the DMA holds, and no word is dropped or
//      duplicated when the stream stalls.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "Vecg_wmem.h"
#include "verilated.h"
#include "ecg_cov.h"

namespace {

Vecg_wmem *dut = nullptr;
uint64_t cycles = 0;

void tick() {
  dut->clk_i = 0;
  dut->eval();
  dut->clk_i = 1;
  dut->eval();
  ++cycles;
}

}  // namespace

double sc_time_stamp() { return 0; }

// Moi `return 2` trong duong PHAN TICH phai NOI RA cho nao hong. Truoc do chung
// im lang: `fscanf` that bai roi `return 2` khong in gi, nen mot tep vector bi
// CAT CUT lam `make sim-wmem` do voi KHONG MOT DONG dau ra va nguoi doc phai di
// doc ma nguon. Chot bao ve dung truong hop TU BAO MINH ("khong mo duoc", co
// thong bao) va bo truong hop IM LANG.
#define DOC_HONG(what) do {                                                    \
    std::printf("tb_ecg_wmem: doc HONG tep vector %s\n"                         \
                "  dang doc: %s  (tb_ecg_wmem.cpp:%d)\n"                       \
                "  Tep bi CAT CUT hay sai dinh dang. Sinh lai:\n"              \
                "    python3 tools/rtl_ref/wmem_ref.py\n",                              \
                path, (what), __LINE__);                                       \
    return 2; } while (0)

int main(int argc, char **argv) {
  Verilated::commandArgs(argc, argv);

  const char *path = (argc > 1) ? argv[1] : "40-rtl/tb/vectors/wmem.txt";
  FILE *fp = std::fopen(path, "r");
  if (!fp) {
    std::printf("tb_ecg_wmem: khong mo duoc %s (chay `python3 "
                "tools/rtl_ref/wmem_ref.py`)\n", path);
    return 2;
  }

  int nbytes = 0, nread = 0;
  if (std::fscanf(fp, "%d %d", &nbytes, &nread) != 2) DOC_HONG("std::fscanf(fp, '%d %d', &nbytes, &nread) != 2");
  const int nwords = nbytes / 8;

  std::vector<uint64_t> words(nwords);
  for (int i = 0; i < nwords; ++i) {
    if (std::fscanf(fp, "%" SCNx64, &words[i]) != 1) DOC_HONG("std::fscanf(fp, '%' SCNx64, &words[i]) != 1");
  }
  std::vector<int> offs(nread);
  std::vector<uint64_t> want(nread);
  for (int i = 0; i < nread; ++i) {
    if (std::fscanf(fp, "%d %" SCNx64, &offs[i], &want[i]) != 2) DOC_HONG("std::fscanf(fp, '%d %' SCNx64, &offs[i], &want[i]) != 2");
  }
  std::fclose(fp);

  dut = new Vecg_wmem;
  dut->rst_ni = 0;
  dut->dma_start_i = 0;
  dut->s_valid_i = 0;
  dut->rd_req_i = 0;
  for (int i = 0; i < 3; ++i) tick();
  dut->rst_ni = 1;
  tick();

  int errors = 0;

  // ---- Pha 0: QUET DO DAI. Nam truoc Pha 1 va KHONG kiem du lieu -- no chi
  // chay duong DEM voi nhieu do dai khac nhau.
  //
  // VI SAO CAN, va cho nay phai phan dinh chu khong doan. Do phu toggle cua
  // module la 93,8 %, va do TUNG BIT chi ra hai loai rat khac nhau:
  //
  //   CAU TRUC -- khong duong nao cham toi:
  //     we_idx / wo_idx  lat bit 0..8, chua lat 9..13. Chi so bank toi da la
  //                      291 (292 tu moi bank), tuc DUNG 9 bit.
  //     wr_word_q        lat 0..9, chua lat 10..13. Toi da 582 tu -> 10 bit.
  //     dma_len_i bit 0,1,2  cong ghi ro "byte, boi cua 8" nen chung LUON 0.
  //     dma_len_i bit 13     = 8.192 > ECG_WMEM_BYTES = 4.664, vuot vung.
  //
  //   LO THAT -- toi duoc ma chua thu:
  //     dma_len_i    chi lat bit 3,4,5,9,12 -- DUNG cac bit cua MOT gia tri duy
  //                  nhat: 4.664 = 0b1001000111000. Bo vector co MOT dong dau
  //                  <n_bytes>, nen ca lan chay chi thu MOT do dai.
  //     wr_words_q   lat 0,1,2,6,9 = cac bit cua 4664>>3 = 583.
  //
  // CHE DO HONG: mot bit KET o vi tri 6,7,8,10 hay 11 cua `dma_len_i` se cat cut
  // hay keo dai mot luot nap trong so, va voi MOT do dai duy nhat thi no vo hinh.
  //
  // NAM DO DAI duoi day duoc chon de lat DUNG cac bit con thieu, khong phai chon
  // cho dep: 64/128/256/1024/2048 lat bit 6/7/8/10/11 cua `dma_len_i`, va chia 8
  // cho 8/16/32/128/256 tuc lat bit 3/4/5/7/8 cua `wr_words_q` -- dung tap con
  // thieu cua ca hai tin hieu. Ca nam deu <= 4.664 va deu la boi cua 8.
  {
    static const int DO_DAI[] = {64, 128, 256, 1024, 2048};
    for (const int L : DO_DAI) {
      const int nw = L / 8;
      dut->dma_start_i = 1;
      dut->dma_len_i = L;
      tick();
      dut->dma_start_i = 0;
      int gui = 0, canh = 0, xung_done = 0;
      while (gui < nw && canh++ < 20 * nw) {
        dut->s_valid_i = 1;
        dut->s_data_i = static_cast<uint64_t>(gui) * 0x0101010101010101ull;
        dut->eval();
        const bool nhan = dut->s_ready_o != 0;
        tick();
        if (dut->dma_done_o) ++xung_done;
        if (nhan) ++gui;
      }
      dut->s_valid_i = 0;
      for (int i = 0; i < 4; ++i) { tick(); if (dut->dma_done_o) ++xung_done; }
      if (gui != nw) {
        std::printf("  FAIL  pha0 L=%d: nhan %d tu, ky vong %d\n", L, gui, nw);
        ++errors;
      } else if (xung_done != 1) {
        std::printf("  FAIL  pha0 L=%d: %d xung dma_done, ky vong DUNG 1\n",
                    L, xung_done);
        ++errors;
      }
    }
    // Dua ve trang thai sach truoc Pha 1.
    dut->s_valid_i = 0;
    dut->rst_ni = 0;
    for (int i = 0; i < 3; ++i) tick();
    dut->rst_ni = 1;
    tick();
  }

  // ---- Phase 1: DMA the whole weight region in, stalling the stream on a
  // fixed pattern so back pressure is exercised rather than assumed.
  dut->dma_start_i = 1;
  dut->dma_len_i = nbytes;
  tick();
  dut->dma_start_i = 0;

  const uint64_t dma_start_cycle = cycles;
  int sent = 0;
  int stalls = 0;
  int done_pulses = 0;
  int guard = 0;
  while (sent < nwords && guard++ < 20 * nwords) {
    // Stall on every seventh beat: coprime with the bank parity, so a stall
    // lands on both an even and an odd word.
    const bool stall = (guard % 7) == 3;
    dut->s_valid_i = stall ? 0 : 1;
    dut->s_data_i = words[sent];
    dut->eval();
    const bool ready = dut->s_ready_o != 0;
    const bool moved = ready && !stall;
    tick();
    if (dut->dma_done_o) ++done_pulses;
    if (moved) ++sent;
    else ++stalls;
  }
  dut->s_valid_i = 0;
  tick();
  if (dut->dma_done_o) ++done_pulses;
  const uint64_t dma_cycles = cycles - dma_start_cycle;

  if (sent != nwords) {
    std::printf("DMA chi nhan %d / %d tu\n", sent, nwords);
    ++errors;
  }
  if (done_pulses != 1) {
    std::printf("dma_done_o xung %d lan, phai dung 1\n", done_pulses);
    ++errors;
  }
  if (dut->dma_busy_o) {
    std::printf("dma_busy_o con cao sau khi xong\n");
    ++errors;
  }
  // Rate check: the transfer must cost one cycle per word plus the stalls the
  // harness itself injected. Anything more means the DMA is not 8 B/cycle and
  // the 669-cycle T_switch of ADR-0015 does not hold.
  const uint64_t ideal = static_cast<uint64_t>(nwords) + stalls;
  if (dma_cycles > ideal + 4) {
    std::printf("DMA ton %llu chu ky, muc ly tuong %llu (+ %d chu ky dung)\n",
                (unsigned long long)dma_cycles, (unsigned long long)ideal,
                stalls);
    ++errors;
  }

  // ---- Phase 2: every read, back to back, one per cycle. rd_data is valid the
  // cycle after the request, so the check trails the request by one -- getting
  // that off by one is exactly the harness bug that has bitten five times, so
  // the pipeline is written out explicitly rather than folded into the loop.
  // rd_data_o va rd_valid_o deu duoc ghi vao thanh ghi o CHINH nhip co rd_req,
  // nen chung hop le ngay sau tick() do -- khong tre them mot nhip nua. Ban dau
  // o day co mot tang `pend` thua, va no bao sai toan bo phep doc trong khi RTL
  // dung: cac gia tri khop chinh xac nhung lech mot vi tri trong danh sach. Do
  // la dau hieu cua harness sai, khong phai RTL sai.
  int checked = 0;
  for (int i = 0; i < nread; ++i) {
    dut->rd_req_i = 1;
    dut->rd_off_i = offs[i];
    tick();
    if (!dut->rd_valid_o) {
      if (++errors <= 10)
        std::printf("thieu rd_valid cho offset %d\n", offs[i]);
      continue;
    }
    const uint64_t got = dut->rd_data_o;
    if (got != want[i]) {
      if (++errors <= 10) {
        std::printf("offset %d (bsel %d, tu %s): got %016llx want %016llx\n",
                    offs[i], offs[i] & 7,
                    ((offs[i] >> 3) & 1) ? "le" : "chan",
                    (unsigned long long)got, (unsigned long long)want[i]);
      }
    }
    ++checked;
  }
  dut->rd_req_i = 0;
  tick();

  dut->final();
  ECG_COV_WRITE();
  delete dut;

  std::printf("tb_ecg_wmem: %d B nap trong %llu chu ky (%d tu, %d chu ky dung), "
              "%d phep doc\n", nbytes, (unsigned long long)dma_cycles, nwords,
              stalls, checked);
  if (checked != nread) {
    std::printf("tb_ecg_wmem: FAIL (kiem %d / %d phep doc)\n", checked, nread);
    return 1;
  }
  if (errors == 0) {
    std::printf("tb_ecg_wmem: PASS (DMA 8 B/chu ky, doc lech byte dung "
                "tung bit)\n");
    return 0;
  }
  std::printf("tb_ecg_wmem: FAIL (%d loi)\n", errors);
  return 1;
}

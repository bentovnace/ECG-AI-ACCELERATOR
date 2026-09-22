// Verilator harness for ecg_sram_macro.
//
// What is actually at risk here. `ecg_sram_macro` has to be indistinguishable
// from `ecg_sram_1r1w` at the port, because the sequencer's one-read-per-cycle
// issue rate and the whole cycle budget are built on that memory's timing. The
// things that can silently break it are all address arithmetic: the bank decode,
// the byte lane inside a macro word, the write mask, and the one-cycle read
// latency. None of those show up as a compile error, and all of them produce a
// memory that works for most addresses.
//
// So the reference is an INDEPENDENT model: a flat std::vector of words with no
// notion of banks, lanes or masks, written in the four lines the specification
// takes to state. It cannot share a mistake with the RTL, because it does not
// contain the concepts the RTL can get wrong. It also encodes the two timing
// rules explicitly -- read data appears one cycle after the request, and holds
// while `re_i` is low -- so a latency slip is a mismatch, not a silent pass.
//
// Phases (all compared against that model, every cycle):
//   1. write the whole address range, then read it all back;
//   2. write and read in the SAME cycle at different addresses -- the only reason
//      a 1r1w memory exists, and the case a single-port mapping would break;
//   3. bank and lane boundaries: every multiple of 256 (the smallest capacity any
//      macro in the plan can have) probed at -2, -1, +0, +1, so no boundary the
//      planner can produce goes unprobed, whatever the plan chose;
//   4. the last legal address; and, in a separate run, a deliberate access one
//      past DEPTH. That one needs two runs and both are checks: with the range
//      assertion compiled in the simulation must STOP (the address is illegal and
//      the memory has to say so), and with it compiled out the access must not
//      alias onto any live byte. `Verilated::assertOn` cannot do this -- Verilator
//      emits $error unconditionally -- so the phase is selected by --phase=oob and
//      the Makefile runs it against both builds;
//   5. hold: `re_i` low must not disturb the last read data, INCLUDING when the
//      address that was read is written during the idle window. That last case
//      caught a real bug: the first version held by replaying the read address
//      into the macro, which returns the NEW word once someone writes it;
//   6. 100.000+ pseudo-random accesses, fixed seed, so the number is reproducible
//      rather than "it ran for a while".
//
// Read-during-write at the SAME address is not tested and must not be: a 1rw1r
// macro has no defined answer for it, `ecg_sram_1r1w` already forbids it with an
// assertion, and so does the replacement. Same macro row at DIFFERENT byte
// addresses is not only tested but unavoidable in phases 2 and 6, and phase 6
// aims at it deliberately; the count is printed at the end.
//
// WIDTH and DEPTH come in as -DTB_WIDTH/-DTB_DEPTH and must match the -G values
// given to verilator; the check below refuses to run if they cannot both be true.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include "Vecg_sram_macro.h"
#include "verilated.h"
#include "ecg_cov.h"

#ifndef TB_WIDTH
#error "TB_WIDTH must be defined and must match -GWIDTH"
#endif
#ifndef TB_DEPTH
#error "TB_DEPTH must be defined and must match -GDEPTH"
#endif

namespace {

constexpr uint32_t WIDTH = TB_WIDTH;
constexpr uint32_t DEPTH = TB_DEPTH;
constexpr uint64_t DMASK = (WIDTH >= 64) ? ~0ULL : ((1ULL << WIDTH) - 1);
// Words per row of the widest macro (128 b). Two addresses that differ only in
// these bits are in the same macro row, which is the read-during-write case that
// a naive banking gets wrong and that this harness has to hit on purpose.
constexpr uint32_t ROW_MASK = (128 / WIDTH) - 1;

Vecg_sram_macro *dut = nullptr;

// The reference: nothing but storage.
std::vector<uint64_t> ref;

// One cycle. The rising edge first, then the falling edge, because the macro
// registers its inputs at the rising edge and drives dout1 at the falling one
// (its .lib read arc is falling_edge). Sampling before the falling edge would
// read the previous cycle's data and every phase would fail by one.
void tick() {
  dut->clk_i = 1;
  dut->eval();
  dut->clk_i = 0;
  dut->eval();
}

uint64_t errors = 0;
uint64_t vectors = 0;

// Expected read data, carried across cycles so that a cycle with re_i low checks
// the HOLD rather than skipping the comparison.
bool     exp_known = false;
uint64_t exp_data  = 0;

// One cycle with the given stimulus, plus the comparison. `check` is false only
// while the array is still being filled and a read would return uninitialised
// memory.
void step(bool re, uint32_t raddr, bool we, uint32_t waddr, uint64_t wdata,
          bool check, const char *phase) {
  // The reference reads BEFORE it writes: ecg_sram_1r1w evaluates the memory on
  // the right-hand side of a non-blocking assignment, so a same-address
  // read-during-write would return the old word. That case is forbidden, but the
  // model states the rule anyway rather than leaving the order to chance.
  const bool     have_old = re && (raddr < DEPTH);
  const uint64_t old      = have_old ? ref[raddr] : 0;
  if (we && (waddr < DEPTH)) ref[waddr] = wdata & DMASK;

  dut->re_i    = re;
  dut->raddr_i = raddr;
  dut->we_i    = we;
  dut->waddr_i = waddr;
  dut->wdata_i = wdata & DMASK;
  tick();

  if (re) {
    exp_data  = old;
    exp_known = have_old;
  }
  vectors++;

  if (check && exp_known) {
    const uint64_t got = static_cast<uint64_t>(dut->rdata_o) & DMASK;
    if (got != exp_data) {
      if (errors < 20)
        printf("MISMATCH [%s] cycle %llu: re=%d raddr=%u we=%d waddr=%u "
               "got=0x%llx want=0x%llx\n",
               phase, (unsigned long long)vectors, (int)re, raddr, (int)we, waddr,
               (unsigned long long)got, (unsigned long long)exp_data);
      errors++;
    }
  }
}

void idle(bool check, const char *phase) {
  step(false, 0, false, 0, 0, check, phase);
}

// A value that depends on the address in every bit, so a lane or bank mix-up
// cannot return a byte that happens to be right.
uint64_t pattern(uint32_t addr, uint32_t salt) {
  uint64_t v = 0x9e3779b97f4a7c15ULL * (addr + 1) + 0x1234567ULL * (salt + 1);
  v ^= v >> 29;
  return v & DMASK;
}

}  // namespace

double sc_time_stamp() { return 0; }

int main(int argc, char **argv) {
  Verilated::commandArgs(argc, argv);

  bool oob_phase = false;
  for (int i = 1; i < argc; i++)
    if (std::string(argv[i]) == "--phase=oob") oob_phase = true;

  printf("tb_ecg_sram_macro: WIDTH=%u DEPTH=%u phase=%s\n", WIDTH, DEPTH,
         oob_phase ? "oob" : "main");
  ref.assign(DEPTH, 0);

  dut = new Vecg_sram_macro;
  dut->clk_i   = 0;
  dut->re_i    = 0;
  dut->raddr_i = 0;
  dut->we_i    = 0;
  dut->waddr_i = 0;
  dut->wdata_i = 0;
  dut->eval();

  // ---- phase 1: fill, then read everything back. Both phases need the fill.
  for (uint32_t a = 0; a < DEPTH; a++)
    step(false, 0, true, a, pattern(a, 0), false, "fill");
  exp_known = false;
  for (uint32_t a = 0; a < DEPTH; a++)
    step(true, a, false, 0, 0, true, "readback");

  if (oob_phase) {
    if ((DEPTH & (DEPTH - 1)) == 0) {
      printf("DEPTH is a power of two: no address outside the array is "
             "representable, nothing to probe -- PASS\n");
      dut->final();
      ECG_COV_WRITE();
  delete dut;
      return 0;
    }
    printf("--- deliberate access at %u, one past the last legal address\n", DEPTH);
    const std::vector<uint64_t> snapshot = ref;
    step(true, DEPTH, true, DEPTH, ~0ULL, false, "oob");
    exp_known = false;
    for (uint32_t a = 0; a < DEPTH; a++)
      step(true, a, false, 0, 0, true, "oob-sweep");
    if (snapshot != ref) {
      printf("HARNESS BUG: reference changed during the out-of-range phase\n");
      errors++;
    }
    printf("tb_ecg_sram_macro: %llu vectors, %llu mismatches after an "
           "out-of-range access -- %s\n",
           (unsigned long long)vectors, (unsigned long long)errors,
           errors == 0 ? "PASS" : "FAIL");
    dut->final();
    ECG_COV_WRITE();
  delete dut;
    return errors == 0 ? 0 : 1;
  }

  // ---- phase 2: write and read in the same cycle, different addresses. The
  // offset walks so that the pair lands in the same macro row sometimes and in
  // different banks other times.
  for (uint32_t off = 1; off <= 5; off++) {
    for (uint32_t a = 0; a + off < DEPTH; a++)
      step(true, a, true, a + off, pattern(a + off, off), true, "rw-same-cycle");
    // The write above changed bytes the next pass will read; the reference
    // followed it, so nothing to re-sync -- that is the point of comparing every
    // cycle instead of only at the end.
  }

  // ---- phase 3: boundaries. 256 words is the smallest capacity any macro in
  // the plan can contribute, so every possible bank boundary is a multiple of it.
  for (uint32_t base = 0; base <= DEPTH; base += 256) {
    for (int d = -2; d <= 1; d++) {
      const int64_t a = (int64_t)base + d;
      if (a < 0 || a >= (int64_t)DEPTH) continue;
      const uint32_t ua = (uint32_t)a;
      step(false, 0, true, ua, pattern(ua, 7), true, "boundary-write");
      step(true, ua, false, 0, 0, true, "boundary-read");
    }
  }

  // ---- phase 4: last legal address, then one past DEPTH.
  step(false, 0, true, DEPTH - 1, pattern(DEPTH - 1, 9), true, "last-write");
  step(true, DEPTH - 1, false, 0, 0, true, "last-read");

  // ---- phase 5: hold.
  const uint32_t hold_addr = DEPTH / 3;
  step(true, hold_addr, false, 0, 0, true, "hold-read");
  for (int i = 0; i < 8; i++) idle(true, "hold-idle");
  // Writes during the idle window, INCLUDING one to the address that was read.
  // The behavioural memory keeps the old word on its output; anything that holds
  // by re-reading the array does not, which is how the first version failed.
  for (int i = 0; i < 8; i++)
    step(false, 0, true, (hold_addr + i) % DEPTH, pattern(i, 11), true,
         "hold-write");
  for (int i = 0; i < 4; i++) idle(true, "hold-idle2");

  // ---- phase 6: pseudo-random, fixed seed.
  std::mt19937 rng(0x5A5A1234u);   // fixed seed: the vector count must be reproducible
  std::uniform_int_distribution<uint32_t> addr(0, DEPTH - 1);
  std::uniform_int_distribution<uint32_t> coin(0, 99);
  const uint32_t N_RAND = 100000;
  uint32_t same_row = 0;
  for (uint32_t n = 0; n < N_RAND; n++) {
    const bool     re = coin(rng) < 90;   // the real duty cycle is near 100 %
    const bool     we = coin(rng) < 70;
    uint32_t       ra = addr(rng);
    const uint32_t wa = addr(rng);
    // Sometimes deliberately aim the read at the same 16-byte macro row as the
    // write, which is the case a naive banking gets wrong.
    if (we && (coin(rng) < 25)) {
      const uint32_t cand = (wa & ~ROW_MASK) | (coin(rng) & ROW_MASK);
      if (cand < DEPTH) ra = cand;
    }
    if (re && we && (ra == wa)) continue;   // forbidden by both memories
    if (re && we && ((ra ^ wa) & ~ROW_MASK) == 0) same_row++;
    step(re, ra, we, wa, pattern(wa, n), true, "random");
  }

  const bool ok = (errors == 0);
  printf("tb_ecg_sram_macro: %llu vectors, %u random accesses, %u of them a "
         "read and a write in the same macro row, %llu mismatches -- %s\n",
         (unsigned long long)vectors, N_RAND, same_row,
         (unsigned long long)errors, ok ? "PASS" : "FAIL");

  dut->final();
  ECG_COV_WRITE();
  delete dut;
  return ok ? 0 : 1;
}

// Verilator harness for ecg_seq (MAC path).
//
// The expectation comes from tools/rtl_ref/seq_ref.py: a conv1d computed in
// numpy, requantised by int_ref.requant -- the same function IntRunner and the
// blob interpreter use, so the integer path is the one frozen in P4 -- and write
// offsets derived from the spec formula dst_off + oc*len_out + out_pos. Nothing
// in the reference models the sequencer's schedule, so agreement is evidence
// about the layer, not about the loop nest.
//
// The harness models the four memories the sequencer talks to. Two details in
// there are worth naming because getting either wrong would blame the RTL:
//
//   * Read latency is one cycle and the harness must present data on the cycle
//     AFTER the request, with the valid flag, exactly as ecg_actbuf and ecg_wmem
//     do. Presenting it in the same cycle makes the accumulator see the next
//     operand and every result is wrong by one term.
//   * The scale and bias tables have DIFFERENT indices: s_off_o follows rq_n
//     (one entry for the whole layer when per-tensor), b_off_o always follows the
//     output channel. Wiring both to s_off_o passes for per-channel layers and
//     fails only for per-tensor ones.
//
// Writes are collected as (offset, value) and compared as a multiset against the
// reference, because the order the sequencer emits them in is its own business.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <vector>

#include "Vecg_seq.h"
#include "verilated.h"
#include "ecg_cov.h"

// Ghi VCD, CHI khi bien dich voi -DECG_TRACE. Ban mac dinh khong doi mot byte nao:
// dau vet lam mo phong cham nhieu lan va mo phong nay chay 73 lop that.
//
// Vi sao can VCD: `report_power` cua OpenROAD khong biet tin hieu doi bao nhieu lan
// nen no dung mot hoat do MAC DINH. Mot con so cong suat dua tren hoat do mac dinh
// khong phai mot phep do -- no la mot phep nhan. VCD nay cho hoat do THAT do tu 73
// lop chay bang trong so that.
#ifdef ECG_TRACE
#include "verilated_vcd_c.h"
static VerilatedVcdC *tfp = nullptr;
static vluint64_t vtime = 0;
#endif

namespace {

constexpr int N_PE = 8;

Vecg_seq *dut = nullptr;

void tick() {
  dut->clk_i = 0;
  dut->eval();
#ifdef ECG_TRACE
  if (tfp) tfp->dump(vtime++);
#endif
  dut->clk_i = 1;
  dut->eval();
#ifdef ECG_TRACE
  if (tfp) tfp->dump(vtime++);
#endif
}

struct Case {
  int op, cin, cout, k, stride, pad, len_in, len_out, dw, relu, per_ch,
      dst_off;
  std::vector<int> x, w, bias;
  std::vector<std::pair<int, int>> sc;      // (mult, shift)
  std::vector<std::pair<int, int>> writes;  // (offset, value)
};

bool read_case(FILE *fp, Case &c) {
  if (std::fscanf(fp, "%d %d %d %d %d %d %d %d %d %d %d %d", &c.op, &c.cin,
                  &c.cout, &c.k, &c.stride, &c.pad, &c.len_in, &c.len_out,
                  &c.dw, &c.relu, &c.per_ch, &c.dst_off) != 12) {
    return false;
  }
  auto rdvec = [&](std::vector<int> &v) {
    int n = 0;
    if (std::fscanf(fp, "%d", &n) != 1) return false;
    v.resize(n);
    for (int i = 0; i < n; ++i) {
      if (std::fscanf(fp, "%d", &v[i]) != 1) return false;
    }
    return true;
  };
  if (!rdvec(c.x) || !rdvec(c.w)) return false;
  int n = 0;
  if (std::fscanf(fp, "%d", &n) != 1) return false;
  c.sc.resize(n);
  for (int i = 0; i < n; ++i) {
    if (std::fscanf(fp, "%d %d", &c.sc[i].first, &c.sc[i].second) != 2) {
      return false;
    }
  }
  if (!rdvec(c.bias)) return false;
  if (std::fscanf(fp, "%d", &n) != 1) return false;
  c.writes.resize(n);
  for (int i = 0; i < n; ++i) {
    if (std::fscanf(fp, "%d %d", &c.writes[i].first,
                    &c.writes[i].second) != 2) {
      return false;
    }
  }
  return true;
}

}  // namespace

double sc_time_stamp() { return 0; }

// Moi `return 2` trong duong PHAN TICH phai NOI RA cho nao hong. Truoc do chung
// im lang: `fscanf` that bai roi `return 2` khong in gi, nen mot tep vector bi
// CAT CUT lam `make sim-seq` do voi KHONG MOT DONG dau ra va nguoi doc phai di
// doc ma nguon. Chot bao ve dung truong hop TU BAO MINH ("khong mo duoc", co
// thong bao) va bo truong hop IM LANG.
#define DOC_HONG(what) do {                                                    \
    std::printf("tb_ecg_seq: doc HONG tep vector %s\n"                         \
                "  dang doc: %s  (tb_ecg_seq.cpp:%d)\n"                       \
                "  Tep bi CAT CUT hay sai dinh dang. Sinh lai:\n"              \
                "    python3 tools/rtl_ref/seq_ref.py\n",                              \
                path, (what), __LINE__);                                       \
    return 2; } while (0)

int main(int argc, char **argv) {
  Verilated::commandArgs(argc, argv);

  const char *path = (argc > 1) ? argv[1] : "40-rtl/tb/vectors/seq.txt";
  FILE *fp = std::fopen(path, "r");
  if (!fp) {
    std::printf("tb_ecg_seq: khong mo duoc %s (chay `python3 "
                "tools/rtl_ref/seq_ref.py`)\n", path);
    return 2;
  }
  int ncase = 0;
  if (std::fscanf(fp, "%d", &ncase) != 1) DOC_HONG("std::fscanf(fp, '%d', &ncase) != 1");

  dut = new Vecg_seq;
#ifdef ECG_TRACE
  Verilated::traceEverOn(true);
  tfp = new VerilatedVcdC;
  dut->trace(tfp, 99);
  const char *vcd = (argc > 2) ? argv[2] : "70-asic/build/ecg_seq.vcd";
  tfp->open(vcd);
#endif
  dut->rst_ni = 0;
  dut->start_i = 0;
  for (int i = 0; i < 3; ++i) tick();
  dut->rst_ni = 1;
  tick();

  int errors = 0, n_case = 0, n_write = 0;
  long total_cycles = 0, total_model = 0;

  for (int ci = 0; ci < ncase; ++ci) {
    Case c;
    if (!read_case(fp, c)) {
      std::printf("tb_ecg_seq: ca thu %d hong\n", ci);
      return 2;
    }
    ++n_case;

    dut->op_i = static_cast<uint8_t>(c.op);
    dut->act_i = static_cast<uint8_t>(c.relu ? 1 : 0);
    dut->src0_i = 0;
    dut->dst_i = 1;
    dut->dst_off_i = static_cast<uint16_t>(c.dst_off);
    dut->cin_i = static_cast<uint16_t>(c.cin);
    dut->cout_i = static_cast<uint16_t>(c.cout);
    dut->len_in_i = static_cast<uint16_t>(c.len_in);
    dut->len_out_i = static_cast<uint16_t>(c.len_out);
    dut->k_i = static_cast<uint8_t>(c.k);
    dut->stride_i = static_cast<uint8_t>(c.stride);
    dut->pad_i = static_cast<uint8_t>(c.pad);
    dut->dw_i = static_cast<uint8_t>(c.dw);
    // BASE KHAC 0, VA DOI THEO TUNG CA. Truoc day hai dong nay chot cung 0, va
    // do phu do duoc noi dung dieu do: `w_base_i`, `w_base_q`, `rq_base_i`,
    // `rq_base_q` deu CHUA CHAM 100 % -- 110 diem toggle, 24 % cua phan chua phu
    // cua module.
    //
    // CHE DO HONG ma mot base = 0 KHONG THE bat: neu `w_base_q` bi ROT khoi
    // `w_off_o = w_base_q + w_row_off + ag_out_ch` (dong 326 cua ecg_seq.sv) thi
    // voi base = 0 dia chi ra Y HET, va 73 ca van dung tung bit. Mot duong bi rot
    // chi lo ra khi base khac 0.
    //
    // VA LOP NAY DA CAN CHINH MODULE NAY ROI: chu thich o ecg_seq.sv:116-118 ghi
    // "ban dau o day dung dst_off TRUC TIEP, va bo vector luon dat dst_off = 0 nen
    // phep kiem khong phan biet duoc". Cung mot hinh dang, mot truong khac.
    //
    // CACH KIEM: tb tra mang cua no bang chi so DA TRU BASE. Neu RTL cong base
    // dung thi hai phep triet tieu va ket qua khong doi; neu RTL ROT base thi chi
    // so lech va ket qua sai. Nen phep thu nay KHONG doi ky vong -- no chi lam
    // duong dia chi phai that su mang base.
    //
    // CHAN TREN CUA BASE la mot rang buoc THAT, khong mot con so tuy y:
    // `w_off_o` rong WBITS = 13 bit (max 8.191) va vung trong so la
    // ECG_WMEM_BYTES = 4.664, nen base phai < 8.192 - 4.664 = 3.528 de tong khong
    // TRAN. Tran se lam phep tru cua tb ra chi so sai va phep thu do vi mot ly do
    // KHONG phai loi cua thiet ke. Nen base cao nhat o day la 2.048.
    // `s_off_o` rong 14 bit va bang thang nho hon nhieu, nen 4.096 an toan.
    const int W_BASE  = (ci % 13) ? (1 << (ci % 12)) : 0;   // 0, 2, 4, ... 2048
    const int RQ_BASE = (ci % 11) ? (1 << (ci % 13)) : 0;   // 0, 2, 4, ... 4096
    dut->w_base_i = static_cast<uint16_t>(W_BASE);
    dut->rq_base_i = static_cast<uint16_t>(RQ_BASE);
    dut->rq_n_i = static_cast<uint16_t>(c.per_ch ? c.cout : 1);

    dut->start_i = 1;
    dut->a_gnt_i = 1;
    dut->a_valid_i = 0;
    dut->w_valid_i = 0;
    tick();
    dut->start_i = 0;

    std::map<std::pair<int, int>, int> got;
    long cycles = 0;
    bool pend_a = false, pend_w = false;
    int pend_a_off = 0, pend_w_off = 0;
    const long cap = 200000;

    while (cycles < cap) {
      // Present the data requested LAST cycle, then latch this cycle's request.
      if (pend_a) {
        dut->a_valid_i = 1;
        dut->a_data_i = (pend_a_off >= 0 &&
                         pend_a_off < static_cast<int>(c.x.size()))
                            ? static_cast<int8_t>(c.x[pend_a_off]) : 0;
      } else {
        dut->a_valid_i = 0;
        dut->a_data_i = 0;
      }
      if (pend_w) {
        dut->w_valid_i = 1;
        for (int j = 0; j < N_PE; ++j) {
          const int idx = pend_w_off + j - W_BASE;
          dut->w_data_i[j] = (idx >= 0 && idx < static_cast<int>(c.w.size()))
                                 ? static_cast<int8_t>(c.w[idx]) : 0;
        }
      } else {
        dut->w_valid_i = 0;
        for (int j = 0; j < N_PE; ++j) dut->w_data_i[j] = 0;
      }

      // Scale and bias are combinational lookups, two different indices.
      dut->eval();
      {
        const int so = dut->s_off_o - RQ_BASE;
        const int bo = dut->b_off_o;
        const int si = (so >= 0 && so < static_cast<int>(c.sc.size())) ? so : 0;
        dut->s_mult_i = static_cast<uint16_t>(c.sc[si].first);
        dut->s_shift_i = static_cast<uint8_t>(c.sc[si].second);
        dut->s_bias_i = static_cast<int8_t>(
            (bo >= 0 && bo < static_cast<int>(c.bias.size())) ? c.bias[bo] : 0);
      }
      dut->eval();

      const bool req_a = dut->a_req_o != 0 && dut->a_gnt_i != 0;
      const bool req_w = dut->w_req_o != 0;
      const int off_a = dut->a_off_o;
      const int off_w = dut->w_off_o;
      const bool oob = dut->a_oob_o != 0;

      const bool trace = (std::getenv("SEQ_TRACE") != nullptr) && (ci == 0)
                         && (cycles < 40);
      if (trace) {
        std::printf("  c%-3ld req_a=%d off_a=%-4d oob=%d req_w=%d off_w=%-4d "
                    "wr=%d wroff=%-4d val=%4d\n", cycles, req_a ? 1 : 0, off_a,
                    oob ? 1 : 0, req_w ? 1 : 0, off_w, dut->wr_o ? 1 : 0,
                    static_cast<int>(dut->wr_off_o),
                    static_cast<int>(static_cast<int8_t>(dut->wr_data_o)));
      }

      tick();

      if (dut->wr_o) {
        got[{static_cast<int>(dut->wr_off_o),
             static_cast<int>(static_cast<int8_t>(dut->wr_data_o))}]++;
        ++n_write;
      }

      pend_a = req_a;
      pend_a_off = oob ? -1 : off_a;
      pend_w = req_w;
      pend_w_off = off_w;

      ++cycles;
      if (dut->done_o) break;
    }

    if (cycles >= cap) {
      std::printf("ca %d: khong ket thuc trong %ld chu ky\n", ci, cap);
      ++errors;
      continue;
    }
    total_cycles += cycles;
    {
      // Mo hinh phai dung ch_step THAT: DWCONV chay mot kenh ra moi lan (xem
      // ecg_addrgen `ch_step`). Dung N_PE cho ca DWCONV thi con so "phi" bao
      // gom ca quyet dinh do va khong con do duoc phi cua duong ong xa.
      const int cin_g = c.dw ? 1 : c.cin;
      const int step = c.dw ? 1 : N_PE;
      const int grp = ((c.cout + step - 1) / step) * c.len_out;
      total_model += static_cast<long>(grp) * cin_g * c.k;
    }

    std::map<std::pair<int, int>, int> want;
    for (const auto &wv : c.writes) want[wv]++;
    if (want != got) {
      if (++errors <= 6) {
        std::printf("ca %d (op=%d cin=%d cout=%d k=%d s=%d pad=%d lin=%d "
                    "lout=%d dw=%d relu=%d perch=%d dstoff=%d): %zu phep ghi, "
                    "ky vong %zu\n", ci, c.op, c.cin, c.cout, c.k, c.stride,
                    c.pad, c.len_in, c.len_out, c.dw, c.relu, c.per_ch,
                    c.dst_off, got.size(), want.size());
        // Doi chieu theo OFFSET, khong theo cap (off,val): mot bang "thieu /
        // thua" khong cho biet gia tri o cung offset lech the nao.
        std::map<int, int> wmap, gmap;
        for (const auto &wv : want) wmap[wv.first.first] = wv.first.second;
        for (const auto &gv : got) gmap[gv.first.first] = gv.first.second;
        int shown = 0;
        for (const auto &wv : wmap) {
          const auto it = gmap.find(wv.first);
          if (it == gmap.end()) {
            if (shown++ < 8)
              std::printf("   off=%3d ky vong %4d, KHONG CO\n", wv.first,
                          wv.second);
          } else if (it->second != wv.second) {
            if (shown++ < 8)
              std::printf("   off=%3d ky vong %4d, nhan %4d\n", wv.first,
                          wv.second, it->second);
          }
        }
      }
    }

    // Reset between cases so state cannot carry over.
    dut->rst_ni = 0;
    tick();
    dut->rst_ni = 1;
    tick();
  }
  std::fclose(fp);

#ifdef ECG_TRACE
  if (tfp) { tfp->close(); delete tfp; tfp = nullptr; }
#endif
  dut->final();
  ECG_COV_WRITE();
  delete dut;

  std::printf("tb_ecg_seq: %d lop, %d phep ghi, %ld chu ky "
              "(mo hinh MAC %ld, phi %+.1f %%)\n", n_case, n_write,
              total_cycles, total_model,
              100.0 * (static_cast<double>(total_cycles) / total_model - 1.0));
  if (errors == 0) {
    std::printf("tb_ecg_seq: PASS (duong MAC dung tung bit tren %d lop)\n",
                n_case);
    return 0;
  }
  std::printf("tb_ecg_seq: FAIL (%d lop sai)\n", errors);
  return 1;
}

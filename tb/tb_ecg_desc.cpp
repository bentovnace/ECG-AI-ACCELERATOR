// Verilator harness for ecg_desc.
//
// Vectors come from tools/rtl_ref/desc_ref.py, which packs the 44 REAL layers of
// the four models using the compiler's own pack_layer -- the same function that
// writes the blob. So this is not a check against a hand model; it is a check
// that the packed struct in ecg_pkg.sv and the field list in compile_model.py
// still describe the same 16 bytes. That pair has drifted once already (the
// reserved field went 14 -> 13 bits on one side only, putting every field off by
// one and turning cin = 0 into groups = 0), which is why the reserved-bits check
// below is treated as a first-class assertion rather than a formality.
//
// The second half of the vector file is deliberately illegal descriptors, driven
// with valid = 0 so the module's own $error assertions stay quiet while legal_o
// must still go low. A decoder that returned legal_o = 1 unconditionally would
// pass a suite made only of real layers.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "Vecg_desc.h"
#include "verilated.h"
#include "ecg_cov.h"

namespace {

Vecg_desc *dut = nullptr;

// The 128-bit input arrives as an array of 32-bit words, word 0 the low bits.
void set_word(const char *hex32) {
  uint32_t w[4] = {0, 0, 0, 0};
  // hex32 is 32 nibbles, most significant first.
  for (int i = 0; i < 4; ++i) {
    char buf[9];
    std::memcpy(buf, hex32 + i * 8, 8);
    buf[8] = '\0';
    w[3 - i] = static_cast<uint32_t>(std::strtoul(buf, nullptr, 16));
  }
  for (int i = 0; i < 4; ++i) dut->word_i[i] = w[i];
}

}  // namespace

double sc_time_stamp() { return 0; }

// Moi `return 2` trong duong PHAN TICH phai NOI RA cho nao hong. Truoc do chung
// im lang: `fscanf` that bai roi `return 2` khong in gi, nen mot tep vector bi
// CAT CUT lam `make sim-desc` do voi KHONG MOT DONG dau ra va nguoi doc phai di
// doc ma nguon. Chot bao ve dung truong hop TU BAO MINH ("khong mo duoc", co
// thong bao) va bo truong hop IM LANG.
#define DOC_HONG(what) do {                                                    \
    std::printf("tb_ecg_desc: doc HONG tep vector %s\n"                         \
                "  dang doc: %s  (tb_ecg_desc.cpp:%d)\n"                       \
                "  Tep bi CAT CUT hay sai dinh dang. Sinh lai:\n"              \
                "    python3 tools/rtl_ref/desc_ref.py\n",                              \
                path, (what), __LINE__);                                       \
    return 2; } while (0)

int main(int argc, char **argv) {
  Verilated::commandArgs(argc, argv);

  const char *path = (argc > 1) ? argv[1] : "40-rtl/tb/vectors/desc.txt";
  FILE *fp = std::fopen(path, "r");
  if (!fp) {
    std::printf("tb_ecg_desc: khong mo duoc %s (chay `python3 "
                "tools/rtl_ref/desc_ref.py`)\n", path);
    return 2;
  }

  int n = 0;
  if (std::fscanf(fp, "%d", &n) != 1) DOC_HONG("std::fscanf(fp, '%d', &n) != 1");

  dut = new Vecg_desc;

  int errors = 0;
  int n_real = 0, n_illegal = 0;
  int n_mac_seen = 0, n_pool_seen = 0, n_gap_seen = 0, n_add_seen = 0;
  int n_percc_seen = 0, n_catpart_seen = 0;

  for (int i = 0; i < n; ++i) {
    char hex[64];
    int valid, legal;
    int f[20];   // op..buf_base, dung thu tu FIELDS cua desc_ref.py
    int dec[8];  // is_mac..relu
    if (std::fscanf(fp, "%40s %d %d", hex, &valid, &legal) != 3) DOC_HONG("std::fscanf(fp, '%40s %d %d', hex, &valid, &legal) != 3");
    for (int j = 0; j < 20; ++j) {
      if (std::fscanf(fp, "%d", &f[j]) != 1) DOC_HONG("std::fscanf(fp, '%d', &f[j]) != 1");
    }
    for (int j = 0; j < 8; ++j) {
      if (std::fscanf(fp, "%d", &dec[j]) != 1) DOC_HONG("std::fscanf(fp, '%d', &dec[j]) != 1");
    }

    set_word(hex);
    dut->valid_i = static_cast<uint8_t>(valid);
    dut->eval();

    if (valid) ++n_real; else ++n_illegal;

    auto chk = [&](const char *name, long got, long want) {
      if (got != want) {
        if (++errors <= 15) {
          std::printf("dong %d truong %s: got %ld want %ld\n", i, name, got,
                      want);
        }
      }
    };

    // Truong, dung thu tu FIELDS cua desc_ref.py. Cong ra tung truong rieng
    // nen harness khong lam phep tinh bit nao -- neu no lam thi phep kiem se la
    // bit math viet tay doi bit math viet tay, tuc mot vong tron.
    chk("op", dut->op_o, f[0]);
    chk("act", dut->act_o, f[1]);
    chk("src0", dut->src0_o, f[2]);
    chk("src1", dut->src1_o, f[3]);
    chk("dst", dut->dst_o, f[4]);
    chk("dst_off", dut->dst_off_o, f[5]);
    chk("cin", dut->cin_o, f[6]);
    chk("cout", dut->cout_o, f[7]);
    chk("len_in", dut->len_in_o, f[8]);
    chk("len_out", dut->len_out_o, f[9]);
    chk("k", dut->k_o, f[10]);
    chk("stride", dut->stride_o, f[11]);
    chk("pad", dut->pad_o, f[12]);
    chk("dw", dut->dw_o, f[13]);
    chk("w_base", dut->w_base_o, f[14]);
    chk("rq_base", dut->rq_base_o, f[15]);
    chk("rq_n", dut->rq_n_o, f[16]);
    chk("last", dut->last_o, f[17]);
    chk("cat_part", dut->cat_part_o, f[18]);

    // 13 bit thap la NEN BYTE cua bo dem dich, khong con la du dia. Day la
    // truong thap nhat cua 16 byte nen no lech dau tien neu hai ben lech nhau.
    chk("buf_base", dut->buf_base_o, f[19]);

    chk("is_mac", dut->is_mac_o, dec[0]);
    chk("is_pool", dut->is_pool_o, dec[1]);
    chk("is_gap", dut->is_gap_o, dec[2]);
    chk("is_add", dut->is_add_o, dec[3]);
    chk("is_store", dut->is_store_o, dec[4]);
    chk("uses_src1", dut->uses_src1_o, dec[5]);
    chk("per_ch", dut->per_ch_o, dec[6]);
    chk("relu", dut->relu_o, dec[7]);
    chk("legal", dut->legal_o, legal);

    if (valid) {
      if (dec[0]) ++n_mac_seen;
      if (dec[1]) ++n_pool_seen;
      if (dec[2]) ++n_gap_seen;
      if (dec[3]) ++n_add_seen;
      if (dec[6]) ++n_percc_seen;
      if (f[18]) ++n_catpart_seen;
    }
  }
  std::fclose(fp);

  dut->final();
  ECG_COV_WRITE();
  delete dut;

  std::printf("tb_ecg_desc: %d lop that + %d the phi phap; thay %d MAC, "
              "%d MAXPOOL, %d GAP, %d ADD, %d per-channel, %d cat_part\n",
              n_real, n_illegal, n_mac_seen, n_pool_seen, n_gap_seen,
              n_add_seen, n_percc_seen, n_catpart_seen);

  // Do phu: neu bo vector khong chua du cac dang thi mot bo giai ma sai o mot
  // dang van pass. Bon ho cong lai PHAI co ca sau dang duoi.
  if (n_mac_seen == 0 || n_pool_seen == 0 || n_gap_seen == 0 ||
      n_add_seen == 0 || n_percc_seen == 0 || n_catpart_seen == 0 ||
      n_illegal == 0) {
    std::printf("tb_ecg_desc: FAIL (bo vector khong phu het cac dang)\n");
    return 1;
  }
  if (errors == 0) {
    std::printf("tb_ecg_desc: PASS (ecg_pkg.sv va compile_model.py khop tung "
                "bit tren 16 B, legal_o dung)\n");
    return 0;
  }
  std::printf("tb_ecg_desc: FAIL (%d loi)\n", errors);
  return 1;
}

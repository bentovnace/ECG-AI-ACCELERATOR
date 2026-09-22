// Verilator harness for ecg_actbuf.
//
// Three properties are checked, and they are the three the thresholds depend on.
//
//   1. Data integrity through the base register file. A shadow model in C++
//      holds what each byte should contain; every granted read is compared
//      against it. Two logical buffers deliberately overlap in bytes, because
//      that is what the compiler's liveness-based allocation produces and a
//      model that assumed disjoint ranges would pass a weaker test.
//
//   2. A single read request. There is no arbiter any more: CONCAT lost its
//      descriptor record in ADR-0014 §2.1 and ADD reads its two operands
//      sequentially because each carries its own requant scale, so nothing needs
//      two simultaneous reads. Removing the second path took the N9 shared-logic
//      fraction from 90,1 % to 92,6 %. What this phase checks instead is that a
//      request is granted every cycle it is asserted -- the property the
//      sequencers now rely on for their one-cycle issue rate.
//
//   3. Boundary zero injection. A read marked out of bounds must return zero and
//      must not fetch. The harness checks the returned value and, separately,
//      that a zero-injected read is never counted as a memory access.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

#include "Vecg_actbuf.h"
#include "verilated.h"
#include "ecg_cov.h"

namespace {

constexpr int BYTES = 6144;
constexpr int N_BUF = 16;   // 13 bo dem logic + 2 cong vao + 1 du

Vecg_actbuf *dut = nullptr;

void tick() {
  dut->clk_i = 0;
  dut->eval();
  dut->clk_i = 1;
  dut->eval();
}

// Combinational outputs (the grants) are only valid after eval(); the registered
// output (rdata) is valid right after the tick that issued the request. An
// earlier version read the grants before eval and compared rdata one request
// late, which reported the RTL as broken when the harness was.
void settle() { dut->eval(); }

}  // namespace

double sc_time_stamp() { return 0; }

int main(int argc, char **argv) {
  Verilated::commandArgs(argc, argv);

  dut = new Vecg_actbuf;
  dut->rst_ni = 0;
  dut->base_we_i = 0;
  dut->infer_start_i = 0;
  dut->ra_req_i = 0;
  dut->we_i = 0;
  for (int i = 0; i < 3; ++i) tick();
  dut->rst_ni = 1;
  tick();

  // Overlapping bases on purpose: buffers 3 and 4 share bytes, which is what the
  // liveness allocator produces for models where they are never live together.
  std::vector<int> base(N_BUF, 0);
  for (int i = 0; i < N_BUF; ++i) base[i] = (i * 256) % BYTES;
  base[4] = base[3];

  for (int i = 0; i < N_BUF; ++i) {
    dut->base_we_i = 1;
    dut->base_sel_i = static_cast<uint8_t>(i);
    dut->base_i = static_cast<uint16_t>(base[i]);
    tick();
  }
  dut->base_we_i = 0;

  std::vector<int8_t> shadow(BYTES, 0);
  std::vector<char> written(BYTES, 0);
  std::mt19937 rng(0);

  int errors = 0;
  int grants_a = 0;
  // Dem RIENG cho pha 3: grants_a cong ca pha 2 nen so sanh no voi so nhip cua
  // pha 3 cho ra so am -- da mac dung loi ke toan do.
  int p3_req = 0, p3_gnt = 0;
  int oob_reads = 0;

  auto check_read = [&](bool unused_b, bool oob, int addr) {
    (void)unused_b;
    if (dut->rdata_valid_o == 0) {
      if (++errors <= 10) std::printf("no rdata_valid after a granted read\n");
      return;
    }
    const int8_t got = static_cast<int8_t>(dut->rdata_o);
    const int8_t want = oob ? 0 : shadow[addr];
    if (got != want) {
      if (++errors <= 10) {
        std::printf("data mismatch addr %d oob %d: got %d want %d\n", addr,
                    oob ? 1 : 0, got, want);
      }
    }
  };

  // Phase 1: fill every byte through the buffer/offset interface, so the shadow
  // and the base register file are exercised together.
  for (int b = 0; b < N_BUF; ++b) {
    for (int off = 0; off < 64; ++off) {
      const int addr = (base[b] + off) % BYTES;
      const int8_t v = static_cast<int8_t>(rng() & 0xFF);
      dut->we_i = 1;
      dut->w_buf_i = static_cast<uint8_t>(b);
      dut->w_off_i = static_cast<uint16_t>(off);
      dut->wdata_i = v;
      dut->ra_req_i = 0;
      settle();
      tick();
      shadow[addr] = v;
      written[addr] = 1;
    }
  }
  dut->we_i = 0;

  // Phase 2: single-sided reads. A alone must be granted every cycle.
  for (int b = 0; b < N_BUF; ++b) {
    for (int off = 0; off < 64; ++off) {
      dut->ra_req_i = 1;
      dut->ra_buf_i = static_cast<uint8_t>(b);
      dut->ra_off_i = static_cast<uint16_t>(off);
      dut->ra_oob_i = 0;
      settle();
      if (dut->ra_gnt_o == 0) {
        if (++errors <= 10) std::printf("khong cap phep khi chi mot yeu cau\n");
      }
      tick();
      check_read(false, false, (base[b] + off) % BYTES);
      ++grants_a;
    }
  }

  // Phase 3: back-to-back reads. With one requester the grant must come every
  // cycle -- a sequencer that issues one MAC per cycle depends on exactly that.
  for (int i = 0; i < 2000; ++i) {
    const int b = static_cast<int>(rng() % N_BUF);
    const int o = static_cast<int>(rng() % 64);
    dut->ra_req_i = 1;
    dut->ra_buf_i = static_cast<uint8_t>(b);
    dut->ra_off_i = static_cast<uint16_t>(o);
    dut->ra_oob_i = 0;
    settle();
    if (dut->ra_gnt_o == 0) {
      if (++errors <= 10) std::printf("khong cap phep o nhip %d\n", i);
    } else {
      ++p3_gnt;
      ++grants_a;
    }
    ++p3_req;
    tick();
    check_read(false, false, (base[b] + o) % BYTES);
  }

  // Phase 4: zero injection. An out-of-bounds read returns zero and must not be
  // treated as a fetch, so it is legal even at an address never written.
  for (int i = 0; i < 500; ++i) {
    dut->ra_req_i = 1;
    dut->ra_buf_i = static_cast<uint8_t>(rng() % N_BUF);
    dut->ra_off_i = static_cast<uint16_t>(rng() % 64);
    dut->ra_oob_i = 1;
    settle();
    tick();
    check_read(false, true, 0);
    ++oob_reads;
  }

  dut->ra_req_i = 0;
  settle();
  tick();

  // ---- Pha 5: BASE DA DANG BIT --------------------------------------------
  //
  // VI SAO PHA NAY TON TAI. `make coverage` do toggle cua `ecg_actbuf` la
  // **34,8 %**, va phan ra theo phan cap bac gia thuyet "bit bo nho chiem mau
  // so": `u_mem` gop 13/90 diem chua cham con logic dieu khien gop 460/636, dan
  // dau la `base_q[0]` (26/26 CHUA CHAM), `base_q[1]`, `base_q[2]`...
  // Nguyen nhan: bon pha tren dung base = `(i * 256) % BYTES`, tuc **tam bit
  // thap cua moi base luon bang 0**. Mot sai so hoc o bit thap cua base -- mot
  // lat bit sai, mot phep cong thieu carry -- di qua ca bon pha ma khong ai thay.
  //
  // Pha nay THEM chu khong SUA bon pha tren: bo base `i * 256` ma hoa mot hanh vi
  // that cua bo cap phat (buffer 3 va 4 trung nhau vi chung khong bao gio song
  // cung luc), nen doi no la lam yeu mot phep kiem dang co.
  {
    // Bo gia tri phai PHU MOI VI TRI BIT theo CA HAI chieu. Khong khang dinh
    // dieu do -- KIEM no ngay duoi, vi mot bo gia tri "trong nhu da dang" ma
    // thieu mot bit thi pha nay dat ma khong kiem duoc gi.
    // MOT BAT BIEN LO RA KHI VIET PHA NAY, va no dang ghi lai.
    //
    // Ban dau bo nay co `6143`, va voi offset 1 thi dia chi ra 6144 -- RTL BAN
    // assertion `ecg_sram_1r1w: write address 6144 out of range (DEPTH=6144)`.
    // Tuc phep cong base+off **KHONG WRAP**: bo cap phat phai bao dam
    // `base + off < BYTES`, va RTL bat neu khong.
    //
    // Nhung mo hinh bong cua chinh testbench nay tinh `(base[b] + off) % BYTES`,
    // tuc no NGU Y wrap. Mo hinh **de dai hon RTL**, va bon pha tren khong bao
    // gio lam lo ra dieu do: base = i*256 voi off < 64 cho toi da 3.903 < 6.144,
    // nen phep `% BYTES` chua bao gio thuc su wrap lan nao. Mot mo hinh de dai
    // hon thiet ke thi khong sai o dau, nhung no che mat mot bat bien -- va bat
    // bien do la mot dieu nguoi doc firmware phai biet.
    //
    // Nen bo gia tri duoi day giu `base + max(off) < BYTES`, va co mot chot noi
    // ra dieu do ngay sau, thay vi de phep `% BYTES` am tham lam no dung.
    const int base5[N_BUF] = {
        0,    1,    2,    4,    8,    16,   32,   64,
        128,  341,  682,  1365, 2730, 4095, 5461, 5120,
    };
    // Mat na phai theo BE RONG DIA CHI, KHONG phai `BYTES - 1`.
    // `BYTES = 6144` khong la luy thua 2, nen `BYTES - 1 = 0x17FF` co bit 11
    // TRONG -- no khong phai mot mat na bit. Chinh chot nay bat loi do: no bao
    // "bit=1 phu 0x1FFF ... can 0x17FF", tuc tieu chi sai chu khong bo gia tri
    // sai. Ca 13 bit deu toi duoc trong khoang hop le (bit 11 qua 2048, bit 12
    // qua 4096, ca hai < 6144).
    int aw = 0;
    while ((1 << aw) < BYTES) ++aw;              // 13 voi BYTES = 6144
    const int MASK = (1 << aw) - 1;
    int co_1 = 0, co_0 = 0;
    for (int i = 0; i < N_BUF; ++i) {
      if (base5[i] >= BYTES) {
        std::printf("tb_ecg_actbuf: FAIL (base5[%d] = %d >= BYTES)\n", i, base5[i]);
        return 1;
      }
      co_1 |= base5[i] & MASK;
      co_0 |= (~base5[i]) & MASK;
    }
    if (co_1 != MASK || co_0 != MASK) {
      std::printf("tb_ecg_actbuf: FAIL (bo base pha 5 KHONG phu moi bit: "
                  "bit=1 phu 0x%X, bit=0 phu 0x%X, can 0x%X)\n",
                  co_1, co_0, MASK);
      return 1;
    }

    for (int i = 0; i < N_BUF; ++i) {
      dut->base_we_i = 1;
      dut->base_sel_i = static_cast<uint8_t>(i);
      dut->base_i = static_cast<uint16_t>(base5[i]);
      tick();
    }
    dut->base_we_i = 0;

    // Ghi roi doc lai qua tung base. Dung offset LE (1, 7, 63) de phep cong
    // base+off sinh carry vao cac bit thap -- offset boi so cua 8 se khong.
    const int offs[] = {0, 1, 7, 63, 255};
    // CHOT cho bat bien "khong wrap": neu ai doi bo base hay bo offset lam mot
    // cap vuot BYTES thi phep kiem noi ra o DAY, chu khong de RTL ban assertion
    // roi nguoi doc phai truy nguoc.
    for (int b = 0; b < N_BUF; ++b) {
      for (int off : offs) {
        if (base5[b] + off >= BYTES) {
          std::printf("tb_ecg_actbuf: FAIL (pha 5 vi pham bat bien khong-wrap: "
                      "base5[%d]=%d + off %d = %d >= BYTES %d)\n",
                      b, base5[b], off, base5[b] + off, BYTES);
          return 1;
        }
      }
    }
    int p5_err = 0;
    // TRINH TU PHAI GIONG PHA 1 VA 2, va `ra_oob_i` PHAI dat lai.
    // Ban dau toi viet trinh tu rieng (`tick()` truoc `settle()`) va **khong dat
    // `ra_oob_i = 0`**. Pha 4 la "bom 0 cho doc ngoai pham vi" nen no de co do
    // bang 1, pha 5 thua huong, va MOI phep doc tra 0 -> 80 loi. Con so 80 do la
    // loi cua HARNESS, khong cua thiet ke: mot trang thai bi ke thua giua hai pha.
    for (int b = 0; b < N_BUF; ++b) {
      for (int off : offs) {
        const int addr = base5[b] + off;   // chot o tren: < BYTES
        const int8_t v = static_cast<int8_t>((b * 31 + off * 7 + 5) & 0xFF);
        dut->we_i = 1;
        dut->w_buf_i = static_cast<uint8_t>(b);
        dut->w_off_i = static_cast<uint16_t>(off);
        dut->wdata_i = v;
        dut->ra_req_i = 0;
        settle();
        tick();
        shadow[addr] = v;
        written[addr] = 1;
      }
    }
    dut->we_i = 0;
    for (int b = 0; b < N_BUF; ++b) {
      for (int off : offs) {
        const int addr = base5[b] + off;   // chot o tren: < BYTES
        dut->ra_req_i = 1;
        dut->ra_buf_i = static_cast<uint8_t>(b);
        dut->ra_off_i = static_cast<uint16_t>(off);
        dut->ra_oob_i = 0;
        settle();
        if (dut->ra_gnt_o == 0 && ++p5_err <= 5)
          std::printf("pha 5: khong cap phep base[%d]=%d off %d\n", b, base5[b], off);
        tick();
        if (dut->rdata_valid_o == 0) {
          if (++p5_err <= 5)
            std::printf("pha 5: khong co rdata_valid base[%d]=%d off %d\n",
                        b, base5[b], off);
        } else if (static_cast<int8_t>(dut->rdata_o) != shadow[addr]) {
          if (++p5_err <= 5)
            std::printf("pha 5: base[%d]=%d off %d -> addr %d: duoc %d, can %d\n",
                        b, base5[b], off, addr,
                        static_cast<int>(static_cast<int8_t>(dut->rdata_o)),
                        static_cast<int>(shadow[addr]));
        }
      }
    }
    dut->ra_req_i = 0;
    if (p5_err) {
      errors += p5_err;
    } else {
      std::printf("  ok    pha 5: %d base da dang bit x %d offset, du lieu dung "
                  "(phu ca %d bit dia chi theo hai chieu)\n",
                  N_BUF, static_cast<int>(sizeof offs / sizeof offs[0]), aw);
    }
  }

  // ------------------------------------------------------------------ pha 6
  // BASE_Q: 16 o x 13 bit = 416 diem toggle, va 317 trong so do CHUA cham --
  // 88,8 % ca lo cua module. Ly do khong phai cau truc: toggle doi HAI gia tri
  // khac nhau ghi vao CUNG mot o, con pha 1 va pha 5 chi ghi MOT gia tri moi o
  // (`base[0] = 0` va `base5[0] = 0`, nen `base_q[0]` khong toggle lan nao).
  // Bo gia tri `base5[]` phu moi bit GIUA CAC O, khong TRONG mot o.
  //
  // Bo ba gia tri duoi day toggle ca 13 bit theo HAI chieu ma moi dia chi van
  // NAM TRONG bo nho, nen pha nay doc lai va KIEM du lieu chu khong chi lam
  // day do phu:
  //     0 -> 4095 (0x0FFF, bit 0-11 len) -> 4096 (0x1000, bit 0-11 xuong,
  //     bit 12 len) -> 0 (bit 12 xuong)
  // 4095 va 4096 deu < BYTES = 6144, nen `base + 0` la dia chi hop le. Mot bo
  // gia tri "de nhat" nhu 0x1FFF = 8191 se vuot bo nho: khoi `assume` cua RTL
  // nam trong `ifdef FORMAL` nen mo phong khong bat, va loi se thanh mot phep
  // ghi ngoai mang -- mot phep do lam hong thu no dang do.
  {
    int p6_err = 0;
    const int vals[] = {4095, 4096, 0};
    for (int i = 0; i < N_BUF; ++i) {
      for (int v : vals) {
        dut->base_we_i = 1;
        dut->base_sel_i = static_cast<uint8_t>(i);
        dut->base_i = static_cast<uint16_t>(v);
        tick();
        dut->base_we_i = 0;
        // Ghi roi doc lai qua chinh o vua dat: neu phep ghi base khong vao dung
        // o `i`, hay lam nhiem mot o khac, dia chi se lech va du lieu sai.
        const int8_t d = static_cast<int8_t>(-100 + i);
        dut->we_i = 1;
        dut->w_buf_i = static_cast<uint8_t>(i);
        dut->w_off_i = 0;
        dut->wdata_i = d;
        tick();
        dut->we_i = 0;
        shadow[v] = d;
        written[v] = 1;
        dut->ra_req_i = 1;
        dut->ra_buf_i = static_cast<uint8_t>(i);
        dut->ra_off_i = 0;
        dut->ra_oob_i = 0;
        settle();
        if (dut->ra_gnt_o == 0 && ++p6_err <= 5)
          std::printf("pha 6: khong cap phep base_q[%d]=%d\n", i, v);
        tick();
        dut->ra_req_i = 0;
        if (dut->rdata_valid_o == 0) {
          if (++p6_err <= 5)
            std::printf("pha 6: khong co rdata_valid base_q[%d]=%d\n", i, v);
        } else if (static_cast<int8_t>(dut->rdata_o) != d) {
          if (++p6_err <= 5)
            std::printf("pha 6: base_q[%d]=%d -> duoc %d, can %d\n", i, v,
                        static_cast<int>(static_cast<int8_t>(dut->rdata_o)),
                        static_cast<int>(d));
        }
      }
    }
    if (p6_err) {
      errors += p6_err;
    } else {
      std::printf("  ok    pha 6: %d o base x 3 gia tri (0/4095/4096) -- ca 13 "
                  "bit base doi theo HAI chieu, dia chi va du lieu dung\n",
                  N_BUF);
    }
  }

  // ------------------------------------------------------------------ pha 7
  // `ra_off_i` va `w_off_i`: 10/26 diem moi cai chua cham -- cac bit cao cua
  // OFFSET chua bao gio doi. Cung bo ba gia tri, lan nay base = 0 va offset
  // doi, tuc doi xung voi pha 6.
  {
    int p7_err = 0;
    const int offs7[] = {0, 4095, 4096, 0};
    dut->base_we_i = 1;
    dut->base_sel_i = 0;
    dut->base_i = 0;
    tick();
    dut->base_we_i = 0;
    for (int off : offs7) {
      const int8_t d = static_cast<int8_t>(off & 0x3F);
      dut->we_i = 1;
      dut->w_buf_i = 0;
      dut->w_off_i = static_cast<uint16_t>(off);
      dut->wdata_i = d;
      tick();
      dut->we_i = 0;
      shadow[off] = d;
      written[off] = 1;
      dut->ra_req_i = 1;
      dut->ra_buf_i = 0;
      dut->ra_off_i = static_cast<uint16_t>(off);
      dut->ra_oob_i = 0;
      settle();
      tick();
      dut->ra_req_i = 0;
      if (static_cast<int8_t>(dut->rdata_o) != d && ++p7_err <= 5)
        std::printf("pha 7: off %d -> duoc %d, can %d\n", off,
                    static_cast<int>(static_cast<int8_t>(dut->rdata_o)),
                    static_cast<int>(d));
    }
    if (p7_err) {
      errors += p7_err;
    } else {
      std::printf("  ok    pha 7: offset 0/4095/4096 -- ca 13 bit offset doi "
                  "theo hai chieu, du lieu dung\n");
    }
  }

  // ------------------------------------------------------------------ pha 8
  // `infer_start_i` CHUA TOGGLE LAN NAO (2/2 diem) va `epoch_q` 16/16 -- tuc
  // co che nhip cua ADR-0013 chua bao gio duoc chay o muc module. 256 nhip
  // `infer_start_i` dua `epoch_q` di 0 -> 255 -> 0, doi ca 8 bit theo hai
  // chieu, va vi no VE 0 thi cac byte ghi o nhip 0 lai nhat quan.
  //
  // KHONG doc gi trong 256 nhip do, co chu y: khoi `ifndef SYNTHESIS` ban
  // `$error` khi doc mot byte ghi o nhip TRUOC, va mot phep doc o giua se
  // ban dung cai canh bao do -- tuc pha nay se lam tb HONG chu khong lam no
  // manh hon. Phep doc kiem dat SAU khi nhip da ve 0.
  {
    int p8_err = 0;
    for (int i = 0; i < 256; ++i) {
      dut->infer_start_i = 1;
      tick();
      dut->infer_start_i = 0;
      tick();
    }
    const int8_t d = 77;
    dut->we_i = 1;
    dut->w_buf_i = 0;
    dut->w_off_i = 8;
    dut->wdata_i = d;
    tick();
    dut->we_i = 0;
    shadow[8] = d;
    written[8] = 1;
    dut->ra_req_i = 1;
    dut->ra_buf_i = 0;
    dut->ra_off_i = 8;
    dut->ra_oob_i = 0;
    settle();
    tick();
    dut->ra_req_i = 0;
    if (static_cast<int8_t>(dut->rdata_o) != d) {
      ++p8_err;
      std::printf("pha 8: sau 256 nhip -> duoc %d, can %d\n",
                  static_cast<int>(static_cast<int8_t>(dut->rdata_o)),
                  static_cast<int>(d));
    }
    if (p8_err) {
      errors += p8_err;
    } else {
      std::printf("  ok    pha 8: 256 nhip `infer_start_i` (epoch_q 0->255->0, "
                  "ca 8 bit doi hai chieu), doc lai dung sau khi nhip ve 0\n");
    }
  }

  // ------------------------------------------------------------------ pha 9
  // `rst_ni` 1/2: mot tb reset MOT lan thi chieu 1 -> 0 khong the cham. Pha nay
  // dat o CUOI CUNG va khong doc gi sau no, vi reset xoa `written` -- mot phep
  // doc sau reset se ban `$error` cua ADR-0013 va lam tb HONG.
  //
  // Va no kiem mot bat bien THAT chu khong chi doi mot bit: reset phai xoa
  // thanh ghi hop le, tuc `rdata_valid_o` ve 0 ke ca khi nhip truoc do vua co
  // mot yeu cau duoc cap.
  {
    dut->ra_req_i = 1;
    dut->ra_buf_i = 0;
    dut->ra_off_i = 8;
    dut->ra_oob_i = 0;
    settle();
    dut->rst_ni = 0;
    tick();
    dut->ra_req_i = 0;
    for (int i = 0; i < 3; ++i) tick();
    settle();
    if (dut->rdata_valid_o != 0) {
      ++errors;
      std::printf("pha 9: sau reset `rdata_valid_o` = %d, can 0 (reset khong "
                  "xoa thanh ghi hop le)\n",
                  static_cast<int>(dut->rdata_valid_o));
    } else {
      std::printf("  ok    pha 9: reset giua bai xoa `rdata_valid_o` ke ca khi "
                  "vua co mot yeu cau duoc cap (va `rst_ni` doi hai chieu)\n");
    }
    dut->rst_ni = 1;
    tick();
  }

  dut->final();
  ECG_COV_WRITE();
  delete dut;

  std::printf("tb_ecg_actbuf: %d phep doc duoc cap, pha lien tiep %d/%d nhip, "
              "%d phep doc bom 0\n", grants_a, p3_gnt, p3_req, oob_reads);

  // Mot yeu cau phai duoc cap MOI nhip: cac sequencer phat mot nhip mot lan doc
  // va se dung neu khong.
  if (p3_gnt != p3_req) {
    std::printf("tb_ecg_actbuf: FAIL (%d nhip khong duoc cap)\n",
                p3_req - p3_gnt);
    return 1;
  }
  if (errors == 0) {
    std::printf("tb_ecg_actbuf: PASS (du lieu dung, cap phep moi nhip, bom 0 "
                "dung)\n");
    return 0;
  }
  std::printf("tb_ecg_actbuf: FAIL (%d errors)\n", errors);
  return 1;
}

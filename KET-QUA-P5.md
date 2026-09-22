# Kết quả P5 · RTL bộ đồng xử lý

Trạng thái: **các bước P5.2a → P5.4 đã xong**. N2 đạt, N9 có số đo, cả bộ đồng xử
lý tổng hợp được. Ba ngưỡng còn trống đều cần board thật, và tần số đạt được cần
Vivado — cả hai không có trong môi trường này.

Mọi con số dưới đây đều lấy từ một lệnh `make` cụ thể, nêu kèm. Không con số nào
là ước lượng trừ nơi nói rõ.

---

## 1. Bảng tổng — ngưỡng nào đạt, bằng gì

| Ngưỡng | Yêu cầu | Kết quả | Đo bằng |
|---|---|---|---|
| **N2** | bit-exact RTL vs đường số nguyên | **1000/1000 nhịp, cả 4 họ** (250/họ, phân tầng theo lớp, gồm *cả 7* nhịp lớp Q của DS2), chạy liên tiếp không reset | `make sim-coproc` |
| **N3** | T_switch ≤ 1.000 chu kỳ | **667** (xấu nhất, m2) | `make isa` |
| **N6** | ≥ 4 mô hình thường trú | 4 (descriptor/thang/bias thường trú, trọng số nạp khi chuyển) | `make n7` |
| **N7** | ≤ 24 kB trên chip | **13.228 B = 12,92 kB** (53,8 %) | `make n7` |
| **N8** | 0 nhịp bỏ lỡ | T_infer 14.898 / 31.558 / 32.382 / 35.278 trên 139.000 → dư **3,9–9,3×** | `make isa` |
| formal | thuộc tính chức năng | **1** — `mac8.sby` chứng minh 24 bit psum là đủ (k-induction), **đã thử đột biến**: cho phép 211 số hạng thì induction bật đúng dòng, nên chặn 210 là chặt | `make formal`, `make formal-mutant` |
| **N9** | ≥ 90 % logic dùng chung | **97,1 % đo bằng tổng hợp**, tức **chi phí cấu hình lại 72 ± 10 LUT** = 2,9 % thiết kế (ước lượng gate: 92,6 %). Sàn nhiễu ±10 LUT: chỉ DWCONV (+85 LUT) là logic riêng đo được | `make n9-synth` |
| N1 | bản ràng buộc mất ≤ 2,0 điểm so với bản tự do | **hai lần nhìn (ADR-0008)**: lần 1 đạt 2/4, lần 2 đạt **3/4** (Inception +0,68 vượt lên đạt). Trượt ở cả hai lần: **MobileNet** +2,68 — phía mô hình, không phải RTL | `make ds2-all` |
| N4, N5, N10 | phương sai / T_switch nguội / độ trễ ba đường | **chưa có số** — cần board | — |

Tài nguyên trên XC7Z020, `make fpga-res`:

| | LUT | FF | CARRY4 | DSP | RAMB |
|---|---|---|---|---|---|
| `ecg_coproc` (cả bộ) | 2.457 | 1.412 | 256 | 10 | 5 |
| `ecg_cvxif` (shim) | 39 | 52 | 6 | 0 | 0 |
| **Tổng** | **2.496** | **1.464** | **262** | **10** | **5** |
| Chiếm | 4,7 % | 1,4 % | — | 4,5 % | 3,6 % |

---

## 2. Kiểm chứng — ba lớp, và mỗi lớp bắt được thứ hai lớp kia không bắt

**Lớp 1 — testbench đối chiếu một hiện thực riêng.** 11 testbench, mỗi cái đối
chiếu với một hiện thực độc lập của **cùng đặc tả**, không phải với hành vi của
module. Đó là chỗ suýt sai: bản `ecg_mac8` đầu tiên cộng tám làn qua cây cộng và
tham chiếu C++ của nó cũng cộng tám làn — hai bên đồng ý với nhau và cả hai lệch
khỏi kiến trúc, vốn ánh xạ tám làn thành tám **kênh ra**. Tham chiếu phải mô tả hệ
thống, không mô tả module.

**Lớp 2 — N2, chạy hết một mô hình.** `make sim-coproc` chạy cả bộ đồng xử lý từ
blob thật trên 4 nhịp DS2 thật cho mỗi họ và đối chiếu **mọi địa chỉ bộ đệm** với
`run_blob`. Nó bắt được đúng một lỗi mà **không** testbench module nào bắt được:
bias miền đầu ra của một kênh (m3.blocks_1_dw_0 kênh 3) là −170, không vừa int8,
nên ở 8 bit nó cuộn thành +86 và cả kênh bão hoà +127. Tham chiếu dùng int64 nên
nó đúng; bộ vector của từng module sinh bias trong [−128, 128] nên chúng không
phân biệt được 8 với 9 bit.

**Lớp 3 — formal.** `make formal`, 27 thuộc tính trên 7 module: 22 chứng minh bằng
k-induction (đúng với **mọi** đầu vào và mọi độ dài), 5 của `ecg_wmem` là BMC độ
sâu 24. Với `ecg_requant` không gian đầu vào là 2⁴⁹ — đó là khác biệt giữa "chưa
thấy sai" và "không thể sai". Chi tiết ở §B′ của `CHECKLIST-VERIFY.md`.

**Lớp 4 — tái lập.** `make vectors-check` bám SHA-256 33 tệp vector và blob. Nó
vẫn khớp sau một lần `make sim-leaf` đầy đủ, tức việc sinh vector là tất định và
bất kỳ ai chạy lại cũng ra đúng những con số trên.

---

## 3. Sáu khoảng trống đặc tả, tất cả cùng một lớp

Đây là phát hiện có giá trị nhất của P5, và nó không phải một lỗi mà là một **kiểu**
lỗi. ADR-0014 §5 đã ghi lại lần đầu; P5 gặp thêm năm lần nữa:

| # | Thiếu gì | Vì sao vòng tròn kiểm lệch không thấy |
|---|---|---|
| 1 | `len_out`/`cin`/`cout` bằng 0 ở 26/44 bản ghi | trình thông dịch có hình dạng từ mảng numpy |
| 2 | `buf_base` tính rồi nhưng không đóng gói vào 16 B | trình thông dịch dùng biến Python, không đọc nền |
| 3 | Hai cổng vào không có chỗ trong bộ cấp phát | hai tensor đó là **đối số hàm**, không nằm trong bộ đệm |
| 4 | Bố cục trọng số `[cout,cin,k]` — 0/34 lớp đọc được 8 byte liền | cả hai bên đọc bằng cách chỉ số hoá, không bằng phép đọc 8 byte |
| 5 | Bias 8 bit trong khi 1/562 mục cần 9 | cả hai bên dùng int64 |
| 6 | Nền bảng bias không nằm trong descriptor | trình thông dịch tra bảng theo lớp |

**Điểm chung:** vòng tròn `make isa` khép lại được vì trình thông dịch **có** thứ
mà phần cứng **không có**. Nó chứng minh hai bộ tiêu thụ *có meta* nhất trí với
nhau, chứ không chứng minh blob tự đủ.

Nên P5 thêm ba phép kiểm mà `make isa` không thay thế được:

- `make selfcheck` — mở blob và **chỉ** blob, dựng lại chuỗi hình dạng từ
  `k`/`stride`/`pad` nên bắt cả `len_out` **sai**, không chỉ bằng 0.
- `make wlayout` — hỏi *tại offset mà bộ sinh địa chỉ sẽ phát, tám byte đọc được
  có phải trọng số của tám kênh ra liền nhau?* `make isa` báo BIT-EXACT với **cả
  hai** bố cục; đã thử và xác nhận.
- `make sim-coproc` — N2, lớp 2 ở trên.

---

## 4. Sáu quyết định kiến trúc, mỗi cái từ một phép đo

| Quyết định | Số liệu đứng sau | ADR |
|---|---|---|
| Bộ đệm kích hoạt 4.096 → **6.144 B** | đỉnh mức thật m4 4.419 B; 5.120 B làm m4 **thất bại cấp phát** (cần lỗ 960 B liên tục, còn 1.661 B phân mảnh 256/768/637); đã quét 12 tổ hợp chính sách xếp; 3 RAMB18 cho **trọn** 6.144 B nên không tốn thêm khối | 0015 |
| Thường trú **một** bộ trọng số + DMA 8 B/ck | bốn bộ thường trú → N7 25.318 B **trượt**; một bộ → 11.634 B đạt, đổi bằng T_switch 96 → 667 chu kỳ (còn dư 33 %) | 0015 |
| **Bỏ cổng đọc kích hoạt thứ hai** | CONCAT mất bản ghi (0014 §2.1) và ADD đọc tuần tự vì mỗi toán hạng một thang → không còn ai cần đọc song song; N9 90,1 → **92,6 %**, actbuf 318 → **207 LUT** | — |
| DWCONV chạy **một** kênh ra mỗi lần | 8 làn cần 8 kích hoạt khác nhau mà đường đọc phát một byte; phương án song song theo thời gian đòi cổng đọc thứ hai — đúng thứ N9 phạt. +50,4 % chu kỳ MAC của m3, và N8 vẫn dư 4,6× | — |
| Xả requant **ghép đường ống** | xả nối tiếp +24 % chu kỳ MAC; ghép đường ống **+3,8 %** đo trên 68 lớp, giá 192 flop | — |
| Bias **9 bit**, bảng 1.124 B | 1/562 mục là −170; và phần tiết kiệm thật của 0014 §2.3 là **bộ cộng** 17 → 9 bit, không phải bảng 1.195 → 1.124 B | 0014 §2.3 (đã sửa) |

Kỷ luật xuyên suốt: **không phép nhân nào trong đường điều khiển.** Năm tích cần
thiết (`in_ch×len_in`, `(in_ch·k+tap)×cout`, `out_ch×len_out`, `k×cout`,
`dst_off×len_out`) đều giữ bằng thanh ghi chạy hoặc dựng bằng dịch-cộng. Kiểm
chứng được: `make fpga-res` cho **10 DSP = 8 của mac8 + 1 của requant + 1 của
seq_vec**, tức các FSM thêm **0 DSP**. Hai lần một phép nhân lọt vào và cả hai lần
con số DSP vô lý là dấu hiệu duy nhất (§C C3, C22).

---

## 5. Bảy lỗi harness, ba lỗi công cụ

**Harness sai còn RTL đúng — bảy lần.** Dấu hiệu mỗi lần là một **con số vô lý**:
31.374 lỗi trên 22.718 vector (nhiều hơn tổng) · làn 0 đúng còn các làn sau ra số
lớn bất thường (Verilator không mở rộng dấu khi đóng gói) · giá trị khớp chính xác
nhưng lệch đúng một vị trí trong danh sách · dãy phát ra 1,2,3,4 thay vì 0,1,2,3 ·
số phép ghi bằng −832.

**Công cụ im lặng — ba lần cùng một kiểu.** Một tín hiệu dùng trước khi khai báo:
Verilator chấp nhận, slang từ chối theo LRM, và `make fpga-res` **lặng lẽ báo 0
cell cho mọi module**. Đã xảy ra năm lần. Hai phép sửa hạ tầng: `make fpga-res` có
chốt báo lỗi thay vì in bảng 0, và `make lint-rtl` nay chạy **cả hai** bộ phân
tích. Lần thứ năm nó dừng ở cổng lint.

**Đo mà không đáng tin thì không ghi.** `ltp` trên cả `ecg_coproc` cho 590 mức kèm
cảnh báo "Detected loop" từ ABC — con số đó không vào bất kỳ bảng nào. Chỉ ghi số
của các module lá, nơi kết quả đọc được. Và `make depth` nói rõ nó **không phải**
tần số: CARRY4 nhanh hơn LUT nhiều lần.

---

## 6. Còn tồn

**Cần board thật** (không làm được ở đây): N4 phương sai T_switch ≤ 5 % · N5
T_switch nguội · N10 độ trễ ba đường.

**Cần Vivado** (người dùng có, môi trường này không): §C C11 tần số đạt được. Hai
module sâu nhất bằng nhau ở 102 mức (`ecg_mac8`, `ecg_vecop`), cả hai vì có một bộ
cộng 24 bit trong một chu kỳ — nếu không đạt tần số thì đó là hai chỗ nhìn trước,
và chia bộ tích luỹ thành hai tầng là phép sửa hiển nhiên. Chưa làm vì chưa có số
để biết có cần.

**Hoãn có lý do đo được:** §C C12 ghép hai phép nhân int8 vào một DSP48E1 (cần thủ
thuật pre-adder mà yosys khó suy ra, và DSP mới dùng 4,5 %) · §C C21 dùng chung một
`ecg_requant` giữa hai sequencer (217 LUT + 1 DSP, trong khi LUT dùng 4,7 % chip —
nó đổi một khoản không ai cần lấy thêm phức tạp cấu trúc ở hai module).

**Phía mô hình, không phải RTL:** N1 phải in **hai lần nhìn cạnh nhau** theo ADR-0008 §2.2 — lần 1 đạt 2/4, lần 2 đạt **3/4**. Bản trước ghi "2/4, số 3/4 là SAI" và điều đó dẫn bảng CŨ; xem `docs/kien-truc-dong-xu-ly.html` §E1-1 và `90-results/KET-QUA-DS2-LAN-2.md`.

**Formal chưa với tới:** nửa còn lại của bất biến ADR-0013 — *byte trong biên đó đã
được một lớp trước ghi* — cần cả sequencer trong phạm vi chứng minh. Ở mức module
thứ chứng minh được là chỉ số nằm trong biên. Đó là P6.

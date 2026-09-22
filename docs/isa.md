# Đặc tả tập lệnh đồng xử lý ECG — 12 opcode trên CV-X-IF

**Pha:** P4.1 · **Trạng thái:** bản thảo, đóng băng ở mốc T6
**Nguồn chân lý của mã lệnh:** `40-rtl/include/ecg_pkg.sv` — tài liệu này giải thích, gói đó
quyết định. Hai bên lệch nhau thì gói đúng.
**Sinh lại bảng dùng opcode:** `python3 tools/opcode_audit.py`

---

## 1. Vì sao 12 lệnh, và vì sao từng lệnh tồn tại

ADR-0001 §2.3 đặt ra một quy tắc: mỗi opcode phải có ít nhất một mô hình dùng tới, opcode không
ai dùng thì loại khỏi đặc tả. `lee2026tbiocas` — công trình đối sánh gần nhất — chỉ dùng 5 lệnh,
nên con số 12 phải biện minh được từng cái, không được để phản biện hỏi "vì sao nhiều gấp đôi".

Bảng dưới **sinh từ đồ thị `torch.fx` của bốn mô hình ràng buộc đã niêm phong**, không từ danh
sách khai báo tay. Cột "lớp đại diện" là một lớp thật, gọi tên được, trong `30-model/champion/`.

| # | Opcode | Họ dùng | Lần | Lớp đại diện | Vì sao không gộp được vào lệnh khác |
|---|---|---|---|---|---|
| 0 | `CONV1D` | m1 m2 m3 m4 | 14 | `m1.features.0.0` | phép cơ bản; k ∈ {3,5,7}, stride ∈ {1,2} |
| 1 | `DWCONV` | m3 | 2 | `m3.blocks.0.dw.0` | `groups = cin = cout`: mỗi kênh một kernel riêng, đường dữ liệu **không** dùng lại được của CONV1D |
| 2 | `PWCONV` | m2 m3 m4 | 6 | `m2.blocks.0.skip.0` | k = 1: không cần line buffer, biến thành phép nhân ma trận trên trục kênh |
| 3 | `MAXPOOL` | m1 m4 | 4 | `m1.features.1` | k = 2 (m1) và k = 3 stride 2 pad 1 (m4) — hai cấu hình, nên k và stride phải là trường |
| 4 | `GAP` | m1 m2 m3 m4 | 4 | `m*.pool` | trung bình toàn trục thời gian; chia bằng dịch bit nếu độ dài là luỹ thừa 2, ở đây **không phải** (8, 32) |
| 5 | `ADD` | m2 | 2 | `m2.blocks.0` residual | cộng hai tensor từ hai vùng đệm khác nhau; ép `skip_src` tồn tại |
| 6 | `CONCAT` | m1 m2 m3 m4 | 6 | `m4.inc1` (3 nhánh) + hợp nhất RR (cả bốn) | nối theo trục kênh; ép descriptor chở danh sách nguồn |
| 7 | `FC` | m1 m2 m3 m4 | 12 | `m*.head`, `m*.extra_enc.net.0/2` | ba FC mỗi model: hai của bộ mã hoá RR, một của đầu phân lớp |
| 8 | `REQUANT` | m1 m2 m3 m4 | 34 | sau mọi conv và FC | int32 → int8; thang **per-channel** ở một số họ (ADR-0010 §7) |
| 9 | `RELU` | — (xem §3) | 30 chỗ, gộp | `m1.features.0.2` | kích hoạt duy nhất trong cả bốn họ |
| 10 | `LOADW` | — (mặt phẳng điều khiển) | — | — | nạp trọng số một lớp từ SRAM trọng số vào mảng PE |
| 11 | `STORE` | — (mặt phẳng điều khiển) | — | — | rào chắn đồng bộ và đường trả kết quả về core (§4.3) |

**Chín opcode đầu suy trực tiếp từ đồ thị và cả chín đều có chủ.** Ba cái cuối không phải lớp
mạng, nên không đời nào suy ra được bằng cách duyệt danh sách lớp — §3 xử lý dứt điểm chúng.

Hai chỗ bảng `30-model/KET-QUA-P3.md` §5 ghi sai, phát hiện khi điểm danh lại bằng đồ thị:

- `CONCAT` được ghi cho riêng Inception. Thực tế **cả bốn họ** dùng, vì `forward` của cả bốn nối
  nhánh RR vào vector hình thái. Đây không phải chi tiết vụn: nó nghĩa là `CONCAT` nằm trên
  đường đi của mọi lần suy luận, không phải một lệnh chỉ Inception cần.
- `PWCONV` được ghi cho MobileNet và Inception. ResNet cũng dùng, ở nhánh skip 1×1 khi số kênh
  đổi (`blocks.*.skip.0`).

---

## 2. Mã hoá

Không gian **custom-0** của RISC-V, `opcode[6:0] = 7'b0001011`. Chọn custom-0 vì CV32E40X
chuyển toàn bộ vùng này ra CV-X-IF mà không cần sửa lõi — ADR-0003 §2.6 cấm sửa lõi trong kho
này.

```
 31    27 26  25 24    20 19    15 14  12 11   7 6           0
┌────────┬──────┬────────┬────────┬──────┬──────┬─────────────┐
│ ECG_OP │ resv │  rs2   │  rs1   │ fn3  │  rd  │  0001011    │
└────────┴──────┴────────┴────────┴──────┴──────┴─────────────┘
    5       2       5        5       3      5         7
```

- `ECG_OP[4:0]` — số hiệu opcode trong bảng §1. Năm bit cho 12 lệnh là thừa; chỗ trống là cố ý,
  vì P5 có thể cần lệnh gỡ lỗi mà không phải đổi khuôn mã hoá.
- `fn3` — lớp lệnh: `3'b000` tính toán, `3'b001` điều khiển. Cùng một `ECG_OP` ở hai lớp là hai
  lệnh khác nhau; nhờ vậy `STORE` có cả biến thể ghi và biến thể rào chắn mà không tốn opcode.
- `rs1` — con trỏ tới Layer Descriptor của lớp cần chạy (offset trong Layer Descriptor store).
- `rs2` — từ điều khiển: chỉ số bộ đệm nguồn/đích, ghi đè stride, cờ tức thời.
- `rd` — thẻ (handle) của lệnh, dùng để hỏi trạng thái. `rd = x0` nghĩa là không cần thẻ.

**Toàn bộ tham số hình dạng — k, stride, số kênh, thang requant — nằm trong descriptor, không
nằm trong lệnh.** Đó là lý do tồn tại của descriptor: đổi mô hình là đổi bảng mô tả, không phải
sinh lại chương trình. Nếu hình dạng nằm trong lệnh thì T_switch sẽ bao gồm cả việc nạp lại mã,
và N3 hết khả thi.

---

## 3. Ba opcode "ngầm định" — quyết định dứt điểm

`00-admin/timeline/pha-va-moc.md` §T6 ghi *"9 tường minh + 3 ngầm định (LOADW, RELU, STORE)"*.
"Ngầm định" là cách nói lảng: không thể lập ngân sách chu kỳ cho một lệnh chưa biết ai gọi.
P4.0 kết luận như sau, và đây là phát biểu thay thế cho chữ "ngầm định".

### 3.1 `RELU` — là lệnh thật, nhưng thường được gộp

ReLU xuất hiện 30 chỗ trong bốn mô hình, luôn ngay sau một REQUANT. Ở int8 đối xứng, ReLU là
phép kẹp về 0, tức **một phép so sánh dấu** — gộp vào tầng requant tốn gần như không gì thêm.

**Quyết định:** `RELU` giữ chỗ trong ISA như một lệnh phát được, *và* Layer Descriptor có trường
`act` chọn việc gộp nó vào REQUANT. Ba mươi chỗ trong bốn mô hình hiện tại **đều dùng đường
gộp**; lệnh rời tồn tại cho trường hợp kích hoạt đứng một mình mà đồ thị hiện tại không có.

Đây là chỗ duy nhất trong đặc tả này cố tình giữ một lệnh mà đồ thị hiện tại không gọi tới, nên
phải nói thẳng cái giá: nó vi phạm tinh thần ADR-0001 §2.3. Lý do giữ là hai điều cùng lúc —
số 12 đã được đóng băng ở T6 và được viện dẫn ở bốn tài liệu, còn phần logic thêm vào cho lệnh
rời là một bộ so sánh mà tầng requant đã có. **Nếu hội đồng phản biện điểm này, câu trả lời
trung thực là: 9 lệnh tính toán có chủ theo đồ thị, 2 lệnh điều khiển, 1 lệnh giữ chỗ.**

### 3.2 `LOADW` — mặt phẳng điều khiển, có chủ thật

Bốn mô hình cùng thường trú (N6 ≥ 4) nhưng mảng PE chỉ có một bộ thanh ghi trọng số. Mỗi lớp
phải nạp trọng số của nó trước khi tính. `LOADW` là lệnh làm việc đó: `rs1` trỏ descriptor,
sequencer đọc `w_base`/`w_len` rồi kéo từ SRAM trọng số.

`LOADW` **không chặn**. Nó là hạng mục chính có thể che lấp sau tính toán: trong khi lớp *i*
đang chạy, trọng số lớp *i + 1* nạp song song. Việc che lấp này là điều kiện cần để T_infer
không bị cộng dồn thời gian nạp — P4.4 tính con số.

### 3.3 `STORE` — rào chắn đồng bộ, và đó là công việc thật của nó

Mọi lệnh tính toán ở §1 đều **không chặn**: CV-X-IF nhận, đẩy vào FIFO của sequencer, trả thẻ
vào `rd` rồi cho core đi tiếp. Nếu không có gì đồng bộ, core không biết khi nào logit sẵn sàng.

`STORE` có hai biến thể phân biệt bằng `fn3`:

- `fn3 = 000` — ghi vùng đệm kích hoạt ra bộ nhớ ngoài qua AXI4/DMA. Dùng khi gỡ lỗi và khi
  xuất tap để đối chiếu vector vàng ở P6.
- `fn3 = 001` — **rào chắn**: chặn core tới khi FIFO của sequencer rỗng, rồi trả kết quả ở
  `rd`. Đây là lệnh duy nhất trong ISA chặn core, và mỗi lần suy luận một nhịp gọi nó đúng một
  lần, ở cuối.

Nhờ cách này `LOADW` và `STORE` không phải "lệnh giữ cho bảng nhìn oai" — chúng là hai đầu của
mô hình thực thi không chặn, và mô hình đó chính là thứ làm T_switch nhỏ được.

---

## 4. Mô hình thực thi

### 4.1 Một nhịp, nhìn từ phía firmware

```
        core (CV32E40X)                     sequencer + mang PE
  ┌───────────────────────────┐
  │ ecg.loadw  d0             │──────►  nap trong so lop 0 (khong chan)
  │ ecg.conv1d d0, buf0       │──────►  vao FIFO, tra the vao rd
  │ ecg.loadw  d1             │──────►  nap lop 1 trong khi lop 0 dang chay
  │ ...                       │
  │ ecg.store.barrier x10     │──────►  chan tai khi FIFO rong, tra logit
  └───────────────────────────┘
```

Firmware không đợi giữa các lệnh. Chuỗi lệnh cho một nhịp là **cố định theo mô hình** và nằm
trong flash — đổi mô hình không sinh lại chuỗi này, chỉ đổi con trỏ bảng descriptor.

### 4.2 Chuyển mô hình

Chuyển mô hình **không** phát lệnh tính toán nào. Nó là: ghi thanh ghi `model_sel`, sequencer
đọc Model Descriptor mới, nạp lại bảng Layer Descriptor và tham số requant, xử lý vùng đệm.
Toàn bộ ngân sách N3 nằm ở đây, và P4.4 tính nó.

### 4.3 Chi phí phát lệnh

| Việc | Chu kỳ | Ghi chú |
|---|---|---|
| Phát một lệnh, CV-X-IF nhận | 1 | không chặn, đường issue rộng một lệnh |
| `store.barrier` khi FIFO đã rỗng | 2 | trả `rd` |
| `store.barrier` khi còn việc | tới khi xong | đại lượng này là T_infer, không phải T_switch |

---

## 5. Rào chắn N9 — điều P4 quyết mà P5 chỉ đo lại

N9 đòi ≥ 90 % logic dùng chung giữa bốn họ. P5 chỉ đo được cái P4 cho phép. Ba chỗ trong đặc tả
này có nguy cơ sinh đường dữ liệu riêng cho một họ, ghi ra đây để P4.5 rà và P5 không phát hiện
muộn:

| Chỗ | Ai đòi | Đã xử thế nào |
|---|---|---|
| `DWCONV` — mỗi kênh một kernel | chỉ m3 | dùng lại mảng PE của CONV1D với `groups` làm cờ định tuyến, **không** thêm mảng |
| `CONCAT` ba nhánh và nhiều bộ đệm ra | m4 (m1–m3 chỉ nối hai) | nối là phép **định địa chỉ**, không phải phép tính: thực hiện bằng cách các nhánh ghi vào offset khác nhau của cùng một bộ đệm ⇒ không tốn logic |
| `ADD` với `skip_src` | chỉ m2 | tái dùng cây cộng của tầng psum |

`CONCAT` không tốn logic là điểm mạnh nhất của thiết kế này và cần nói rõ trong luận văn: chi
phí của nó nằm ở bố cục bộ đệm mà descriptor mô tả, không ở phần cứng.

---

## 6. Rào chắn N9 — kết quả rà (P4.5)

**Sinh lại:** `python3 tools/n9_audit.py` · **Bảng:** `90-results/tables/n9-audit.csv`

N9 đòi ≥ 90 % logic dùng chung và được chốt ở P5 bằng báo cáo tổng hợp. Rà đặc tả trước khi
viết RTL cho kết quả:

| Khối | Họ dùng | % gate ước lượng |
|---|---|---|
| Mảng PE 8 × MAC int8 | cả bốn | 42,3 |
| Bộ tích luỹ psum int32 | cả bốn | 11,0 |
| Sequencer FSM + đọc descriptor | cả bốn | 9,3 |
| Bộ sinh địa chỉ + bơm 0 biên | cả bốn | 7,6 |
| Tầng requant (nhân 11 bit + dịch) | cả bốn | 6,3 |
| LOADW DMA · shim CV-X-IF · STORE | cả bốn | 13,5 |
| **Nạp thang requant per-channel** | m1 m2 | 3,0 |
| **Bộ so sánh MAXPOOL** | m1 m4 | 2,3 |
| **Cổng đọc thứ hai cho ADD/skip** | m2 | 2,7 |
| **Định tuyến DWCONV (groups)** | m3 | 1,9 |

**Dùng chung cả bốn họ: 90,1 %.** Đạt ngưỡng, nhưng **đúng 0,1 điểm** — tức không có biên nào.

Phải nói rõ ba điều về con số này, vì nó rất dễ bị đọc quá tự tin:

1. **Đây là ước lượng trước tổng hợp, không phải số đo.** Trọng số gate là tỉ lệ tương đối do
   người viết đặt, có ghi cơ sở từng dòng trong `tools/n9_audit.py`. Số chốt đến từ báo cáo tổng
   hợp ở P5. Với biên 0,1 điểm, **N9 nay là ngưỡng có rủi ro thật**, không phải ngưỡng dư dả.
2. **N9 nói về logic, không nói về SRAM.** Trong một thiết kế 24 kB bộ nhớ trên 130 nm, SRAM
   chiếm phần lớn diện tích, và SRAM thì dùng chung 100 %. Nếu N9 được phát biểu theo diện tích
   thì nó đạt dễ; theo logic thì mới sát. Luận văn phải nói rõ đang tính theo cái nào.
3. **Bốn khối riêng đều nhỏ và không khối nào là đường dữ liệu song song.** Không có "mảng PE
   thứ hai cho MobileNet" — `DWCONV` chỉ thêm một mux định tuyến, `CONCAT` không thêm gì
   (§5), `ADD` thêm một cổng đọc. Đây là điều P4 phải bảo đảm và đã bảo đảm.

Một hệ quả ngược mà thứ tự hy sinh ở `pha-va-moc.md` §4 không nói tới: **loại một họ không làm
tăng tỉ lệ dùng chung, nó tạo ra logic chết.** Loại m2 thì 65 gate cổng đọc thứ hai thành vô
dụng; loại m3 thì 45 gate định tuyến DWCONV thành vô dụng. Cắt phạm vi để cứu N6 vì thế còn
làm N9 xấu đi nếu RTL đã viết xong.

## 7. Còn phải quyết ở các bước sau

- **P4.2** — thang requant per-channel nằm trong descriptor hay trong vùng trọng số. Ảnh hưởng
  ngân sách 1,0 kB bảng mô tả.
- **P4.4** — độ sâu FIFO sequencer. Quá nông thì core phải chờ, quá sâu thì tốn thanh ghi.
- ~~**P4.4** — có thật cần xoá toàn bộ 4 kB bộ đệm khi chuyển mô hình~~ → **xong, ADR-0013:**
  bơm 0 ở bộ sinh địa chỉ, T_switch 610 → 98 chu kỳ.
- ~~**P4.2** — thang requant per-channel nằm đâu~~ → **xong, ADR-0012:** hạng mục riêng, 2 B.

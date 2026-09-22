# Kết quả P4 · Đặc tả tập lệnh và descriptor

Chạy 2026-09-01. Sinh lại toàn bộ: `make isa` và `make int-ref`.
Bốn bảng số: `90-results/tables/opcode-usage.csv` · `descriptor-budget.csv` ·
`cycle-budget.csv` · `int-reference.csv` · `n9-audit.csv`

Tài liệu đặc tả: `40-rtl/docs/isa.md` · `descriptor.md` · `ngan-sach-chu-ky.md`
Nguồn chân lý mã lệnh: `40-rtl/include/ecg_pkg.sv`
Quyết định mới: **ADR-0012** (hạng mục thang requant) · **ADR-0013** (bơm 0 thay xoá bộ đệm)

---

## 1. Bảng tổng — sáu điều kiện xong P4

| Điều kiện | Trạng thái |
|---|---|
| Bảng opcode sinh bằng lệnh, mỗi opcode có lớp cụ thể dùng | Đạt — 9 suy từ đồ thị, 3 điều khiển |
| `ecg_pkg.sv` là nguồn chân lý duy nhất | Đạt |
| Bốn model tuần tự hoá thành bảng mô tả, tổng ≤ 1,0 kB | Đạt — 864 B, cộng 512 B thang (ADR-0012) |
| Blob nhị phân chạy lại **đúng từng bit** vector vàng | Đạt — bit-exact cả bốn, xem §4 |
| T_switch và T_infer có trong `cycle-budget.csv` | Đạt — 98 và 30.258 chu kỳ |
| Mọi con số sinh lại bằng một mục `make` | Đạt — `make isa` |

**Ngoài kế hoạch:** rà N9 (P4.5) cho 90,1 %, sát ngưỡng đúng 0,1 điểm — `isa.md` §6.

---

## 2. Điểm danh opcode — 9 + 3, và hai sai sót đã sửa

Suy từ đồ thị `torch.fx` của bốn model ràng buộc, không từ danh sách khai báo tay.

| Opcode | Họ dùng | Số lớp |
|---|---|---|
| CONV1D | m1 m2 m3 m4 | 14 |
| DWCONV | m3 | 2 |
| PWCONV | m2 m3 m4 | 6 |
| MAXPOOL | m1 m4 | 4 |
| GAP | m1 m2 m3 m4 | 4 |
| ADD | m2 | 2 |
| CONCAT | m1 m2 m3 m4 | 6 |
| FC | m1 m2 m3 m4 | 12 |
| REQUANT | m1 m2 m3 m4 | 34 |
| RELU · LOADW · STORE | mặt phẳng điều khiển | — |

Hai chỗ `30-model/KET-QUA-P3.md` §5 ghi sai: `CONCAT` là của **cả bốn họ** (hợp nhất nhánh RR),
không riêng Inception; và ResNet cũng dùng `PWCONV` ở nhánh skip 1×1. Cả hai chỉ lộ ra khi điểm
danh bằng đồ thị.

Ba opcode "ngầm định" nay có phát biểu dứt khoát (`isa.md` §3): `LOADW` và `STORE` là hai đầu
của mô hình thực thi **không chặn** — `STORE` biến thể rào chắn là lệnh duy nhất chặn core, gọi
đúng một lần mỗi nhịp; `RELU` là **lệnh giữ chỗ**, 30 chỗ trong bốn model đều đi đường gộp qua
trường `act`. Cái giá của việc giữ nó được ghi thẳng vào đặc tả thay vì để dành cho lúc phản biện.

---

## 3. Ngân sách — hai chỗ vỡ, cả hai đã có ADR

### 3.1 Bảng mô tả vừa, thang requant vỡ

54 bản ghi (50 lớp + 4 Model Descriptor) × 16 B = **864 B / 1.024 B**, vừa. *(Số của P4. Sau ADR-0014 §2.1 CONCAT không còn bản ghi riêng: 48 bản ghi = 768 B.)* Chỉ vừa nhờ REQUANT
và RELU **không** có bản ghi riêng; tách riêng là thêm 1.024 B.

Nhưng per-channel của ADR-0010 sinh **256 thang**. Ở 4 B là 1.024 B — lớn hơn cả bảng mô tả, và
vỡ mọi cách xếp. **ADR-0012** chốt thang là hạng mục riêng, 2 B (nhân 11 bit + dịch 5 bit):

```
24,0 kB tổng (24.576 B)
├── 18.488 B  trọng số bốn mô hình
├──  4.096 B  bộ đệm kích hoạt
├──    864 B  bảng mô tả
├──    512 B  thang requant        ← hạng mục mới
└──    616 B  dự phòng  (2,5 %)
```

**N7 (≤ 24 kB) nay là ngưỡng có rủi ro thật**, biên 2,5 %, không còn dư dả như ADR-0002 tưởng.

### 3.2 T_switch — giảm 6,2 lần

Xoá bộ đệm chiếm 512/610 chu kỳ. **ADR-0013**: không xoá; zero padding do bộ sinh địa chỉ bơm
vào đường nạp PE, vì mọi ô còn lại đều bị ghi đè trước khi đọc.

| | T_switch xấu nhất | Biên so với N3 |
|---|---|---|
| Xoá trọn 4 kB (ADR-0002) | 610 | 39 % |
| Bơm 0 ở bộ sinh địa chỉ | **98** | **90 %** |

Kèm bất biến mà P6 **phải** kiểm, không được tin: kiểm bit-exact với thứ tự model **xoay vòng**,
không phải bốn lượt riêng. Bốn lượt riêng không bao giờ thấy đường rò giữa hai model.

### 3.3 T_infer và số PE — đại lượng chưa ai tính

Xấu nhất **30.258 chu kỳ** (m4-inception, không phải ResNet dù ResNet nhiều MAC hơn), dùng 22 %
một chu kỳ lấy mẫu. Roofline gãy giữa 8 và 16 PE: từ 16 trở lên hiệu suất mảng rơi dưới 60 % vì
model chỉ có 8…72 kênh ra. **Chốt 8 PE.**

Kết luận đáng vào luận văn: **ngân sách 4.700 tham số đặt trần cho số PE có ích.** Đó là hệ quả
của một quyết định ở P0, chỉ nhìn thấy được khi có mô hình chu kỳ.

---

## 4. Trình biên dịch và vòng tròn bit-exact

`tools/compile_model.py` sinh blob rồi một trình thông dịch đọc **chỉ blob** và chạy lại:

| Model | Bản ghi | Blob | Thang | Bộ đệm | Đỉnh kích hoạt | Vòng tròn |
|---|---|---|---|---|---|---|
| m1-cnn | 10 | 4.974 B | 108 | 3 | 3.072 B | **bit-exact** |
| m2-resnet | 14 | 5.094 B | 140 | 3 | 3.072 B | **bit-exact** |
| m3-mobilenet | 10 | 4.864 B | 11 | 3 | 3.840 B | **bit-exact** |
| m4-inception | 16 | 4.868 B | 19 | 4 | 3.840 B | **bit-exact** |

Đỉnh kích hoạt **3.840 B / 4.096 B** — vừa, biên 6 %. Số bộ đệm song đồng thời nhiều nhất là 4,
nên trường `src` 4 bit đủ rộng.

**Ba lỗi đặc tả bị bắt bởi chính vòng tròn này**, và đây là lý do tồn tại của nó:

1. `act` được đặt cho conv/FC nhưng **quên khối ADD** — khối residual của m2 cũng có ReLU sau
   phép cộng. Hậu quả: giá trị âm sống sót, mọi lớp sau đó lệch.
2. **Đánh số bộ đệm bằng chỉ số bản ghi là không hợp lệ.** m4 có 16 bản ghi, trường `src` chứa
   14 giá trị. Phải cấp phát bộ đệm vật lý thật ở P4, không để dành cho P5.
3. **`src1 = 0` vừa nghĩa "bộ đệm 0" vừa nghĩa "không dùng".** Mọi bản ghi không có nguồn thứ
   hai trở thành người tiêu thụ bộ đệm 0, giữ nó sống đến hết mạng, và đỉnh kích hoạt báo
   5.888 B thay vì 3.840 B — tức **báo vỡ 4 kB khi thực ra vừa**. Nay có mã riêng `ECG_SRC_NONE`.

Cả ba đều là lỗi *đặc tả*, không phải lỗi hiện thực. Nếu để tới P6 mới phát hiện thì phải sửa
descriptor sau khi T6 đã đóng băng nó.

---

## 5. Phát hiện nặng nhất: luồng số nguyên thật đắt hơn fake quant

`quantize.py` làm **fake quant** — lượng tử hoá rồi nhân lại về float, phép tích chập chạy
float32. `30-model/src/int_ref.py` dựng luồng thật: int8 × int8 → psum int32 → nhân nguyên và
dịch bit → int8.

Hai khác biệt cơ bản, và cái thứ hai mới là cái đắt:

- **Làm tròn** của phép nhân số nguyên khác của phép nhân float, tích tụ qua 34 tầng requant.
- **`attach_act_quant` chỉ gắn sau ReLU**, nên số thang ít hơn số chỗ requant (m2: 7 thang cho
  10 chỗ) — **ba tensor của m2 hiện đang chảy ở float32**. Phần cứng không có lựa chọn đó: mọi
  biên lớp phải có một thang. `int_ref.py` hiệu chỉnh ở **mọi** biên, tức một lược đồ chặt hơn
  cái đã báo cáo.

Kết quả trên **validation** (DS2 không bị chạm, ADR-0008), bộ nhân 11 bit:

| Họ | F1 float | F1 số nguyên | Mất | Ngưỡng N1b ≤ 0,5 |
|---|---|---|---|---|
| m1-cnn | 0,6066 | 0,5911 | **+1,55** | trượt |
| m2-resnet | 0,6307 | 0,6277 | +0,30 | đạt |
| m3-mobilenet | 0,6116 | 0,6084 | +0,32 | đạt |
| m4-inception | 0,6209 | 0,5741 | **+4,68** | trượt |

Đối chiếu: ADR-0010 §7 cộng per-channel báo N1b đạt **4/4** trên validation. Luồng số nguyên
thật cho **2/4**. Nói thẳng: **N1b như đang báo cáo đo bằng một lược đồ mà phần cứng không thi
hành được, và nó lạc quan hơn thực tế 1,2 điểm với CNN và 4,4 điểm với Inception.**

Cơ chế của m4 khớp đúng phân tích ADR-0010 §3: `CONCAT` buộc bốn nhánh biên độ khác nhau dùng
**một thang chung**. Trong mô hình fake quant, các nhánh ở lại float nên cơ chế đó **chưa bao
giờ được thi hành**; ở đây nó được thi hành, và nó tốn 4,68 điểm.

### 5.1 Bộ nhân 11 bit là đủ — điều kiện của ADR-0012 thoả

| Độ rộng bộ nhân | m1 | m2 | m3 | m4 |
|---|---|---|---|---|
| 8 bit | 0,5859 | 0,6233 | 0,6089 | 0,5752 |
| **11 bit** | 0,5911 | 0,6277 | 0,6084 | 0,5741 |
| 16 bit | 0,5921 | 0,6274 | 0,6110 | 0,5749 |
| 32 bit | 0,5920 | 0,6274 | 0,6095 | 0,5749 |

Từ 11 lên 32 bit đổi ≤ 0,10 điểm ở m1/m2/m4 và 0,26 ở m3 (không đơn điệu, tức là nhiễu). **Thang
2 B đứng vững** — ngân sách của ADR-0012 không phải tính lại.

Điều này quan trọng vì nó tách bạch hai nguyên nhân: mất mát **không** đến từ độ phân giải bộ
nhân, mà đến từ việc **có thang ở mọi biên lớp** và từ thang chung của CONCAT.

### 5.2 Việc phải làm, và ranh giới không được vượt

Ba hướng, theo thứ tự nên thử, **tất cả chỉ trên validation**:

1. **Phân vị hiệu chỉnh riêng cho những biên mới.** Hiện `int_ref.py` dùng một phân vị cho mọi
   biên của một họ. Với m4 phân vị là 100,0 (không cắt) — hợp lý cho CONCAT nhưng có thể sai cho
   các biên khác. Đây là hướng rẻ nhất.
2. **Thang riêng cho từng nhánh trước CONCAT.** Trường `dst_off` và `rq_base` đã cho phép mỗi
   nhánh có bộ nhân riêng khi ghi vào bộ đệm chung — tức phần cứng **không** buộc bốn nhánh dùng
   một thang, chỉ cần trình biên dịch cấp thang riêng. Đây là hướng đúng cho m4 và đã có chỗ
   trong descriptor.
3. Chấp nhận và báo cáo, theo đúng `pha-va-moc.md` §3.

**Ranh giới:** DS2 đã mở một lần và `accuracy-recipe.csv` là con số báo cáo. Nếu sửa được và cần
đo lại DS2, lần đó phải khai là **lần nhìn thứ hai** và luận văn in cả hai con số cạnh nhau
(ADR-0008 §2). Việc này **chưa làm** và cần quyết định của người hướng dẫn.

---

## 6. Còn tồn

- **N1b phải phát biểu lại theo luồng số nguyên** (§5). Đây là việc chặn, vì N2 bit-exact kiểm
  RTL với vector vàng, mà vector vàng hiện tại là float. Cần sinh lại `30-model/golden/` với tap
  số nguyên — làm được vì **T6 chưa qua**.
- **Lệch tài liệu/mã:** ADR-0010 §7 ghi `CALIB_PERCENTILE = {m1: 99.98, …}`, `quantize.py:116`
  ghi `99.95`. Mọi số ở §5 dùng giá trị trong mã. Một trong hai sai và nó ảnh hưởng vector vàng.
- **N9 biên 0,1 điểm** (`isa.md` §6). Số chốt ở P5.
- **CONCAT tính 0 chu kỳ** trong `cycle_model.py` là đúng **với điều kiện** P5 gộp phép ha thang
  của từng nhánh vào chính requant của nhánh đó qua `dst_off`. Chưa gộp thì CONCAT tốn một lượt
  quét tensor. Ràng buộc này phải nằm trong kế hoạch P5.

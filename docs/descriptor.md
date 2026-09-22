# Model Descriptor và Layer Descriptor

**Pha:** P4.2 · **Trạng thái:** bản thảo, đóng băng ở mốc T6
**Trường bit:** `40-rtl/include/ecg_pkg.sv` · **Sinh lại số:** `python3 tools/build_descriptors.py`
**Bảng số:** `90-results/tables/descriptor-budget.csv`

---

## 1. Bảng mô tả là gì và vì sao nó là toàn bộ luận điểm của đề tài

RQ0 hỏi: chuyển giữa bốn mô hình khác kiến trúc tốn bao nhiêu. Câu trả lời chỉ nhỏ được nếu
**chuyển mô hình = đổi bảng mô tả**, chứ không phải nạp lại chương trình hay cấu hình lại đường
dữ liệu. Vì vậy hai định dạng trong tài liệu này chở **toàn bộ** thứ khác nhau giữa bốn mô hình,
còn chuỗi lệnh và phần cứng thì giữ nguyên.

Ngân sách: **≤ 1,0 kB cho bảng mô tả của cả bốn mô hình** (ADR-0002 §4.1).

---

## 2. Số bản ghi thật — đếm được, không ước lượng

ADR-0002 §4.2 giả định "≈ 12 lớp × 16 B". Đếm từ đồ thị bốn mô hình đã niêm phong:

| Mô hình | Số lớp | Bản ghi (lớp + 1 Model Descriptor) | Byte |
|---|---|---|---|
| m1-cnn | 10 | 11 | 176 |
| m2-resnet | 14 | 15 | 240 |
| m3-mobilenet | 10 | 11 | 176 |
| m4-inception | 16 | 17 | 272 |
| **Tổng** | **50** | **54** | **864** |

> **Cập nhật (ADR-0014 §2.1).** CONCAT không còn bản ghi riêng — mỗi nhánh ghi thẳng vào một dải kênh qua `dst_off`/`cat_part` — nên 6 lớp CONCAT biến mất: **48 bản ghi × 16 B = 768 B**. `make n7` đọc từ blob thật và đo đúng 768 B. Bảng trên là số của P4.

**864 B / 1.024 B — vừa, còn 160 B.** Con số này vừa được nhờ hai phép gộp, và nếu bỏ một trong
hai thì vỡ ngân sách:

- **REQUANT không có bản ghi riêng.** Nó là trường của chính lớp sinh ra psum. Có 34 chỗ
  requant; tách riêng là thêm 544 B.
- **RELU không có bản ghi riêng.** Nó là trường `act`, đúng theo `isa.md` §3.1. Có 30 chỗ; tách
  riêng là thêm 480 B.

Ước lượng 12 lớp của ADR-0002 sai ở phía an toàn với m1/m3 (10) và sai ở phía nguy hiểm với m4
(16). Inception là mô hình đắt nhất về descriptor, đúng như dự đoán ở ADR-0002 §8.

---

## 3. Layer Descriptor — 114 bit trong ban ghi 16 B

Mỗi trường phải chỉ ra được một lớp thật đòi nó tồn tại. Đây là ADR-0001 §2.3 áp cho trường
descriptor, không riêng cho opcode.

| Trường | Bit | Lớp nào đòi nó |
|---|---|---|
| `op` | 5 | `ecg_pkg::ecg_op_e` |
| `act` | 2 | 30 chỗ ReLU của cả bốn họ, đi đường gộp |
| `src0` | 4 | mọi lớp — 0…12 là bộ đệm, 13 `NONE`, 14 cổng vào ECG, 15 cổng vào RR |
| `src1` | 4 | `m2.blocks.*` — chính là `skip_src` của ADR-0001; và nhánh thứ hai của CONCAT |

Ba mã đặc biệt của trường `src` không phải trang trí. `src1 = 0` vừa nghĩa "bộ đệm 0" vừa nghĩa
"không dùng" đã làm mọi bản ghi không có nguồn thứ hai trở thành người tiêu thụ bộ đệm 0, giữ nó
sống đến hết mạng, và đỉnh kích hoạt báo 5.888 B thay vì 3.840 B — **báo vỡ 4 kB khi thực ra
vừa**. Lỗi này bị bắt bởi vòng tròn kiểm tra ở P4.3, không bởi suy luận.
| `dst` | 4 | mọi lớp |
| `dst_off` | 10 | `m4.inc1/inc2` — ba nhánh ghi vào ba offset kênh của **cùng một** bộ đệm |
| `cin` `cout` | 9 + 9 | rộng nhất: 88 (`m3.head` vào), 72 (`m3.blocks.1.pw` ra) |
| `len_in` `len_out` | 10 + 10 | rộng nhất 256 (đầu vào), 128 |
| `k` | 3 | k ∈ {1, 3, 5, 7}; MAXPOOL của m1 là 2, của m4 là 3 |
| `stride` | 2 | stride ∈ {1, 2} |
| `pad` | 3 | `m4.inc1.b3.0` cần pad = 1 trên MAXPOOL k = 3 |
| `dw` | 1 | `m3.blocks.*.dw` — groups = cin |
| `w_base` | 14 | offset trong SRAM trọng số |
| `rq_base` `rq_n` | 14 + 9 | `rq_n` = 1 khi per-tensor, = cout khi per-channel (§4) |
| `last` | 1 | lớp cuối, để sequencer biết khi nào rào chắn mở |

**114 bit, còn trống 14 bit trong 16 B.** Chỗ trống là cố ý: P5 gần như chắc chắn cần thêm một
cờ mà bây giờ chưa thấy, và mở rộng bản ghi lên 24 B là vỡ ngân sách ngay.

`dst_off` là trường đáng nói nhất. Nhờ nó, **CONCAT không tốn logic tính toán**: ba nhánh của
Inception không nối gì cả, chúng chỉ ghi vào ba vùng kênh khác nhau của cùng một bộ đệm, và
"phép nối" xảy ra vì lớp sau đọc trọn bộ đệm. Đây là lập luận N9 mạnh nhất mà thiết kế có
(`isa.md` §5), và nó nằm gọn trong 10 bit của descriptor.

## 4. Model Descriptor — 87 bit

| Trường | Bit | Ghi chú |
|---|---|---|
| `n_layers` | 6 | tối đa 16 hiện tại, 6 bit cho dư địa |
| `ld_base` | 10 | bản ghi Layer Descriptor đầu tiên |
| `w_base` `w_len` | 14 + 14 | vùng trọng số của mô hình này |
| `act_amax` | 16 | thang kích hoạt đầu vào; **khác nhau theo họ** vì phân vị hiệu chỉnh khác nhau (ADR-0010 §7) |
| `in_len` | 10 | 256 |
| `n_class` | 4 | 5 |
| `clear_len` | 13 | số byte bộ đệm phải xử lý khi chuyển sang mô hình này — trường này là chỗ P4.4 gắn kết luận về T_switch vào |

---

## 5. Thang requant per-channel — ngân sách 1,0 kB **vỡ**, và cách xử

ADR-0010 §7 bật per-channel cho m1 và m2 (`PER_CHANNEL_WEIGHTS = {m1: True, m2: True,
m3: False, m4: False}`). Đếm số thang thật:

| Mô hình | Chế độ | Số thang |
|---|---|---|
| m1-cnn | per-channel | 105 |
| m2-resnet | per-channel | 133 |
| m3-mobilenet | per-tensor | 8 |
| m4-inception | per-tensor | 10 |
| **Tổng** | | **256** |

Ở 4 B mỗi thang là **1.024 B — nhiều hơn cả bảng mô tả**. Để chung với bảng mô tả thì tổng
1.888 B, **vỡ ngân sách 1,0 kB gần gấp đôi**.

Đây là hệ quả mà ADR-0002 §4.1 không thể lường, vì per-channel là quyết định của ADR-0010 —
ra sau, dựa trên số đo. Ba phương án, kèm số thật:

| Phương án | Kết quả |
|---|---|
| Thang nằm trong vùng trọng số | trọng số 18.488 B + 1.024 B = **19.512 B > 18,4 kB (18.842 B)** — vỡ ngân sách con |
| Thang là hạng mục riêng, 4 B | cần 1.024 B, chỗ dư có được là 160 B (bảng mô tả) + 614 B (dự phòng) = **774 B** — vỡ |
| **Thang là hạng mục riêng, 2 B** (nhân 11 bit + dịch 5 bit) | cần **512 B ≤ 774 B** — **vừa**, còn dư 262 B |

**Quyết định đề xuất: hạng mục riêng, 2 B mỗi thang.** Ngân sách 24 kB thành:

```
24,0 kB tổng (24.576 B)
├── 18.488 B  trong so bon mo hinh (do dem tu champion, khong uoc luong)
├──  4.096 B  bo dem kich hoat
├──    864 B  bang mo ta (54 ban ghi x 16 B)
├──    512 B  thang requant (256 thang x 2 B)
└──    616 B  du phong
```

Tổng 23.960 B / 24.576 B. **Vừa, còn 616 B dự phòng** — tức 2,5 %, mỏng hơn con số 0,6 kB mà
ADR-0002 dự trù nhưng vẫn còn.

Việc này cần một ADR, vì nó **sửa bảng ngân sách của ADR-0002 §4.1** bằng cách thêm một dòng
hạng mục mà bảng đó không có.

---

## 6. Hai vấn đề phải xử trước T6, không được để tới P6

### 6.1 Vector vàng là số thực, không phải số nguyên — **đã dựng đường số nguyên**

**Cập nhật:** đường tham chiếu số nguyên đã có ở `30-model/src/int_ref.py`, và trình biên dịch
`tools/compile_model.py` chạy lại **đúng từng bit** cho cả bốn model. Kết quả và hệ quả về N1b:
`40-rtl/KET-QUA-P4.md` §5. Việc còn lại là sinh lại `30-model/golden/` với tap số nguyên.


`quantize.py` hiện làm **fake quant**: `torch.clamp(torch.round(x / scale), -127, 127) * scale`
— lượng tử hoá rồi nhân lại về float, và phép tích chập chạy bằng float32. Vector vàng trong
`30-model/golden/` do đó là **tensor float32**, không phải psum int32 hay kích hoạt int8.

RTL ở P5 sẽ tính psum int32 rồi requant bằng nhân số nguyên và dịch bit. Hai đường tính này
**không cho ra cùng một bit**: làm tròn của phép nhân int khác làm tròn của phép nhân float, và
sai lệch tích tụ qua 34 tầng requant.

Nghĩa là **N2 "bit-exact" hiện chưa kiểm được như đang phát biểu** — không có gì để so bit với.
Hai đường xử:

1. **Sinh thêm đường tham chiếu số nguyên** trong `quantize.py`: mô phỏng đúng luồng
   int8 × int8 → int32 → nhân-dịch → int8 của phần cứng, và ghi tap số nguyên vào vector vàng.
   Vector float hiện có vẫn giữ, để đối chiếu mất mát.
2. Hoặc phát biểu lại N2 thành sai số bao dung (ví dụ ≤ 1 LSB mỗi tap).

**Nên chọn (1).** Bit-exact là ngưỡng bắt buộc của đề tài (`pha-va-moc.md` §3) và là thứ làm
luận điểm kiểm chứng mạnh; hạ nó xuống sai số bao dung là mất một trong bốn điều kiện cần.
Quan trọng hơn: **T6 chưa qua**, nên sửa được bây giờ mà không phải trả giá "đổi vector vàng
sau khi đóng băng". Để tới P6 thì hết đường lùi.

### 6.2 Chú thích trong `quantize.py` đã lạc hậu và nói ngược lại thiết kế

Docstring của `quantize.py` viết: *"Per-tensor, not per-channel: the coprocessor carries one
scale per layer, and a scheme the hardware cannot execute is not a measurement of anything."*

Lập luận đó đúng lúc viết, nhưng ADR-0010 §7 đã bật per-channel cho hai họ dựa trên số đo, và
§5 ở trên vừa chứng minh phần cứng **thi hành được** per-channel trong ngân sách. Chú thích phải
sửa, nếu không nó là một phát biểu sai nằm trong mã nguồn — đúng loại thứ mà người đọc lại sau
sáu tháng sẽ tin.

---

## 7. Còn phải quyết

- ADR mới cho hạng mục "thang requant" trong bảng ngân sách 24 kB (§5).
- Đường tham chiếu số nguyên cho vector vàng (§6.1) — **chặn P6**.
- `clear_len` lấy giá trị nào: phụ thuộc kết luận P4.4 về việc có phải xoá trọn 4 kB.

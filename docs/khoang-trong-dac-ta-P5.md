# Ba khoảng trống đặc tả phát hiện khi bắt đầu P5.2

Phát hiện 2026-09-01, khi thiết kế `ecg_actbuf` và cần biết địa chỉ nền của từng bộ đệm.
Cả ba đều là **lỗi đặc tả, không phải lỗi hiện thực**, nên phải xử ở P4 chứ không ở P5.

Lý do ghi ra thay vì viết tiếp: RTL chỉ đọc blob. Nếu blob không đủ thông tin thì sequencer
phải đoán, và cái đoán đó sẽ biểu hiện thành "N2 trượt" ở P6 — nơi khó truy nhất.

---

## 1. `dst_off` được đặc tả nhưng chưa bao giờ được dùng

`40-rtl/docs/isa.md` §5 nói về `CONCAT`:

> nối là phép **định địa chỉ**, không phải phép tính: thực hiện bằng cách các nhánh ghi vào
> offset khác nhau của cùng một bộ đệm ⇒ không tốn logic

Và gọi đó là "điểm mạnh nhất của thiết kế này".

Nhưng trong `tools/compile_model.py`, `dst_off` **luôn bằng 0** — năm chỗ khởi tạo đều là
`dst_off=0`, không chỗ nào đặt giá trị khác. Trình thông dịch thực hiện `CONCAT` bằng cách
**gom** qua `meta[i]["srcs"]` rồi `np.concatenate`.

Nói cách khác: tài liệu tả một cơ chế, mã hiện thực một cơ chế khác, và trường 10 bit dành cho
cơ chế trong tài liệu đang nằm không.

**Hệ quả nếu không xử:** N9 sẽ khác con số đã báo. Rà P4.5 tính 90,1 % **với giả định `CONCAT`
không tốn logic**. Nếu phần cứng phải gom ba nguồn thì nó cần đường đọc thứ ba, và giả định đó
sai.

## 2. `CONCAT` ba nhánh không mã hoá được trong hai trường nguồn

Layer Descriptor chỉ có `src0` và `src1`. Nhưng m4 có hai lớp `cat` nối **ba** nhánh:

| Lớp | Hình dạng ra | Số nhánh |
|---|---|---|
| `m4.inc1.cat` | [1, 30, 64] | 3 × 10 kênh |
| `m4.inc2.cat` | [1, 24, 32] | 3 × 8 kênh |

Hai trường không chứa được ba nguồn. Hiện tại trình thông dịch lấy từ `meta`, mà `meta` **không
nằm trong blob**.

Điều này thực ra **củng cố** phương án ở §1: nếu ba nhánh tự ghi vào ba `dst_off` của bộ đệm
dùng chung thì bản ghi `CONCAT` không cần nguồn nào cả, và giới hạn hai trường không còn là vấn
đề. Đó là lý do nên sửa theo hướng tài liệu, không theo hướng mã hiện tại.

## 3. Blob không có bảng địa chỉ nền bộ đệm

`allocate_buffers` cấp phát 13 số hiệu bộ đệm logic và báo đỉnh 3.840 B, nhưng blob chỉ chứa:

```
MAGIC | version | n_recs | w_size | n_scales | ModelDesc | LayerDesc[] | weights | scales
```

Không có chỗ nào nói bộ đệm số `n` bắt đầu ở byte nào. Các bộ đệm có kích thước rất khác nhau
(256 B cho cửa sổ vào, 2.304 B cho `72 × 32` của MobileNet), nên chia đều 3.840 / 13 = 295 B
không dùng được.

Phần cứng cần `base[13]`, mỗi giá trị 12 bit để địa chỉ hoá 3.840 B: **13 × 12 bit ≈ 20 B mỗi
mô hình, 80 B cho bốn mô hình.** Ngân sách N7 còn dư 872 B, nên thêm được — nhưng phải tính vào
bảng §0.1 của `KE-HOACH-P5.md`, đưa tổng lên 23.784 B.

## 4. Còn hai thứ trong `meta` mà blob không có

`run_blob` nhận `meta` và docstring của nó đã tự cảnh báo:

> `meta` chỉ chứa thứ blob có nhưng không mã hoá được trong 16 B... Nếu danh sách này dài thêm,
> đó là dấu hiệu descriptor còn thiếu trường.

Danh sách hiện tại: **bias int32** và **hình dạng tensor trọng số**. Cả hai đều cần cho phần
cứng. Bias phải nằm trong vùng trọng số hoặc vùng thang; hình dạng suy được từ
`cin/cout/k/dw` nên chỉ là tiện lợi, không phải thiếu.

---

## 5. Điều nghiêm trọng: bias có thể phá N7

Đếm số kênh cần một giá trị bias — mọi lớp tích chập sau khi gấp BatchNorm, cộng các lớp
fully-connected — được **534 kênh** trên bốn mô hình. Thử ba độ rộng:

| Độ rộng bias | Byte bias | Tổng N7 | Dư | |
|---|---|---|---|---|
| 32 bit | 2.136 | 25.918 B = 25,31 kB | −1.342 B | **VƯỢT** |
| 16 bit | 1.068 | 24.850 B = 24,27 kB | −274 B | **VƯỢT** |
| 8 bit | 534 | 24.316 B = 23,75 kB | +260 B | vừa, nhưng mất độ chính xác |

Cộng thêm bảng nền bộ đệm 78 B.

**Nghĩa là ngân sách 24 kB không đủ cho bias ở độ rộng an toàn.** Đây là phát hiện quan trọng
hơn cả ba khoảng trống trên, và nó không xuất hiện ở P4 vì P4 để bias trong `meta` — ngoài blob,
nên ngoài ngân sách.

`bias_int` được tính là `round(bias / (s_in · s_w))`, tức **nằm trong miền psum**, nên biên độ
của nó cùng cỡ với psum (dưới 2²⁴) và cần khoảng 25 bit nếu lưu thô.

### Hướng thoát, theo thứ tự nên thử

1. **Lưu bias trong miền đầu ra, không miền psum.** `bias_out = bias / s_out` nằm cùng thang với
   kích hoạt int8, nên 8–12 bit là đủ. Requant khi đó cộng bias **sau** khi nhân và dịch, không
   phải trước. Đổi thứ tự này làm `ecg_requant` phải sửa và **phải kiểm lại bit-exact**.
2. **Gộp bias vào bản ghi thang requant.** Vùng thang đang là 256 × 2 B; thêm một trường bias
   vào mỗi bản ghi tốn 256 × 2 B = 512 B nữa, và chỉ phục vụ được 256 trong 534 kênh — không đủ.
3. **Nới N7 lên 32 kB.** Là phương án cuối trong thứ tự hy sinh của `pha-va-moc.md` §4, và nó
   phá cơ sở so sánh với NCKU (cũng 24 kB).

### Đã đo — và phép đo quyết dứt điểm

`make isa` với mục dump mới (`compile_model.py --dump-bias`,
`90-results/tables/bias-width.csv`), **562 kênh trên 34 lớp**:

| Miền lưu bias | |max| | Bit cần | Byte | |
|---|---|---|---|---|
| psum, `bias / (s_in · s_w)` | **18.728** | 16 | 1.124 | N7 **vượt** 330 B |
| **đầu ra, `bias / s_out`** | **80** | **8** | **562** | N7 **đạt**, dư 232 B |

**Chênh 234 lần về biên độ.** Lý do rõ ràng khi nhìn lại: miền psum là miền của bộ tích luỹ nên
bias ở đó cùng cỡ với tổng tích chập; miền đầu ra cùng thang với kích hoạt int8 nên bias ở đó
không thể lớn hơn vài lần 127.

### Ngân sách N7 sau khi bổ sung cả ba thứ còn thiếu

| Vùng | Byte |
|---|---|
| Trọng số bốn mô hình | 18.488 |
| Bộ đệm kích hoạt | 3.840 |
| Bảng descriptor | 864 |
| Thang requant | 512 |
| **Bảng nền bộ đệm** (mới) | **78** |
| **Bias miền đầu ra** (mới) | **562** |
| **Tổng** | **24.344 = 23,77 kB** |

> **Lỗi thời (ADR-0015).** Ước lượng viết tay, đã trượt N7. Số đúng: `make n7` → 11.634 B = 11,36 kB.
| Ngưỡng N7 | 24.576 = 24,00 kB |
| **Dư** | **232 B (0,9 %)** |

**N7 đạt, nhưng dư chỉ 0,9 %.** Từ đây trở đi mọi vùng nhớ thêm vào đều phải tính lại bảng này,
và 232 B không cho phép thêm gì đáng kể.

### Cái giá của việc đổi miền, phải trả và phải kiểm

Đổi miền không phải phép biến đổi tương đương. Requant hiện tính

    y = clamp( round( (psum + bias) · mult / 2^shift ) )

đổi sang miền đầu ra thì thành

    y = clamp( round( psum · mult / 2^shift ) + bias_out )

Phép làm tròn xảy ra ở **chỗ khác**, và `bias_out = round(bias / s_out)` mất độ chính xác nhiều
hơn `round(bias / (s_in · s_w))` vì bước lượng tử lớn hơn. Nên:

- `ecg_requant.sv` phải sửa thứ tự, và **22.718 vector phải kiểm lại** — rẻ, đã có hạ tầng.
- Đường số nguyên trong `compile_model.py` phải đổi theo, và **vòng tròn bit-exact phải chạy lại**.
- Nếu vòng tròn trượt, phương án còn lại là giữ miền psum và cắt 330 B ở chỗ khác — nhưng chỗ
  duy nhất còn co được là bảng thang requant.

## 6. Đề nghị

Viết **ADR-0014** quyết bốn điều, rồi chạy lại `make isa` để blob tự đủ:

1. **`CONCAT` bỏ bản ghi riêng.** Mỗi nhánh mang `dst` = bộ đệm dùng chung và `dst_off` = offset
   kênh của nhánh đó. Giữ đúng lập luận "không tốn logic" mà N9 đang dựa vào, và giải quyết luôn
   giới hạn hai trường nguồn.
2. **Thêm `buf_base[13]` vào vùng Model Descriptor**, 12 bit mỗi giá trị, 78 B cho bốn mô hình.
3. **Bias chuyển sang miền đầu ra**, **8 bit**, vào blob — đã đo, 562 B, N7 đạt với dư 232 B.
4. **Cập nhật bảng ngân sách N7** trong `KE-HOACH-P5.md` §0.1 sau khi có ba điều trên.

## 7. Việc không bị chặn, làm song song

`ecg_actbuf` là một vùng nhớ phẳng 1R1W; nó không cần biết địa chỉ nền — sequencer mới cần.
`ecg_wmem` tương tự. Hai module đó viết được ngay, và đó là việc tiếp theo.

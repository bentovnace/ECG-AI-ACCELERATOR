# Kế hoạch P4 · Đặc tả tập lệnh và descriptor

Pha **T4–T7**, chồng lấn P3 (T3–T6) và P5 (T6–T11) một cách cố ý. Pha này **không chốt ngưỡng
nào**, nhưng nó là chỗ duy nhất kiểm **tính khả thi của N3** trước khi có một dòng RTL —
bằng ngân sách chu kỳ, không bằng mô phỏng.

Ràng buộc kế thừa: ADR-0001 §2.3 (bốn họ, 12 opcode, mỗi opcode phải có chủ) · ADR-0002 §4.1
(≤ 1,0 kB bảng mô tả, ≤ 4,0 kB bộ đệm kích hoạt) và §4.2 (ngân sách 1.000 chu kỳ) ·
ADR-0003 (CV32E40X, giao diện CV-X-IF) · ADR-0010 §7 (phân vị hiệu chỉnh khác nhau theo họ) ·
`30-model/golden/manifest.json` (vector vàng đã đóng băng).

**Đầu ra của pha này là thứ mốc T6 đóng băng:** tập opcode, định dạng descriptor. Sau T6 muốn
sửa thì phải viết ADR thay thế và trả giá lịch.

---

## Nguyên tắc thứ tự — đọc trước khi làm bất cứ bước nào

**Đặc tả phải suy ra từ đồ thị mô hình đã đóng băng, không suy ra từ trí nhớ.** Bốn model
trong `30-model/champion/` là nguồn chân lý về việc phần cứng phải làm gì. Mọi trường trong
descriptor phải chỉ ra được ít nhất một lớp cụ thể của một model cụ thể cần tới nó — cùng
đúng một logic với ADR-0001 §2.3 áp cho opcode.

**Không đặc tả cái chưa có ai dùng.** ADR-0001 §2.3 nói opcode không mô hình nào dùng thì loại
khỏi đặc tả. Áp nguyên tắc đó cho cả **trường descriptor**, không riêng opcode.

**P4 không được đụng vào vector vàng.** Vector vàng đã đóng băng cùng thang requant per-channel
theo họ (ADR-0010 §7). Nếu đặc tả descriptor không chở nổi thang đó thì **sửa descriptor**,
không sửa vector — đổi vector là vô hiệu hoá toàn bộ N2 ở P6.

---

## P4.0 · Điểm danh opcode lại, suy từ đồ thị

**Ra:** `tools/opcode_audit.py` · `90-results/tables/opcode-usage.csv` · cập nhật `export.py`

`export.py` hiện điểm danh bằng thuộc tính khai báo tay `Model.opcodes`, và danh sách đó
**đã lệch với đồ thị thật**. Hai chỗ lệch đã thấy trước khi bắt tay:

- `CONCAT` được khai cho riêng Inception, nhưng cả bốn họ đều dùng — phép nối nhánh RR vào
  vector hình thái (`torch.cat` trong `forward` của cả bốn lớp model).
- ResNet dùng tích chập 1×1 ở nhánh skip, tức `PWCONV`, nhưng không khai.

Việc của bước này là thay danh sách khai báo tay bằng **duyệt đồ thị**: mỗi `nn.Module` ánh xạ
về một opcode theo quy tắc viết ra được, rồi đối chiếu với 12 opcode dự kiến.

**Chốt ba opcode "ngầm định".** `pha-va-moc.md` §T6 ghi *"9 tường minh + 3 ngầm định (LOADW,
RELU, STORE)"*. Không thể lập ngân sách chu kỳ cho một lệnh không biết ai gọi, nên P4.0 phải
kết luận dứt khoát cho từng cái: hoặc nó là opcode thật có chỗ trong ISA, hoặc nó là **trường
trong descriptor** (trường hợp RELU rất có thể như vậy), hoặc nó bị loại và mọi chỗ viết
"12 opcode" phải sửa.

**Điều kiện xong:** bảng opcode ↔ (họ, tên lớp cụ thể) sinh bằng lệnh, không gõ tay. Opcode nào
không có hàng thì có quyết định thành văn về việc giữ hay bỏ.

## P4.1 · Đặc tả ISA — mã hoá 12 opcode trên CV-X-IF

**Ra:** `40-rtl/docs/isa.md` · `40-rtl/include/ecg_pkg.sv`

- Mã hoá 32 bit theo khuôn lệnh tuỳ biến RISC-V, dùng không gian `custom-0/1` (ADR-0003).
- Với mỗi opcode: ngữ nghĩa, toán hạng, tác dụng phụ lên bộ đệm, số chu kỳ phát hành, và
  **lớp cụ thể của model nào ép nó tồn tại**.
- Phân biệt rạch ròi lệnh **đồng bộ** (core chờ kết quả qua CV-X-IF) và lệnh **khởi động rồi
  rời** (coproc chạy nền, core hỏi trạng thái) — đây là quyết định ảnh hưởng thẳng tới N3.
- `ecg_pkg.sv` là nguồn chân lý duy nhất của mã opcode. RTL ở P5 và testbench ở P6 cùng nạp
  gói này; không nơi nào được gõ lại hằng số.

**Điều kiện xong:** mọi lớp của cả bốn model biểu diễn được bằng đúng tập lệnh này, chứng minh
bằng bảng liệt kê từng lớp → chuỗi lệnh.

## P4.2 · Model Descriptor và Layer Descriptor

**Ra:** `40-rtl/docs/descriptor.md` · trường bit trong `ecg_pkg.sv`

Ngân sách cứng: **≤ 1,0 kB cho toàn bộ bảng mô tả của cả bốn model** (ADR-0002 §4.1). Với
16 B mỗi bản ghi, đó là **64 bản ghi tổng** cho 4 Model Descriptor cộng toàn bộ Layer
Descriptor — tức trung bình ≤ 15 lớp mỗi model. Bước này phải đếm số lớp thật của bốn model
và xác nhận vừa; không vừa thì hoặc nén trường, hoặc dùng bản ghi 8 B cho lớp đơn giản.

Bốn trường đã bị quyết định trước ép phải có mặt:

| Trường | Ai ép | Nguồn |
|---|---|---|
| `skip_src` — nguồn của nhánh cộng | ResNet | ADR-0001 §2.3 |
| Mô tả nhánh và thứ tự nối | Inception (3 nhánh × 2 khối) | ADR-0001 §2.3 |
| Thang requant **per-channel**, phân vị theo họ | cả bốn, khác nhau từng họ | ADR-0010 §7 |
| Đường nạp đặc trưng RR | cả bốn (n_extra = 3) | KE-HOACH-P3 §P3.5 |

Trường thứ ba là ràng buộc nặng nhất và mới xuất hiện sau ADR-0002: thang per-channel nghĩa là
bảng thang **tỉ lệ với số kênh**, không phải một hằng số mỗi lớp. Phải quyết ngay thang nằm
trong descriptor hay nằm trong vùng trọng số, và tính lại ngân sách 1,0 kB theo lựa chọn đó.

**Điều kiện xong:** bốn model hiện tại tuần tự hoá được thành bảng mô tả thật, tổng ≤ 1,0 kB,
và bảng đó nạp lại dựng đúng đồ thị.

## P4.3 · Trình biên dịch: model đã đóng băng → blob nhị phân

**Ra:** `tools/compile_model.py` · `30-model/export/*.bin` · kiểm tra vòng tròn

ADR-0001 §T1 đã cam kết "trình biên dịch ONNX → blob nhị phân" với hội đồng. Bước này hiện
thực nó ở mức tối thiểu cần cho P5: đọc checkpoint đã niêm phong, sinh descriptor cộng vùng
trọng số theo đúng bố cục §P4.2.

**Kiểm tra vòng tròn bắt buộc:** một trình thông dịch Python đọc blob và chạy suy luận phải
cho ra **đúng từng bit** vector vàng trong `30-model/golden/`. Nếu không khớp thì đặc tả sai,
và biết điều đó ở P4 rẻ hơn biết ở P6 vài tháng.

## P4.4 · Mô hình theo chu kỳ và kiểm khả thi N3

**Ra:** `tools/cycle_model.py` · `90-results/tables/cycle-budget.csv` ·
`40-rtl/docs/ngan-sach-chu-ky.md`

Thay ước lượng ≈ 594 chu kỳ của ADR-0002 §4.2 bằng con số tính từ đồ thị bốn model thật, và
tách hai đại lượng khác nhau mà bảng cũ để lẫn:

- **T_switch** — chuyển mô hình, ngưỡng N3 ≤ 1.000 chu kỳ.
- **T_infer** — chạy trọn một nhịp, ràng buộc bởi N8 (không bỏ lỡ nhịp) chứ không bởi N3.

T_infer là đại lượng chưa ai trong đề tài tính, mà nó mới quyết định số PE. MAC thật đã đếm
được: 92.428 · 190.416 · 159.720 · 180.216 cho m1…m4. Ở một MAC mỗi PE mỗi chu kỳ, 8 PE và
hiệu suất 70 %, ResNet cần ≈ 34.000 chu kỳ — vẫn dưới 139.000 chu kỳ của một chu kỳ lấy mẫu ở
50 MHz, nhưng biên không rộng như trực giác. Phải tính cho đủ, kèm phân tích roofline chốt số
PE mà ADR-0001 §T1 đã cam kết.

**Hạng mục phải soi kỹ nhất:** ADR-0002 §4.2 dành 512 / 594 chu kỳ (86 %) cho việc *xoá bộ đệm
kích hoạt 4 kB*. Cần trả lời: có thật sự phải xoá toàn bộ 4 kB không, hay chỉ cần xoá vùng
đệm biên mà lớp sau đọc trước khi có ai ghi vào? Nếu chỉ cần xoá vùng biên thì T_switch giảm
mạnh và ràng buộc "đường xoá 64 bit phải có từ RTL đầu tiên" cần phát biểu lại. Đây là kết
luận có hệ quả kiến trúc, nên phải thành ADR chứ không phải một ghi chú.

**Điều kiện xong:** T_switch tính được cho cả 12 cặp chuyển (4 model, mỗi cặp có thứ tự), số
xấu nhất so được với 1.000; T_infer tính được cho cả bốn; số PE có luận cứ roofline.

## P4.5 · Rào chắn N9 — tỉ lệ tái sử dụng logic

**Ra:** mục trong `40-rtl/docs/isa.md`

N9 đòi **≥ 90 % logic dùng chung giữa bốn họ** và được chốt ở P5. Nhưng P5 chỉ *đo* được cái
mà P4 đã *cho phép*: nếu đặc tả để cho Inception cần một đường dữ liệu riêng, thì không tối ưu
RTL nào cứu được N9. Bước này rà lại toàn đặc tả và đánh dấu mọi chỗ một họ đòi phần cứng mà
ba họ kia không dùng, rồi hoặc gộp lại, hoặc ghi nhận trước phần trăm sẽ mất.

---

## Thứ tự phụ thuộc

```
P4.0 diem danh opcode  ──►  P4.1 ISA  ──►  P4.2 descriptor  ──►  P4.3 compiler
                                                │                      │
                                                └──►  P4.4 chu ky  ◄────┘
                                                          │
                                                          ▼
                                                     P4.5 rao chan N9
```

P4.0 làm được ngay. P4.4 cần P4.2 để biết chi phí nạp descriptor, và cần P4.3 để có blob thật
mà đếm.

---

## Rủi ro đã biết trước

| Rủi ro | Nguồn | Cách xử |
|---|---|---|
| Thang requant per-channel làm vỡ ngân sách 1,0 kB bảng mô tả | ADR-0010 §7 xuất hiện sau ADR-0002 §4.1 | P4.2 đếm thật; nếu vỡ thì chuyển thang sang vùng trọng số và tính lại 18,4 kB |
| Ba opcode ngầm định không có chủ thật | `pha-va-moc.md` §T6 | P4.0 quyết dứt: opcode, hay trường descriptor, hay loại |
| Inception ba nhánh đòi đường dữ liệu riêng ⇒ N9 < 90 % | ADR-0002 §3 | P4.5 rà trước khi viết RTL |
| T_infer vượt chu kỳ lấy mẫu ở số PE đã chọn | chưa ai tính | P4.4 tính roofline; thiếu thì tăng PE hoặc hạ tần số mục tiêu |

## Điều kiện coi là xong P4

- [ ] Bảng opcode sinh bằng lệnh, mỗi opcode có ít nhất một lớp cụ thể dùng tới
- [ ] `ecg_pkg.sv` là nguồn chân lý duy nhất của mã opcode và trường descriptor
- [ ] Bốn model tuần tự hoá thành bảng mô tả thật, tổng ≤ 1,0 kB
- [ ] Blob nhị phân chạy lại cho **đúng từng bit** vector vàng ở `30-model/golden/`
- [ ] T_switch xấu nhất và T_infer cả bốn model có trong `90-results/tables/cycle-budget.csv`
- [ ] Mọi con số sinh lại được bằng một mục `make`

Đủ sáu cái này thì P5 có đủ đầu vào để viết dòng SystemVerilog đầu tiên mà không phải đoán.

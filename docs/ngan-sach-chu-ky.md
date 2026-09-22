# Ngân sách chu kỳ — kiểm khả thi N3 và chốt số PE

**Pha:** P4.4 · **Sinh lại:** `python3 tools/cycle_model.py --pe 8`
**Bảng số:** `90-results/tables/cycle-budget.csv`

---

## 1. Hai đại lượng mà ADR-0002 §4.2 để lẫn

| | Là gì | Ràng buộc | Nguồn ràng buộc |
|---|---|---|---|
| **T_switch** | chuyển mô hình | ≤ 1.000 chu kỳ | N3, ngưỡng cạnh tranh |
| **T_infer** | chạy trọn một nhịp | < 139.000 chu kỳ (2,78 ms @360 Hz, @50 MHz) | N8, ràng buộc chức năng |

T_infer **chưa từng được tính trong đề tài**, mà nó mới là đại lượng quyết định số PE. Bảng bốn
dòng của ADR-0002 §4.2 chỉ nói về T_switch.

---

## 2. T_infer — tính từ đồ thị bốn mô hình

Ánh xạ giả định: song song theo kênh ra, một MAC mỗi PE mỗi chu kỳ. Một lớp `cout` kênh ra chạy
`ceil(cout / PE)` lần quét, nên **lớp hẹp lãng phí mảng** — đầu phân lớp có 5 kênh ra dùng đúng
5 trên 8 PE.

Ở **8 PE**:

| Mô hình | Chu kỳ | @50 MHz | Lớp đắt nhất |
|---|---|---|---|
| m1-cnn | 12.384 | 248 µs | `features.2.0` — 62 % |
| m2-resnet | 28.808 | 576 µs | hai conv của `blocks.0` — 53 % |
| m3-mobilenet | 20.332 | 407 µs | `blocks.1.pw.0` — 57 % |
| m4-inception | **30.258** | **605 µs** | `inc1.b2.0` (k = 7) — 36 % |

**Xấu nhất 30.258 chu kỳ, dùng 22 % một chu kỳ lấy mẫu.** N8 khả thi với biên rất rộng ở 8 PE.

Điểm đáng chú ý: **Inception đắt nhất về thời gian chạy, không phải ResNet** — dù ResNet có
nhiều MAC hơn tính trên giấy (190.416 so với 180.216). Nguyên nhân là nhánh k = 7 của Inception
chạy trên tensor còn dài (64 mẫu), trong khi MAC của ResNet phân bố về các lớp đã hẹp.

## 3. Roofline — chốt số PE

| PE | m1 | m2 | m3 | m4 | Xấu nhất | Hiệu suất mảng |
|---|---|---|---|---|---|---|
| 2 | 48.138 | 96.186 | 81.102 | 93.172 | 96.186 | 99,9 % |
| 4 | 24.114 | 48.142 | 40.618 | 50.912 | 50.912 | 94,4 % |
| **8** | 12.384 | 28.808 | 20.332 | 30.258 | **30.258** | **79,4 %** |
| 16 | 7.533 | 14.453 | 11.481 | 20.479 | 20.479 | 58,7 % |
| 32 | 4.333 | 14.453 | 7.673 | 20.351 | 20.351 | 29,5 % |
| 64 | 4.333 | 14.453 | 5.177 | 20.351 | 20.351 | 14,8 % |

**Điểm gãy nằm giữa 8 và 16 PE.** Từ 16 trở lên, hiệu suất rơi dưới 60 % và tới 32 PE thì thời
gian gần như không giảm nữa — vì các mô hình ràng buộc chỉ có 8…72 kênh ra, không đủ chiều rộng
để nuôi mảng. Đây là hệ quả trực tiếp của ngân sách 4.700 tham số: **mô hình nhỏ đặt trần cho
số PE có ích**, và đó là một kết quả đáng viết vào luận văn chứ không phải một hạn chế phải giấu.

**Chọn 8 PE.** Ở đó: dùng 22 % chu kỳ lấy mẫu, hiệu suất mảng 79 %, và còn dư địa 4,6 lần cho
việc hạ tần số để tiết kiệm công suất ở P7 — mà công suất mới là thứ RQ0 quan tâm.

Ngay cả **2 PE cũng đủ** cho ràng buộc chức năng (96.186 < 139.000). Nói cách khác N8 không phải
ràng buộc chặt; nó chỉ trở nên chặt nếu tần số ASIC hạ xuống dưới ≈ 11 MHz. Cần nhớ điều này khi
P7 chọn điểm vận hành.

## 4. T_switch — kiểm khả thi N3

T_switch chỉ phụ thuộc **mô hình đích**: với mô hình nguồn không có việc gì phải làm ngoài đợi
FIFO sequencer rỗng, mà việc đó đã nằm trong T_infer của nhịp cuối. Vì vậy 12 cặp chuyển có thứ
tự rút về **4 con số**, không phải 12.

Đường bus đọc bảng mô tả và đường xoá bộ đệm đều 64 bit/chu kỳ.

| Mô hình đích | Model Desc | Layer Desc | Thang requant | Xoá bộ đệm | FSM | **Tổng** |
|---|---|---|---|---|---|---|
| m1-cnn | 6 | 20 | 27 | 512 | 30 | **595** |
| m2-resnet | 6 | 28 | 34 | 512 | 30 | **610** |
| m3-mobilenet | 6 | 20 | 2 | 512 | 30 | **570** |
| m4-inception | 6 | 32 | 3 | 512 | 30 | **583** |

**Xấu nhất 610 / 1.000 — đạt, biên 39 %.** Ước lượng ≈ 594 của ADR-0002 §4.2 hoá ra rất sát
tổng, nhưng sát vì tình cờ: bảng cũ cho Layer Descriptor 48 chu kỳ (thực tế 20–32) và cho thang
requant 0 chu kỳ (thực tế tới 34, vì per-channel ra đời sau ở ADR-0010).

Chi phí per-channel là **27 chu kỳ cho m1 và 34 cho m2**, so với 2–3 cho hai họ per-tensor.
Đây là cái giá bằng chu kỳ của quyết định ADR-0010 §7, và nó nhỏ — 5 % ngân sách N3.

## 5. Hạng mục chi phối, và câu hỏi nên hỏi về nó

**Xoá bộ đệm chiếm 512 / 610 chu kỳ, tức 84 %.** ADR-0002 §4.2 kết luận từ đó rằng đường xoá
64 bit phải có mặt từ RTL đầu tiên. Câu hỏi chưa ai hỏi là: **có thật cần xoá không?**

Xoá bộ đệm chỉ cần khi có ô nhớ được **đọc trước khi có ai ghi vào**. Trong bốn mô hình, điều đó
xảy ra đúng một chỗ: vùng đệm biên (zero padding) của các tích chập có `pad > 0`. Mọi ô còn lại
đều bị lớp ghi đè trước khi lớp sau đọc — kể cả bộ đệm dùng chung của CONCAT, vì ba nhánh phủ
kín ba dải kênh của nó.

Mà zero padding **không cần nằm trong bộ nhớ**. Bộ sinh địa chỉ biết chỉ số nào nằm ngoài biên,
và bơm 0 thẳng vào đường nạp PE. Chi phí: một bộ so sánh và một mux ở đường vào PE — vài chục
cổng, không phải 4 kB đường xoá.

| Phương án | T_switch xấu nhất | Biên so với 1.000 |
|---|---|---|
| Xoá trọn 4 kB | 610 | 39 % |
| **Bơm 0 ở bộ sinh địa chỉ** | **98** | **90 %** |

**Giảm 6,2 lần.** Với 98 chu kỳ, T_switch còn 2 µs @50 MHz, và nó thôi là một ràng buộc thiết
kế — nó trở thành một con số để đem đi so.

Hệ quả phải phát biểu thành ADR, vì nó **đảo ngược một kết luận của ADR-0002 §4.2**: đường xoá
bộ đệm 64 bit không còn là hạng mục bắt buộc từ RTL đầu tiên; thứ bắt buộc là **bơm 0 ở bộ sinh
địa chỉ**, và thứ đó thì rẻ hơn nhiều.

Điều kiện để kết luận này đúng, phải kiểm ở P6 chứ không được tin: **mọi ô bộ đệm mà một lớp
đọc thì hoặc do lớp trước ghi, hoặc nằm ngoài biên**. Nếu sai ở một lớp thôi thì kết quả nhiễm
dữ liệu của mô hình trước — đúng loại lỗi mà kiểm bit-exact toàn DS2 sẽ bắt được, và cũng đúng
loại lỗi mà kiểm sơ sài sẽ bỏ qua.

## 6. Giới hạn của mô hình này

Mô hình đếm chu kỳ tính toán. Nó **chưa** kể ba thứ, và cả ba đều làm số thật lớn hơn:

- **Băng thông SRAM trọng số.** Giả định mọi PE lấy được toán hạng mỗi chu kỳ. Với 8 PE và
  int8, cần 8 B/chu kỳ — đúng bằng bus 64 bit, tức không còn biên. Đây là hạng mục P5 phải đo lại.
- **Nạp trọng số (`LOADW`) che lấp hoàn toàn sau tính toán.** Đúng khi lớp trước đủ dài; lớp
  cuối (`head`, 5 kênh ra) thì không. Sai số vài trăm chu kỳ trên T_infer — không đổi kết luận.
- **Đợi đồng bộ CDC** giữa miền cảm biến và miền lõi. Thuộc N10, đo ở P7.

Không thứ nào trong ba thứ này chạm tới T_switch, nên kết luận §5 vững. Chúng chỉ ảnh hưởng
T_infer, mà T_infer đang dư 4,6 lần biên.

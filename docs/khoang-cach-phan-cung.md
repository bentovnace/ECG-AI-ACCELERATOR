# Khoảng cách giữa đường báo cáo và đường phần cứng

Phát hiện và đo 01-09-2026 ở P5.2. Đây là sửa chữa quan trọng nhất của pha này, và nó lật lại
một kết luận tôi đã báo cáo.

---

## 1. Hai đường, và chỉ một trong hai chạy được trên chip

| | Đường **báo cáo** | Đường **phần cứng** |
|---|---|---|
| Mã | `quantize.py` | `int_ref.IntRunner` |
| Trọng số | int8 | int8 |
| Kích hoạt | fake-quant qua `ActQuant`, giá trị vẫn là số thực | mảng int8 thật |
| Bias | **float32 Parameter** | **8 bit, miền đầu ra** (ADR-0014 §2.3) |
| Bộ nhân requant | số thực | **nguyên 11 bit + dịch 5 bit** |
| Là nguồn của | **mọi con số DS2 trong luận văn** | thứ chip sẽ tính |

Bias float32 không triển khai được ở 4,5 kB. Nên nếu hai đường lệch nhau nhiều thì con số báo
cáo là con số của một mô hình không tồn tại.

## 2. Đo lần đầu: lệch tới 10,47 điểm

Validation, ba hạt giống, với chính sách lượng tử hoá chọn trên đường báo cáo:

| Họ | báo cáo | phần cứng | chênh |
|---|---|---|---|
| CNN | 0,6174 | 0,5909 | +2,65 |
| ResNet | 0,6350 | 0,6284 | +0,67 |
| MobileNet | 0,6246 | 0,5938 | +3,08 |
| **Inception** | 0,6262 | **0,5214** | **+10,47** |

Inception mất **10,47 điểm** — và đó chính là mô hình tôi vừa tuyên bố tốt nhất dựa trên DS2.

## 3. Truy nguyên nhân: không phải bias, không phải bộ nhân

Nghi bộ nhân 11 bit trước. **Đo và loại:** sai số lượng tử hoá bộ nhân chỉ **0,013–0,019 %**
trung bình, tệ nhất 0,05 %, đều nhau ở cả bốn họ. Không thể gây mất 10 điểm.

Nguyên nhân thật là **phân vị hiệu chỉnh 100,0 của Inception**. Trên đường báo cáo, giá trị giữa
các phép toán vẫn là số thực nên `CONCAT` bốn nhánh biên độ khác nhau không mất gì. Trên đường
phần cứng, mỗi nhánh là mảng int8 với thang riêng và phải **đổi về thang dùng chung**; với phân
vị 100 thì thang đó do một ngoại lai quyết định, và ba nhánh yếu bị nghiền vào một phần nhỏ của
dải int8.

Nói cách khác: phát hiện "Inception cần phân vị 100" mà tôi rút ra từ validation là đúng **cho
đường báo cáo** và **sai cho phần cứng**.

## 4. Quét lại phân vị trên đường phần cứng

Validation, ba hạt giống mỗi ô:

| Họ | phân vị cũ | macro-F1 phần cứng | phân vị mới | macro-F1 phần cứng | Δ |
|---|---|---|---|---|---|
| CNN | 99,95 | 0,5909 ± 0,0051 | **99,5** | **0,6133 ± 0,0056** | **+2,24** |
| ResNet | 99,9 | 0,6284 ± 0,0167 | 99,9 | không đổi | — |
| MobileNet | 99,9 | 0,5938 ± 0,0193 | **99,0** | **0,6111 ± 0,0248** | **+1,73** |
| **Inception** | 100,0 | 0,5214 ± 0,0501 | **99,99** | **0,6067 ± 0,0095** | **+8,53** |

Phân vị 100 của Inception không chỉ kém trung bình mà còn có **độ lệch chuẩn 0,0501** — gấp năm
lần lựa chọn mới. Đó là dấu hiệu tự nó: khi không cắt đỉnh, một ngoại lai duy nhất quyết định
thang cho cả bốn nhánh, nên kết quả phụ thuộc hạt giống rất mạnh.

MobileNet ở phân vị 100 cho **0,2786** — gần như sụp hoàn toàn.

## 5. Sau khi sửa: chênh lệch xấu nhất 1,76 điểm

| Họ | báo cáo | phần cứng | chênh |
|---|---|---|---|
| CNN | 0,6041 | **0,6133** | **−0,92** |
| ResNet | 0,6350 | 0,6284 | +0,67 |
| MobileNet | 0,6053 | **0,6111** | **−0,58** |
| Inception | 0,6244 | 0,6067 | +1,76 |

Từ **10,47 xuống 1,76**. Hai họ có chênh lệch **âm** — đường phần cứng *tốt hơn* đường báo cáo,
vì phân vị mới phù hợp hơn cho cả hai.

Cả bốn mô hình vẫn **bit-exact** với vector vàng, và ba testbench RTL vẫn đạt.

## 6. Hệ quả phải xử

**Bảng DS2 hiện tại đo trên đường báo cáo với chính sách cũ, nên nó không còn là số của phần
cứng.** Ba việc:

1. **Số DS2 phải đo lại trên đường phần cứng** — đó sẽ là **lần nhìn thứ ba**. Cần quyết định
   riêng theo ADR-0008.
2. **Kết luận "Inception tốt nhất" phải rút lại** cho tới khi có số mới. Trên đường phần cứng ở
   validation, ResNet dẫn (0,6284 so với 0,6067), và ResNet cũng là mô hình mang luận điểm chính
   về chính quy hoá.
3. **Bài báo phải nói rõ hai đường tồn tại** và con số nào thuộc đường nào.

## 7. Bài học phương pháp

Đây là lần thứ hai trong đề tài một lựa chọn siêu tham số hoá ra chỉ đúng trong bối cảnh nó được
đo. Lần đầu là "nhiều tham số làm mô hình tệ hơn" — đúng ở 40 epoch không chính quy hoá, sai khi
có tăng cường. Lần này là "Inception cần phân vị 100" — đúng cho fake-quant, sai cho int8 thật.

Quy tắc rút ra: **hiệu chỉnh lượng tử hoá phải được chọn trên đường mà phần cứng thực thi.** Một
đường fake-quant tiện lợi hơn để chạy nhưng nó không phải thứ được triển khai, và ở đây nó sai
tới 10 điểm.

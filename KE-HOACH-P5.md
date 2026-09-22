# Kế hoạch P5 — RTL và tổng hợp thử

**Đích:** Arty Z7-20 (XC7Z020-1CLG400C) + Vivado.
**Vào:** toàn bộ đầu ra P4 — `include/ecg_pkg.sv`, `40-rtl/docs/isa.md`, `40-rtl/docs/descriptor.md`,
`40-rtl/docs/ngan-sach-chu-ky.md`, blob nhị phân đã khớp bit-exact vector vàng.
**Ngưỡng chốt ở pha này:** **N6 ≥ 4** · **N7 ≤ 24 kB** · **N9 ≥ 90 %**.

---

## 0. Ba con số phải nhớ trước khi viết dòng đầu

### 0.1 Ngân sách bộ nhớ chỉ dư 0,9 %

| Vùng | Byte | kB |
|---|---|---|
| Trọng số bốn mô hình (4.609 + 4.601 + 4.693 + 4.585) | 18.488 | 18,05 |
| Bộ đệm kích hoạt (đỉnh thật, `tools/compile_model.py`) | 3.840 | 3,75 |
| Bảng descriptor (54 bản ghi × 16 B) | 864 | 0,84 |
| Thang requant (256 × 2 B) | 512 | 0,50 |
| Bảng nền bộ đệm (P5.2) | 78 | 0,08 |
| Bias miền đầu ra, 8 bit (P5.2) | 562 | 0,55 |
| **Tổng** | **24.344** | **23,77** |

> **Lỗi thời (ADR-0015).** Bảng này là ước lượng viết tay và đã trượt N7: đỉnh mức thật của m4 là 4.416 B chứ không 3.840 B. Số đúng đo bằng `make n7`: 11.634 B = 11,36 kB với chính sách thường trú một bộ trọng số.
| Ngưỡng N7 | 24.576 | 24,00 |
| **Dư** | **232** | **0,23 (0,9 %)** |

Hai dòng cuối là hai vùng mà P4 để ngoài blob và do đó ngoài ngân sách; P5.2 phát hiện và đo
(xem `40-rtl/docs/khoang-trong-dac-ta-P5.md`).

**Hệ quả cho RTL:** với 232 B dư, bất kỳ bộ đệm phụ hay FIFO sâu nào cũng phá N7. Mỗi
lần thêm một vùng nhớ phải tính lại bảng này. Trên FPGA thì thoải mái — 23,77 kB là 3,8 % BRAM
của XC7Z020 — nhưng N7 là ràng buộc ASIC, và FPGA chỉ là bàn thử.

### 0.2 Tám PE, không nhiều hơn

Roofline P4.4 đã chốt: từ 16 PE trở lên hiệu suất mảng rơi dưới 60 % vì mô hình ràng buộc chỉ
có 8…72 kênh ra. Tám PE cho T_infer xấu nhất 30.258 chu kỳ trên ngân sách 139.000, tức dư 4,6
lần để hạ tần số ở P7.

### 0.3 N9 đạt bằng 0,1 điểm

Rà P4.5 cho 90,1 % so với ngưỡng 90,0 %, và đó là **ước lượng gate bằng tay**. Chỗ mỏng đã biết:
**m2 đòi 65 gate chỉ riêng nó dùng** — cổng đọc thứ hai cho đường `ADD`/skip. Nếu tổng hợp thật
đẩy N9 xuống dưới 90 %, đó là chỗ xem đầu tiên.

Vì vậy P5 phải viết đường dữ liệu sao cho cổng đọc thứ hai **dùng chung** với đường đọc `src0`
qua một mux thời gian, chứ không phải một cổng RAM thứ hai thật. Quyết định này phải nằm trong
mã ngay từ đầu; sửa sau khi có 3.000 dòng SystemVerilog thì đắt.

---

## 1. Nguyên tắc thứ tự

Viết từ lá lên gốc, và **mỗi module có testbench trước khi module sau dùng nó**. Lý do không
phải kỷ luật hình thức: N2 đòi bit-exact với vector vàng, và một sai một bit ở tầng requant sẽ
biểu hiện thành "mô hình sai" ở tầng trên, nơi khó truy nhất.

Công cụ tại chỗ: Verilator 4.038 (lint + mô phỏng), iverilog, yosys. **Vivado không có trên máy
này** — tổng hợp và số tài nguyên thật phải chạy ở máy người dùng, và đó là điều kiện của P5.4.

---

## 2. P5.0 · Khung dự án và bộ khung ký hiệu

- `src/common/` — `ecg_sram.sv` (bọc BRAM, suy ra được trên cả Vivado và Verilator),
  `ecg_fifo.sv`.
- `filelists/` — một danh sách tệp duy nhất, dùng chung bởi lint, mô phỏng và Vivado. Không
  duplicate danh sách; hai bản sao là hai cơ hội lệch nhau.
- `make lint` mở rộng cho mọi module mới, chạy sạch **không cảnh báo**, không chỉ không lỗi.

**Ra:** `make lint` sạch trên khung rỗng, `filelists/coproc.f` tồn tại.

## 3. P5.1 · Ba module lá, có testbench

Ba module này là nơi bit-exact được quyết định.

| Module | Việc | Kiểm bằng |
|---|---|---|
| `ecg_mac8.sv` | 8 MAC int8 + cây cộng, ra psum int32 | vector ngẫu nhiên đối chiếu mô hình Python |
| `ecg_requant.sv` | nhân 11 bit, dịch 5 bit, kẹp int8 | **quét toàn bộ** miền đầu vào có nghĩa |
| `ecg_addrgen.sv` | bộ đếm lồng kênh/thời gian + bơm 0 ở biên | đối chiếu chỉ số với `numpy` |

`ecg_requant.sv` là module đáng quét toàn bộ: nó chỉ có 11 + 5 bit tham số và một đầu vào int32,
nên miền cần kiểm đủ nhỏ để không phải lấy mẫu. Nếu tầng này lệch một LSB thì N2 trượt và
nguyên nhân sẽ bị quy oan cho lượng tử hoá.

**Ra:** ba module + ba testbench, tất cả khớp mô hình Python đúng từng bit.

## 4. P5.2 · Đường dữ liệu và bộ đệm

- `ecg_actbuf.sv` — 3.840 B, 13 bộ đệm logic, nhiều nhất 4 sống đồng thời.
  **Một cổng đọc, mux thời gian cho `src1`** (xem §0.3).
- `ecg_wmem.sv` — 18.488 B trọng số + DMA cho `LOADW`, không chặn.
- `ecg_datapath.sv` — ghép mac8, requant, addrgen, actbuf.

**Ra:** một lớp `CONV1D` chạy đúng trên mô phỏng, khớp vector vàng của lớp đó.

## 5. P5.3 · Sequencer và giao diện CV-X-IF

- `ecg_desc.sv` — đọc Model/Layer Descriptor, giải nén trường theo `ecg_pkg`.
- `ecg_seq.sv` — FSM: nạp descriptor → phát vòng lặp → requant → ghi → lớp sau.
- `ecg_cvxif.sv` — giải mã `ECG_MAJOR_OPCODE`, bắt tay issue, trả `rd`.
- `ecg_coproc.sv` — đỉnh.

Rào chắn phải giữ: mọi lệnh tính toán **không chặn**; chỉ `store.barrier` chặn. Đó là điều kiện
để `LOADW` của lớp i+1 che lấp lên tính toán của lớp i, và là lý do T_switch nhỏ được.

**Ra:** cả bốn mô hình chạy trọn trên mô phỏng, khớp vector vàng — tức **N2 sơ bộ** ở mức RTL.

## 6. P5.4 · Tổng hợp thử trên Vivado (máy người dùng)

- Tạo dự án Vivado ở **chế độ non-project** bằng TCL, để tái lập được và ghi vào git.
- Tổng hợp `ecg_coproc` riêng trước, rồi ghép CV32E40X.
- Ràng buộc thời gian: bắt đầu 50 MHz, ghi lại đường tới hạn thật.

**Ra:** `90-results/tables/synth-resources.csv` — LUT, FF, BRAM, DSP thật; và **N7, N9 đo bằng
báo cáo tổng hợp, không phải ước lượng**.

## 7. Rủi ro đã biết trước

| Rủi ro | Vì sao | Xử |
|---|---|---|
| N9 trượt khi tổng hợp | đang đạt bằng 0,1 điểm, dựa trên ước lượng | §0.3: cổng đọc thứ hai phải là mux thời gian ngay từ đầu |
| N7 vỡ vì thêm bộ đệm | dư chỉ 232 B | tính lại bảng §0.1 mỗi lần thêm vùng nhớ |
| BRAM Vivado không suy được từ mã | mẫu viết SRAM không đúng khuôn | `ecg_sram.sv` là **một** chỗ duy nhất chạm bộ nhớ |
| Vector vàng là số thực, đường phần cứng là số nguyên | descriptor.md §6.1 | đường số nguyên đã dựng ở P4; P5 đối chiếu với đường đó |
| `make rtl` trỏ vào `tools/gen/` chưa tồn tại | sót từ kế hoạch P4 | dọn hoặc hiện thực trước P5.0 |

## 8. Điều kiện coi là xong P5

- [ ] `make lint` sạch, **không cảnh báo**, trên toàn bộ RTL
- [ ] Ba module lá khớp mô hình Python đúng từng bit
- [ ] Bốn mô hình chạy trọn trên mô phỏng, khớp vector vàng ở `30-model/golden/`
- [ ] Báo cáo tổng hợp Vivado thật cho XC7Z020, có trong `90-results/tables/`
- [ ] **N7 ≤ 24 kB** và **N9 ≥ 90 %** xác nhận bằng báo cáo đó, không bằng ước lượng
- [ ] **N6 ≥ 4** — bốn mô hình cùng thường trú, chứng minh bằng bản đồ bộ nhớ
- [ ] Mọi số sinh lại được bằng một mục `make`

Đủ bảy cái này thì T12 có đủ cơ sở, và P6 (kiểm chứng) có RTL để đối chiếu.

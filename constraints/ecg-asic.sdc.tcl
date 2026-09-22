# Rang buoc thoi gian, DUNG CHO CA HAI luong: tools/asic-pnr.tcl (chay tu dau) va
# tools/asic-drt.tcl (chay lai tu diem luu sau global route). Tach ra mot tep vi
# read_db KHONG luu SDC: mot ban sao thu hai cua khoi nay se troi ngay lan sua dau
# tien, va mot slack do voi rang buoc khac thi khong so duoc voi slack cu.
#
# Can bien $period truoc khi source.
create_clock -name clk -period $period [get_ports clk_i]
# `remove_from_collection` khong co trong OpenSTA cua OpenROAD, nen loc bang Tcl:
# dat input delay tren cong dong ho lam OpenSTA canh bao va co the lam lech phan
# tich, nen phai bo no ra that su chu khong dua vao viec cong cu tu bo qua.
set data_in {}
foreach p [get_ports *] {
  set nm [get_name $p]
  if {$nm eq "clk_i"} { continue }
  if {[get_property $p direction] eq "input"} { lappend data_in $p }
}
if {[llength $data_in]} {
  # Ti le ngan sach tre I/O. Voi mot khoi DUNG RIENG, dat 10 % vao va 10 % ra nghia
# la 20 % chu ky bi tru truoc khi logic duoc dung -- va khoi do KHONG dung rieng
# trong thiet ke that. Bien nay de do duoc ca hai cach: ECG_IO_FRAC=0 cho duong
# reg-to-reg thuan, mac dinh 0.1 cho ngan sach thu.
#
# Vi sao can: `ecg_seq_vec` do duoc 57,4 MHz trong khi `ecg_requant` -- mot khoi
# NAM TRONG no -- do duoc 42,3 MHz. Mot module chua X khong the nhanh hon X neu hai
# phep do so duoc, nen bat nhat do la dau hieu chinh phep do khac nhau chu khong
# phai thiet ke la.
set io_frac 0.1
if {[info exists ::env(ECG_IO_FRAC)]} { set io_frac $::env(ECG_IO_FRAC) }
set_input_delay -clock clk [expr {$period * $io_frac}] $data_in
}
set_output_delay -clock clk [expr {$period * $io_frac}] [all_outputs]
# Tai dau ra: mot cell dem vua phai, khong de mac dinh 0 vi 0 lam thoi gian lac quan.
set_load 0.05 [all_outputs]

# ------------------------------------------------------------------ P23: rst_ni
# `rst_ni` KHONG co cay dem trong netlist tong hop: do duoc trong
# 70-asic/build/ecg_seq.pnr.v, 338 flop noi THANG chan RESET_B vao cong rst_ni, va
# trong DB sau dinh tuyen net rst_ni co 364 iterm voi 1 bterm. Hau qua do duoc bang
# `report_check_types -max_slew -violators` tren DB da dinh tuyen voi SPEF THAT:
# 222 vi pham max_slew, trong do 157 (70,7 %) o chan RESET_B, slew 2,17 ns tren
# gioi han 1,50 ns cua liberty.
#
# (Nhiem vu vao voi con so "446/480". Do lai hom nay ra 157/222 -- xem log
# 90-results/logs/p23-do-lai-nen-20260902T033336Z.log. Khong tep log nao trong
# 90-results/logs chua cap 446/480, ke ca cau hinh khong PDN hay iter 5. Nen con so
# duoc dung o day la con so DO LAI.)
#
# Vi sao repair_design san co KHONG tu sua: rang buoc no thay chi den tu liberty, va
# truoc dinh tuyen ky sinh la UOC LUONG tu vi tri. O giai doan global route, chinh
# bao cao nay co 35 vi pham va KHONG cai nao o RESET_B -- slew uoc luong cua rst_ni
# chua vuot nguong. Vi pham chi hien ra khi co ky sinh THAT tu OpenRCX, luc do da
# qua muon de chen cell. Nen phai RANG BUOC TAY de ep chia net.
#
# Vi sao mac dinh la 64 chu khong nho hon -- DO CA HAI:
#   fanout 64: 7 cell dem, dien tich 88.419 -> 88.611 um2 (+192), max_slew 222 -> 0,
#              met3.6 269 (KHONG doi), khong sinh luat DRC moi nao.
#   fanout 16: 28 cell dem, dien tich -> 88.943 um2 (+524), max_slew cung 222 -> 0,
#              NHUNG met3.6 269 -> 300 va sinh 6 lop luat truoc do bang 0:
#              abut/overlap giua subcell 84, via3.2 24, met5.2 20, via2.2 20,
#              overlap 18, via.2 12. Tong thuoc thiet ke 269 -> 478.
# Tuc gioi han chat hon KHONG mua them gi (slew da ve 0 o 64) ma tra bang DRC va
# dien tich. 64 la diem duoc nhan; 16 bi BAC BO bang so.
#
# Slack thi KHONG doi trong pham vi nhieu: +1,0508 (nen) / +1,0249 (fo 64) /
# +1,0984 (fo 16) ns. Bien do ~0,05 ns la 0,2 % chu ky va KHONG don dieu theo so
# cell dem, nen do la nhieu cua mot lan dinh tuyen khac, khong phai tac dung cua
# phep sua. Khong duoc bao +0,048 ns cua fo 16 nhu mot cai loi.
#
# ECG_RST_FANOUT=0 de TAT (tro lai hien trang 222 vi pham) khi can do lai nen.
set rst_fanout 64
if {[info exists ::env(ECG_RST_FANOUT)]} { set rst_fanout $::env(ECG_RST_FANOUT) }
if {$rst_fanout ne "0"} {
  # Module khong co cong rst_ni (vi du mot khoi to hop) thi bo qua, khong bao loi.
  set rst_port [get_ports rst_ni]
  if {$rst_port ne ""} {
    set_max_fanout $rst_fanout $rst_port
    puts "##### SDC: set_max_fanout $rst_fanout tren cong rst_ni (P23)"
  } else {
    puts "##### SDC: khong co cong rst_ni -- bo qua set_max_fanout (P23)"
  }
}

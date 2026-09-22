# Tap nguon cua `tb_xif_bridge_wrap`, SINH RA tu dong lenh da khai trien cua dich
# `make sim-xif-bridge` (xem tools/gen_filelists.py). DUNG SUA TAY: Makefile la
# noi cac danh sach nguon that su song, va mot ban sao go tay o day se
# tao ra su that thu hai. `make checks` so lai tung byte.
#
# CACH DUNG, va tep nay KHONG mang du: no chi chua NGUON, khong
# chua duong tim (`-y`) hay duong include (`-I`). Da kiem chay duoc:
#   verilator --lint-only -sv -Wall -Wno-DECLFILENAME -Wno-UNUSEDPARAM \
#     -Iinclude -y src/coproc -y src/common \
#     -f filelists/tb_xif_bridge_wrap.f --top-module tb_xif_bridge_wrap
src/core/rtl/cv32e40x_if_xif.sv
src/soc/ecg_xif_bridge.sv
tb/tb_xif_bridge_wrap.sv

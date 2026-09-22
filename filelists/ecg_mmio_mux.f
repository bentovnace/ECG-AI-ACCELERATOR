# Tap nguon cua `ecg_mmio_mux`, SINH RA tu dong lenh da khai trien cua dich
# `make sim-mmio-mux` (xem tools/gen_filelists.py). DUNG SUA TAY: Makefile la
# noi cac danh sach nguon that su song, va mot ban sao go tay o day se
# tao ra su that thu hai. `make checks` so lai tung byte.
#
# CACH DUNG, va tep nay KHONG mang du: no chi chua NGUON, khong
# chua duong tim (`-y`) hay duong include (`-I`). Da kiem chay duoc:
#   verilator --lint-only -sv -Wall -Wno-DECLFILENAME -Wno-UNUSEDPARAM \
#     -Iinclude -y src/coproc -y src/common \
#     -f filelists/ecg_mmio_mux.f --top-module ecg_mmio_mux
src/soc/ecg_mmio_mux.sv

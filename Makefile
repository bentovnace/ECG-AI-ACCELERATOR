#=============================================================================
# ECG-AI-ACCELERATOR Makefile
# Multi-mode ECG AI Accelerator Core & SoC Platform
# Target Board: Digilent Arty Z7-20 (Xilinx Zynq-7000 XC7Z020-CLG400-1)
#=============================================================================

# Tim duong dan trinh bien dich Vivado (uu tien trong PATH, fallback thu muc cai dat)
VIVADO ?= $(shell which vivado 2>/dev/null || echo "/work/Vivado/2025.2.1/Vivado/bin/vivado")

# Part mac dinh: xc7z020clg400-1 (Arty Z7-20), co the truyen override: make synth PART=xc7a100tcsg324-1
PART   ?= xc7z020clg400-1

PROJ_DIR = fpga/vivado_project
PROJ_XPR = $(PROJ_DIR)/ecg_ai_accelerator.xpr

.PHONY: all help vivado vivado-gui vivado-batch synth synth-coproc sim sim-mac8 sim-requant submodules clean distclean

# Mac dinh: hien thi tro giup
all: help

help:
	@echo "======================================================================"
	@echo "         ECG-AI-ACCELERATOR - HE THONG VI MACH AI TANG TOC ECG         "
	@echo "======================================================================"
	@echo "Cac lenh Makefile khoi tao va dieu khien Vivado:"
	@echo "  make vivado        - Tao project Vivado day du va mo giao dien GUI"
	@echo "  make vivado-gui    - Mo project Vivado hien co (hoac tao moi neu chua co)"
	@echo "  make vivado-batch  - Tao project Vivado o che do dong lenh (Batch mode)"
	@echo "  make synth-coproc  - Tong hop out-of-context bo tang toc AI (ecg_coproc),"
	@echo "                       do tai nguyen LUT/FF/DSP/BRAM va uoc tinh Fmax"
	@echo "  make sim-mac8      - Chay mo phong Vivado xsim cho engine MAC8 (tb_ecg_mac8_sim)"
	@echo "  make sim-requant   - Chay mo phong Vivado xsim cho khoi Requantization"
	@echo "  make submodules    - Clone va cap nhat submodule loi RISC-V (cv32e40x)"
	@echo "  make clean         - Xoa tep nhat ky va rac tam thoi cua Vivado (*.log, *.jou...)"
	@echo "  make distclean     - Xoa toan bo project Vivado da sinh ra de tao lai tu dau"
	@echo "======================================================================"

# Khoi tao project Vivado va mo GUI
vivado: vivado-gui

vivado-gui:
	@if [ -f "$(PROJ_XPR)" ]; then \
		echo "=== Mo project Vivado hien co: $(PROJ_XPR) ==="; \
		$(VIVADO) $(PROJ_XPR) & \
	else \
		echo "=== Khoi tao project Vivado moi va mo GUI ==="; \
		$(VIVADO) -mode tcl -source fpga/scripts/create_project.tcl; \
	fi

# Khoi tao project o che do batch (khong mo GUI)
vivado-batch:
	@echo "=== Tao project Vivado o che do Batch ==="
	@BATCH_MODE=1 $(VIVADO) -mode batch -nojournal -nolog -source fpga/scripts/create_project.tcl

# Tong hop out-of-context bo tang toc AI (ecg_coproc)
synth: synth-coproc
synth-coproc:
	@echo "=== Chay tong hop out-of-context ecg_coproc tren $(PART) ==="
	@$(VIVADO) -mode batch -nojournal -nolog -source fpga/scripts/synth_coproc.tcl -tclargs $(PART)

# Mo phong Vivado xsim cho testbench SystemVerilog
sim: sim-mac8

sim-mac8:
	@echo "=== Mo phong tb_ecg_mac8_sim tren Vivado xsim ==="
	@mkdir -p build/sim
	@xvlog -sv include/ecg_pkg.sv src/coproc/ecg_mac8.sv tb/tb_ecg_mac8_sim.sv -i include
	@xelab -timescale 1ns/1ps -top tb_ecg_mac8_sim -s tb_mac8_sim
	@xsim tb_mac8_sim -runall

sim-requant:
	@echo "=== Mo phong tb_ecg_requant_sim tren Vivado xsim ==="
	@mkdir -p build/sim
	@xvlog -sv include/ecg_pkg.sv src/coproc/ecg_requant.sv tb/tb_ecg_requant_sim.sv -i include
	@xelab -timescale 1ns/1ps -top tb_ecg_requant_sim -s tb_requant_sim
	@xsim tb_requant_sim -runall

# Khoi tao / dong bo submodule Git
submodules:
	@echo "=== Cap nhat Git Submodule (CV32E40X Core) ==="
	git submodule update --init --recursive

# Don dep cac file rac sinh ra trong qua trinh chay Vivado
clean:
	@echo "=== Don dep file tam cua Vivado ==="
	rm -rf *.log *.jou *.str *.pb *.wdb .Xil xsim.dir
	rm -f hs_err_*.log xvlog.pb xelab.pb

# Xoa ca project Vivado da sinh ra
distclean: clean
	@echo "=== Xoa toan bo project Vivado sinh ra ==="
	rm -rf $(PROJ_DIR) reports

# ECG-AI-ACCELERATOR

[![SystemVerilog](https://img.shields.io/badge/Language-SystemVerilog-blue.svg)](https://en.wikipedia.org/wiki/SystemVerilog)
[![RISC-V](https://img.shields.io/badge/Architecture-RISC--V%20CV--X--IF-red.svg)](https://openhwgroup.org/)
[![Target](https://img.shields.io/badge/Target-Arty%20Z7--20%20(XC7Z020)-brightgreen.svg)](https://digilent.com/reference/programmable-logic/arty-z7/start)
[![EDA](https://img.shields.io/badge/EDA-AMD%20Vivado%202025%2B-orange.svg)](https://www.xilinx.com/products/design-tools/vivado.html)

Multi-mode ECG Deep Learning Coprocessor & SoC Platform based on **RISC-V CV-X-IF** (Core-V eXtension Interface). The accelerator provides hardware acceleration for 1D-CNN inference on 12-lead and single-lead ECG signals, achieving ultra-low power consumption and real-time arrhythmia classification.

---

## 🏗️ Architecture Overview

```
                          +------------------------------------------+
                          |        RISC-V Core (CV32E40X)            |
                          +--------------------+---------------------+
                                               |
                                     CV-X-IF   | Instruction Offload
                                               v
+------------------------------------------------------------------------------------+
|                                 ECG COPROCESSOR                                    |
|                                                                                    |
|  +--------------------+   +--------------------+   +----------------------------+  |
|  |     ecg_desc       |-->|     ecg_seq        |-->|       ecg_addrgen          |  |
|  | (12 Opcode Decode) |   |  (FSM Controller)  |   |  (Address Generation Unit) |  |
|  +--------------------+   +--------------------+   +--------------+-------------+  |
|                                                                   |                |
|  +--------------------+   +--------------------+                  |                |
|  |     ecg_wmem       |   |     ecg_actbuf     |                  |                |
|  | (Weight SRAM 256x32|   | (Act SRAM 128x8b)  |                  |                |
|  +---------+----------+   +----------+---------+                  |                |
|            |                         |                            |                |
|            +------------+------------+                            |                |
|                         |                                         v                |
|                         v                           +----------------------------+ |
|            +-------------------------+              |         ecg_vecop          | |
|            |        ecg_mac8         |              |  (Vector Ops: MAXPOOL,     | |
|            |  (8-way Parallel INT8)  |              |   GLOBAL AVG POOL, ADD)    | |
|            +------------+------------+              +-------------+--------------+ |
|                         |                                         |                |
|                         +--------------------+--------------------+                |
|                                              v                                     |
|                                 +-------------------------+                        |
|                                 |       ecg_requant       |                        |
|                                 | (INT32 -> INT8 Scaler)  |                        |
|                                 +-------------------------+                        |
+------------------------------------------------------------------------------------+
```

### Key Technical Specifications
* **Core Engine:** 8 parallel INT8 multiply-accumulate units (`ecg_mac8`) with 24-bit accumulator.
* **Vector Operations:** Dedicated units for Max Pooling, Depthwise/Pointwise Conv, Global Average Pooling (GAP), Element-wise Add (`ecg_vecop`).
* **Requantization:** Hardware scaling with per-channel multipliers, shift clamping, and symmetric rounding (`ecg_requant`).
* **Memory Subsystem:** Dual ping-pong activation buffer (`ecg_actbuf`) + dedicated weight memory (`ecg_wmem`).
* **Coprocessor Interface:** OpenHW CV-X-IF coprocessor interface supporting non-blocking offload, dual writeback, and pipeline kill hazards.
* **SoC Integration:** Complete SoC featuring APB3 interconnect, UART (115200 baud), MMIO registers, and shadow config pipeline.
* **FPGA Target:** Digilent Arty Z7-20 (Zynq-7000 XC7Z020-CLG400-1), 50 MHz system clock, on-chip BRAM.

---

## 📁 Repository Structure

```
.
├── include/                # SystemVerilog global definitions & package
│   └── ecg_pkg.sv          # 12 opcodes, data types, register map
├── src/
│   ├── common/             # Shared memory primitives (SRAM 1R1W, single-port)
│   ├── coproc/             # ECG Coprocessor RTL (MAC8, ActBuf, Requant, etc.)
│   ├── soc/                # SoC integration (Core XIF wrapper, APB, UART, MMIO)
│   └── core/               # Submodule: OpenHW CV32E40X RISC-V core
├── fpga/
│   ├── board/              # Pin constraints (Arty Z7-20 XDC)
│   ├── rtl/                # Board top-level wrapper & clock gating primitives
│   └── scripts/            # Automated Vivado TCL scripts (project creation & synth)
├── tb/                     # Testbenches (SystemVerilog & Verilator C++)
├── filelists/              # Simulation filelists for Verilator / EDA tools
├── formal/                 # Formal verification SymbiYosys (.sby) properties
├── constraints/            # ASIC / SDC timing constraints
└── Makefile                # Master command runner for Vivado, synthesis, & sim
```

---

## 🚀 Quick Start with Vivado

### 1. Clone Repository with Submodules
```bash
git clone --recursive https://github.com/bentovnace/ECG-AI-ACCELERATOR.git
cd ECG-AI-ACCELERATOR
```
*(If already cloned without `--recursive`, run `make submodules`)*

### 2. Launch Vivado Project (GUI Mode)
To automatically create the Vivado `.xpr` project with all source files, constraints, and testbenches, run:
```bash
make vivado
```
This script will configure the Arty Z7-20 part, load SystemVerilog sources, set up include paths, attach constraints, and launch the Vivado GUI.

### 3. Generate Project in Batch Mode (No GUI)
```bash
make vivado-batch
```
The project will be generated at `fpga/vivado_project/ecg_ai_accelerator.xpr`.

### 4. Synthesize AI Accelerator Out-Of-Context
To synthesize `ecg_coproc` standalone, measure exact LUT/FF/DSP/BRAM usage, and estimate Fmax:
```bash
make synth-coproc
```
*Reports will be saved in `reports/`.*

### 5. Run Hardware Simulation
Run Vivado xsim for the 8-way MAC unit:
```bash
make sim-mac8
```
Run simulation for the Requantization unit:
```bash
make sim-requant
```

---

## 🛠️ Verification & Formal Proofs
* **Formal Verification:** SymbiYosys (`sby`) scripts in `formal/` prove arithmetic correctness, bounded model checking (BMC), and protocol safety invariants.
* **Cycle-accurate Co-simulation:** Verilator C++ testbenches in `tb/` verify exact bit-level match against golden PyTorch/INT8 vectors.

---

## 📜 License
This project is open-source under the Apache-2.0 License. See [LICENSE](LICENSE) for details.

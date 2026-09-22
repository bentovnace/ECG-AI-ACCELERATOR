#-----------------------------------------------------------------------------
# fpga/scripts/create_project.tcl
#
# Tu dong tao Project Vivado (.xpr) day du cho ECG-AI-ACCELERATOR trong vai giay.
#
# Cach dung:
#   vivado -mode tcl -source fpga/scripts/create_project.tcl
# Hoac qua Makefile:
#   make vivado
#   make vivado-gui
#   make vivado-batch
#-----------------------------------------------------------------------------

set PROJ_NAME "ecg_ai_accelerator"
set PROJ_DIR  "fpga/vivado_project"
set ROOT      [file normalize [file join [file dirname [info script]] "../.."]]

cd $ROOT

# 1. Chon Part: Uu tien Zynq xc7z020 (Arty Z7-20), fallback Artix-7 xc7a100t
if {[llength [get_parts -quiet xc7z020clg400-1]] > 0} {
    set PART "xc7z020clg400-1"
} else {
    set PART "xc7a100tcsg324-1"
    puts "CANH BAO: Chua co device Zynq xc7z020, dung tam proxy $PART"
}

puts "=== TAO PROJECT VIVADO: $PROJ_NAME ($PART) ==="

# 2. Tao thu muc va project
file mkdir $PROJ_DIR
create_project $PROJ_NAME $PROJ_DIR -part $PART -force

# 3. Tap hop toan bo file nguon SystemVerilog
set CORE "$ROOT/src/core"
set SRCS [list]

# Include package
if {[file exists "$ROOT/include/ecg_pkg.sv"]} {
    lappend SRCS "$ROOT/include/ecg_pkg.sv"
}

# Common & Coprocessor RTL
foreach f [glob -nocomplain "$ROOT/src/common/*.sv"] { lappend SRCS $f }
foreach f [glob -nocomplain "$ROOT/src/coproc/*.sv"] { lappend SRCS $f }
foreach f [glob -nocomplain "$ROOT/src/soc/*.sv"]    { lappend SRCS $f }
foreach f [glob -nocomplain "$ROOT/fpga/rtl/*.sv"]   { lappend SRCS $f }

# CV32E40X Core RTL (neu submodule da duoc clone)
if {[file exists "$CORE/rtl"]} {
    foreach f [glob -nocomplain "$CORE/rtl/include/*.sv"] { lappend SRCS $f }
    foreach f [glob -nocomplain "$CORE/rtl/*.sv"]         { lappend SRCS $f }
} else {
    puts "CANH BAO: Chua co submodule src/core. Chay 'git submodule update --init --recursive' de lay core RISC-V."
}

set SRCS [lsort -unique $SRCS]
add_files -norecurse -fileset sources_1 $SRCS

# 4. Dat Include Directories cho sources_1
set INC_DIRS [list "$ROOT/include"]
if {[file exists "$CORE/rtl/include"]} {
    lappend INC_DIRS "$CORE/rtl/include" "$CORE/bhv" "$CORE/bhv/include"
}
set_property include_dirs $INC_DIRS [get_filesets sources_1]

# 5. Dat Top Module
if {[file exists "$ROOT/fpga/rtl/ecg_soc_board.sv"] && [file exists "$CORE/rtl"]} {
    set_property top ecg_soc_board [get_filesets sources_1]
} else {
    set_property top ecg_coproc [get_filesets sources_1]
}
update_compile_order -fileset sources_1

# 6. Nap tep rang buoc XDC
set XDC "$ROOT/fpga/board/arty_z7_20.xdc"
if {[file exists $XDC]} {
    add_files -fileset constrs_1 -norecurse $XDC
    puts "--- Da nap constraints: $XDC"
}

# 7. Nap cac Testbench SystemVerilog vao sim_1
set TB_SRCS [glob -nocomplain "$ROOT/tb/*.sv"]
if {[llength $TB_SRCS] > 0} {
    add_files -fileset sim_1 -norecurse $TB_SRCS
    set_property include_dirs $INC_DIRS [get_filesets sim_1]
    
    # Dat testbench mac dinh
    if {[file exists "$ROOT/tb/tb_ecg_mac8_sim.sv"]} {
        set_property top tb_ecg_mac8_sim [get_filesets sim_1]
    } elseif {[file exists "$ROOT/tb/tb_loadw_pair_sim.sv"]} {
        set_property top tb_loadw_pair_sim [get_filesets sim_1]
    }
    
    set_property -name {xsim.elaborate.xelab.more_options} -value {-timescale 1ns/1ps} -objects [get_filesets sim_1]
    update_compile_order -fileset sim_1
    puts "--- Da nap [llength $TB_SRCS] testbench SystemVerilog vao sim_1"
}

puts "\n============================================================"
puts "=== TAO PROJECT THANH CONG!"
puts "=== File project nam tai: $PROJ_DIR/$PROJ_NAME.xpr"
puts "=== Mo lai: vivado $PROJ_DIR/$PROJ_NAME.xpr &"
puts "============================================================\n"

# 8. Mo giao dien GUI neu khong o batch mode
if {![info exists env(BATCH_MODE)] || $env(BATCH_MODE) != 1} {
    if {[info commands start_gui] ne ""} {
        start_gui
    }
}

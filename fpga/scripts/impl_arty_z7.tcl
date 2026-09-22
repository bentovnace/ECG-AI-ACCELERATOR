# Implement ecg_soc_board cho bo mach Digilent Arty Z7-20 (xc7z020clg400-1)
# Gan chan that theo 60-fpga/board/arty_z7_20.xdc
#
#     vivado -mode batch -source 60-fpga/vivado/impl_arty_z7.tcl

set PART   "xc7z020clg400-1"
set ROOT   [pwd]
set OUT    $ROOT/60-fpga/measurements
set BIT    $ROOT/60-fpga/bitstream
set XDC    $ROOT/60-fpga/board/arty_z7_20.xdc
file mkdir $OUT $BIT
set STAMP  [clock format [clock seconds] -format %Y%m%dT%H%M%SZ -gmt 1]

if {[llength [get_parts -quiet $PART]] == 0} { error "Khong co part $PART." }

set IMEM_HEX $ROOT/80-firmware/build/fw.hex
set DMEM_HEX $ROOT/80-firmware/build/model.hex
foreach f [list $IMEM_HEX $DMEM_HEX $XDC] {
    if {![file exists $f]} { error "Thieu file `$f`." }
}

puts "\n============================================================"
puts "=== Chay Implement Arty Z7-20 ($PART) voi Pinout that"
puts "============================================================"

set CORE 40-rtl/src/core
set RTL  40-rtl
set SRCS [lsort -unique [concat [lsort [glob $CORE/rtl/include/*.sv]] \
                                [lsort [glob $CORE/rtl/*.sv]] \
                                [lsort [glob 60-fpga/rtl/*.sv]] \
                                [lsort [glob $RTL/include/*.sv]] \
                                [lsort [glob $RTL/src/common/*.sv]] \
                                [lsort [glob $RTL/src/coproc/*.sv]] \
                                [lsort [glob $RTL/src/soc/*.sv]]]]
read_verilog -sv $SRCS
set_property include_dirs [list $RTL/include $CORE/rtl/include $CORE/bhv $CORE/bhv/include] \
                          [current_fileset]

puts "--- Tong hop ([llength $SRCS] nguon), top = ecg_soc_board, generic IMEM/DMEM"
synth_design -top ecg_soc_board -part $PART -flatten_hierarchy rebuilt \
             -generic IMEM_HEX=$IMEM_HEX -generic DMEM_HEX=$DMEM_HEX \
             -generic USE_MMCM=1 -generic RST_ACTIVE_HIGH=1

puts "--- Nap rang buoc chan va timing tu $XDC"
read_xdc $XDC

puts "\n--- Chay Place and Route (opt / place / phys_opt / route)"
opt_design
place_design
phys_opt_design
route_design

set rpt [report_timing_summary -return_string]
set re {([-0-9.]+)\s+([-0-9.]+)\s+(\d+)\s+(\d+)\s+([-0-9.]+)\s+([-0-9.]+)\s+\d+\s+\d+}
set wns ""; set whs ""; set ths ""
if {[regexp -line $re $rpt -> a b c d e f]} { set wns $a; set whs $e; set ths $f }
set unconstrained 0
if {[regexp {There are (\d+) unconstrained endpoints} $rpt -> u]} { set unconstrained $u }

puts "\n--- Ket qua Timing tren Arty Z7-20:"
puts [format "    WNS = %s ns   WHS = %s ns   THS = %s ns   unconstrained_ep = %s" \
      $wns $whs $ths $unconstrained]
set dong 1
foreach {ten gt} [list WNS $wns WHS $whs THS $ths] {
    if {![string is double -strict $gt] || $gt < 0} { set dong 0; puts "    => $ten TRUOT" }
}
if {$unconstrained != 0} { set dong 0; puts "    => CON $unconstrained ENDPOINT KHONG RANG BUOC" }
puts [format "    Timing closure: %s" [expr {$dong ? "DAT (PASS)" : "TRUOT (FAIL)"}]]

report_utilization -file $OUT/arty-z7-utilization-$STAMP.rpt
report_timing_summary -file $OUT/arty-z7-timing-$STAMP.rpt

set bf $BIT/ecg_soc_arty_z7.bit
set bf_stamped $BIT/ecg_soc_arty_z7-$STAMP.bit
if {[catch {write_bitstream -force $bf} e]} {
    puts "\n=== BITSTREAM: KHONG SINH DUOC: $e"
} else {
    file copy -force $bf $bf_stamped
    puts "\n=== BITSTREAM ARTY Z7: thanh cong -> [file tail $bf] ([file size $bf] byte)"
}

puts "============================================================\n"

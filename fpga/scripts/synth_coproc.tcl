#-----------------------------------------------------------------------------
# fpga/scripts/synth_coproc.tcl
#
# Tong hop out-of-context ecg_coproc (AI Accelerator) bang Vivado.
# Do dac tai nguyen (LUT, FF, DSP, BRAM) va Fmax.
#
# Cach dung:
#   vivado -mode batch -source fpga/scripts/synth_coproc.tcl
#   vivado -mode batch -source fpga/scripts/synth_coproc.tcl -tclargs xc7z020clg400-1
# Hoac qua Makefile:
#   make synth-coproc
#-----------------------------------------------------------------------------

set PART   [expr {$argc > 0 ? [lindex $argv 0] : "xc7a100tcsg324-1"}]
set ROOT   [file normalize [file join [file dirname [info script]] "../.."]]
cd $ROOT

set OUTDIR "$ROOT/reports"
set STAMP  [clock format [clock seconds] -format {%Y%m%dT%H%M%S}]
set PERIOD 10.000

file mkdir $OUTDIR

read_verilog -sv [glob "$ROOT/include/*.sv"]
read_verilog -sv [glob "$ROOT/src/common/*.sv"]
read_verilog -sv [glob "$ROOT/src/coproc/*.sv"]
set_property include_dirs [list "$ROOT/include"] [current_fileset]

puts "=== Tong hop out-of-context: ecg_coproc tren $PART ==="
synth_design -top ecg_coproc -part $PART -mode out_of_context -no_iobuf

create_clock -name clk -period $PERIOD [get_ports clk_i]

report_utilization    -file "$OUTDIR/coproc-vivado-util-$STAMP.rpt"
report_timing_summary -file "$OUTDIR/coproc-vivado-timing-$STAMP.rpt"

set nLUT   [llength [get_cells -hier -filter {REF_NAME =~ LUT?}]]
set nFF    [llength [get_cells -hier -filter {REF_NAME =~ FD*}]]
set nCARRY [llength [get_cells -hier -filter {REF_NAME =~ CARRY*}]]
set nDSP   [llength [get_cells -hier -filter {REF_NAME =~ DSP48*}]]
set nBRAM  [llength [get_cells -hier -filter {REF_NAME =~ RAMB*}]]

if {$nLUT == 0 || $nFF == 0} {
    error "Dem cell ve 0 -- loi bo loc get_cells."
}
set wns   [get_property SLACK [get_timing_paths -delay_type max]]
set fmax  [format %.1f [expr {1000.0 / ($PERIOD - $wns)}]]

set csv "$OUTDIR/coproc-vivado.csv"
if {![file exists $csv]} {
    set fh [open $csv w]
    puts $fh "ngay,part,top,lut,ff,carry4,dsp,bram,wns_ns,fmax_mhz,ghi_chu"
} else {
    set fh [open $csv a]
}
puts $fh "$STAMP,$PART,ecg_coproc,$nLUT,$nFF,$nCARRY,$nDSP,$nBRAM,$wns,$fmax,out-of-context tai T=$PERIOD ns"
close $fh

puts "\n============================================================"
puts "  Ket qua tong hop ecg_coproc tren $PART (out-of-context)"
puts "    LUT   : $nLUT"
puts "    FF    : $nFF"
puts "    CARRY4: $nCARRY"
puts "    DSP   : $nDSP"
puts "    BRAM  : $nBRAM"
puts "    WNS   : $wns ns tai T=$PERIOD ns  ->  Fmax ~ $fmax MHz"
puts "  Bao cao luu tai: $OUTDIR"
puts "============================================================\n"

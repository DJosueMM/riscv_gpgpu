set script_dir [file dirname [file normalize [info script]]]
set repo_root  [file normalize "$script_dir/../.."]

set project_file \
    "$repo_root/build/vivado_kv260/riscv_gpgpu_kv260.xpr"

if {![file exists $project_file]} {
    puts "ERROR: Vivado project not found: $project_file"
    puts "Run first: vivado -mode batch -source fpga/scripts/synth_kv260_split_ctrl_v2.tcl"
    exit 1
}

open_project $project_file

set synth_status [get_property STATUS [get_runs synth_1]]
puts "SYNTH STATUS: $synth_status"
if {![string match "*Complete*" $synth_status]} {
    puts "ERROR: synth_1 is not complete. Run synth_kv260_split_ctrl_v2.tcl first."
    exit 1
}

puts ""
puts "======================================================"
puts "STARTING IMPLEMENTATION + BITSTREAM (KRIA KV260, SPLIT-CTRL V2)"
puts "======================================================"

reset_run impl_1
launch_runs impl_1 -to_step write_bitstream -jobs 8
wait_on_run impl_1

set impl_status [get_property STATUS [get_runs impl_1]]
puts "IMPL STATUS: $impl_status"

if {[string match "*Complete*" $impl_status] || [string match "*write_bitstream*" $impl_status]} {
    open_run impl_1
    report_utilization \
        -file "$repo_root/build/vivado_kv260/impl_utilization_split_ctrl_v2.rpt"
    report_timing_summary \
        -file "$repo_root/build/vivado_kv260/impl_timing_split_ctrl_v2.rpt"

    set bit_file "$repo_root/build/vivado_kv260/riscv_gpgpu_kv260.runs/impl_1/gpgpu_system_wrapper.bit"
    if {[file exists $bit_file]} {
        file mkdir "$repo_root/build/vivado_kv260/bitstreams"
        file copy -force $bit_file \
            "$repo_root/build/vivado_kv260/bitstreams/gpgpu_system_wrapper_split_ctrl_v2.bit"
        puts "PASS: bitstream generated -> build/vivado_kv260/bitstreams/gpgpu_system_wrapper_split_ctrl_v2.bit"
    } else {
        puts "ERROR: expected bitstream not found: $bit_file"
        exit 1
    }
} else {
    puts "ERROR: split-ctrl V2 implementation failed."
    exit 1
}

close_project

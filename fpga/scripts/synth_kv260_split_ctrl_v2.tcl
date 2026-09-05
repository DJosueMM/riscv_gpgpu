set script_dir [file dirname [file normalize [info script]]]
set repo_root  [file normalize "$script_dir/../.."]

set project_file \
    "$repo_root/build/vivado_kv260/riscv_gpgpu_kv260.xpr"

set bd_file \
    "$repo_root/build/vivado_kv260/riscv_gpgpu_kv260.srcs/sources_1/bd/gpgpu_system/gpgpu_system.bd"

if {![file exists $project_file]} {
    puts "ERROR: Vivado project not found: $project_file"
    puts "Run first: vivado -mode batch -source fpga/scripts/build_all.tcl"
    exit 1
}

open_project $project_file
open_bd_design $bd_file

set locked_ips [get_ips -quiet -all *]
if {[llength $locked_ips] > 0} {
    set upgrade_needed {}
    foreach ip $locked_ips {
        set is_locked [get_property IS_LOCKED $ip]
        if {$is_locked} {
            lappend upgrade_needed $ip
        }
    }
    if {[llength $upgrade_needed] > 0} {
        puts ""
        puts "Upgrading locked IPs:"
        foreach ip $upgrade_needed {
            puts "  $ip"
        }
        upgrade_ip $upgrade_needed
        save_bd_design
    }
}

puts ""
puts "======================================================"
puts "SPLIT-CTRL V2 SYNTHESIS PREP (KRIA KV260)"
puts "======================================================"

# Vivado aligns the current HLS-generated AXI-Lite slaves to 64 KiB segments,
# so this migration step keeps a dedicated control plane but uses legal aligned
# windows. A truly compact 4 KiB V2 CSR map requires a single RTL wrapper.
assign_bd_address

set_property OFFSET 0xA0000000 \
    [get_bd_addr_segs zynq_ultra_ps_e_0/Data/SEG_gpgpu_scheduler_0_Reg]
set_property RANGE 0x00010000 \
    [get_bd_addr_segs zynq_ultra_ps_e_0/Data/SEG_gpgpu_scheduler_0_Reg]

set_property OFFSET 0xA0010000 \
    [get_bd_addr_segs zynq_ultra_ps_e_0/Data/SEG_gpgpu_scheduler_0_Reg_1]
set_property RANGE 0x00010000 \
    [get_bd_addr_segs zynq_ultra_ps_e_0/Data/SEG_gpgpu_scheduler_0_Reg_1]

set_property OFFSET 0xA0020000 \
    [get_bd_addr_segs zynq_ultra_ps_e_0/Data/SEG_memory_pipeline_0_Reg]
set_property RANGE 0x00010000 \
    [get_bd_addr_segs zynq_ultra_ps_e_0/Data/SEG_memory_pipeline_0_Reg]

set_property OFFSET 0xA0030000 \
    [get_bd_addr_segs zynq_ultra_ps_e_0/Data/SEG_scheduler_status_gpio_0_Reg]
set_property RANGE 0x00010000 \
    [get_bd_addr_segs zynq_ultra_ps_e_0/Data/SEG_scheduler_status_gpio_0_Reg]

puts ""
puts "Control plane address map (Vivado-legal aligned migration view):"
puts "  0xA0000000 : gpgpu_scheduler / s_axi_control"
puts "  0xA0010000 : gpgpu_scheduler / s_axi_control_r"
puts "  0xA0020000 : memory_pipeline / s_axi_control"
puts "  0xA0030000 : scheduler status / AXI GPIO"
puts ""
puts "Data plane (unchanged):"
puts "  gpgpu_scheduler m_axi_gmem{0,1,2} + memory_pipeline m_axi_gmem -> S_AXI_HPC0_FPD"

validate_bd_design
save_bd_design

generate_target all [get_files $bd_file]

set wrapper_file [make_wrapper \
    -files [get_files $bd_file] \
    -top]

add_files -norecurse $wrapper_file
set_property top gpgpu_system_wrapper [current_fileset]
update_compile_order -fileset sources_1

puts ""
puts "======================================================"
puts "STARTING VIVADO SYNTHESIS"
puts "======================================================"

reset_run synth_1
launch_runs synth_1 -jobs 8
wait_on_run synth_1

set synth_status [get_property STATUS [get_runs synth_1]]
puts "SYNTH STATUS: $synth_status"

if {[string match "*Complete*" $synth_status]} {
    open_run synth_1
    report_utilization \
        -file "$repo_root/build/vivado_kv260/synthesis_utilization_split_ctrl_v2.rpt"
    report_timing_summary \
        -file "$repo_root/build/vivado_kv260/synthesis_timing_split_ctrl_v2.rpt"
    puts "PASS: split-ctrl V2 synthesis completed."
} else {
    puts "ERROR: split-ctrl V2 synthesis failed."
    exit 1
}

close_project

# Check if bitstream file and target arguments are provided
if {[llength $::argv] < 2} {
    puts "Usage: vivado -mode batch -source <script.tcl> -tclargs <bitstream_file> <target>"
    exit
}

# Capture arguments from tclargs
set bitstream_file [lindex $::argv 0]
set target [lindex $::argv 1]

# Open hardware manager and connect to server
open_hw_manager
connect_hw_server -allow_non_jtag

# Set the hardware target from tclargs
set current_hw_target [get_hw_targets */$target]
current_hw_target $current_hw_target
open_hw_target

# Select the hardware device
set current_hw_device [get_hw_devices xcvu9p_0]
refresh_hw_device -update_hw_probes false [lindex [get_hw_devices xcvu9p_0] 0]

# Clear any existing probe files
set_property PROBES.FILE {} [get_hw_devices xcvu9p_0]
set_property FULL_PROBES.FILE {} [get_hw_devices xcvu9p_0]

# Set the bitstream file from tclargs
set_property PROGRAM.FILE $bitstream_file [get_hw_devices xcvu9p_0]

# Program the device
program_hw_devices [get_hw_devices xcvu9p_0]
refresh_hw_device [lindex [get_hw_devices xcvu9p_0] 0]

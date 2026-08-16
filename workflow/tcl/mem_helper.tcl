# --- 0.12 ?????xPack OpenOCD 0.12 ?? mem2array/array2mem? ---
# read_memory/write_memory ????/??????????????
# ?? binary?Jim Tcl ??????????????
#   mem2array <arr> <bits> <addr> <count>?arr(0..count-1)=???
proc mem2array {name bits addr count} {
    upvar $name arr
    set vals [read_memory $addr $bits $count]
    set i 0
    foreach v $vals {
        set arr($i) $v
        incr i
    }
}
proc array2mem {name bits addr count} {
    upvar $name arr
    set vals ""
    for {set i 0} {$i < $count} {incr i} {
        append vals " $arr($i)"
    }
    eval write_memory $addr $bits $vals
}
# Helper for common memory read/modify/write procedures

# mrw: "memory read word", returns value of $reg
proc mrw {reg} {
	set value ""
	mem2array value 32 $reg 1
	return $value(0)
}

add_usage_text mrw "address"
add_help_text mrw "Returns value of word in memory."

# mrh: "memory read halfword", returns value of $reg
proc mrh {reg} {
	set value ""
	mem2array value 16 $reg 1
	return $value(0)
}

add_usage_text mrh "address"
add_help_text mrh "Returns value of halfword in memory."

# mrb: "memory read byte", returns value of $reg
proc mrb {reg} {
	set value ""
	mem2array value 8 $reg 1
	return $value(0)
}

add_usage_text mrb "address"
add_help_text mrb "Returns value of byte in memory."

# mmw: "memory modify word", updates value of $reg
#       $reg <== ((value & ~$clearbits) | $setbits)
proc mmw {reg setbits clearbits} {
	set old [mrw $reg]
	set new [expr {($old & ~$clearbits) | $setbits}]
	mww $reg $new
}

add_usage_text mmw "address setbits clearbits"
add_help_text mmw "Modify word in memory. new_val = (old_val & ~clearbits) | setbits;"

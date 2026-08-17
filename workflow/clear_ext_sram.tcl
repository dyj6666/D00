# 清外部 SRAM 关键区（LVGL 堆 128KB + 双缓冲 38KB + LA 预触发 6KB = 0x68080000..0x680B2000）
set base 0x68080000
set size 0x000B2000
for {set i 0} {$i < $size} {incr i 4} {
    mww [expr {$base + $i}] 0
}
puts "ext SRAM cleared: $size bytes"

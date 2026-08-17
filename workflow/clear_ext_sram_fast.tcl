# 清外部 SRAM 关键区（write_memory 批量——target 前缀 + 列表参数）
set base 0x68080000
set total_words 45056
set chunk_words 2048
for {set start 0} {$start < $total_words} {incr start $chunk_words} {
    set n [expr {$total_words - $start}]
    if {$n > $chunk_words} { set n $chunk_words }
    set vals ""
    for {set i 0} {$i < $n} {incr i} {
        append vals " 0"
    }
    stm32f4x.cpu write_memory [expr {$base + $start * 4}] 32 $vals
}
puts "ext SRAM cleared OK: [expr {$total_words * 4}] bytes"

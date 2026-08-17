# 清 CCM（0x10000000 64KB）+ 主 RAM（0x20000000 128KB）——RAM 残留最终测试
# CCM
set base 0x10000000
set total_words 16384
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
puts "CCM cleared OK"
# 主 RAM
set base 0x20000000
set total_words 32768
for {set start 0} {$start < $total_words} {incr start $chunk_words} {
    set n [expr {$total_words - $start}]
    if {$n > $chunk_words} { set n $chunk_words }
    set vals ""
    for {set i 0} {$i < $n} {incr i} {
        append vals " 0"
    }
    stm32f4x.cpu write_memory [expr {$base + $start * 4}] 32 $vals
}
puts "main RAM cleared OK"

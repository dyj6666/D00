# DAP 纭欢璋冭瘯鎵嬪唽锛坉ap_debug.py锛?
> 鎺掓煡 BUG 鐨勭涓€鍘熷垯锛?*鐢?DAP 璇诲瘎瀛樺櫒 / 鏂偣瀹氫綅锛岃€屼笉鏄弽澶嶅姞涓插彛
> 鎵撳嵃銆佸弽澶嶇儳褰?*銆傛湰宸ュ叿璁?Codex 涓庝汉閮借兘浠モ€滅‖浠剁骇鈥濊瑙掔洿鎺ヨ瀵?> 鍐呮牳銆佸唴瀛樹笌澶栬鐨勫疄鏃剁姸鎬併€?
## 1. 鑳藉姏鎬昏

| 鑳藉姏 | 鍛戒护 | 璇存槑 |
| --- | --- | --- |
| 鏆傚仠/鎭㈠/澶嶄綅 | `halt` / `resume` / `reset` | 鍗曟浼氳瘽锛涙柇寮€浼氳瘽鍚庡唴鏍歌嚜鍔ㄦ仮澶嶈繍琛?|
| 璇诲唴鏍稿瘎瀛樺櫒 | `reg pc sp lr r0..r12 msp psp` | 鑷姩绗﹀彿鍖?PC |
| 璇诲唴瀛?澶栬瀵勫瓨鍣?| `read GPIOB_ODR` / `read 0x40020414 2` | 鏀寔澶栬瀵勫瓨鍣ㄥ悕銆佺鍙峰悕銆佽８鍦板潃 |
| 鍐欏瘎瀛樺櫒 | `write ADDR VALUE` | 鍗遍櫓鎿嶄綔锛屼粎鍦ㄦ槑纭渶瑕佹椂浣跨敤 |
| 鏁呴殰瑙ｇ爜 | `fault` | CFSR/HFSR/BFAR/MMFAR + MSP/PSP 寮傚父鏍堝抚 + 宕╂簝鐐圭鍙?|
| 鏍堝洖婧?| `stack --depth 16` | 閫愬瓧灞曞紑鏍堬紝flash 鍊艰嚜鍔ㄧ鍙峰寲 |
| 鏂偣鎺㈡祴 | `bp 绗﹀彿 --wait 1500` | 璁炬柇鐐光啋杩愯鈫掔瓑寰呪啋鎶ュ憡鏄惁鍛戒腑鈫掕嚜鍔ㄦ竻鐞?|
| 浜や簰寮忚皟璇?| `debug` | 鎸佷箙浼氳瘽锛歨alt/step/bp 鐘舵€佽法鍛戒护淇濇寔 |
| 鍦板潃鈫旂鍙?| `sym 0x0803BD92` / `sym 鍑芥暟鍚峘 | 鍙屽悜瑙ｆ瀽 Keil map |
| 澶栬甯冨眬 | `periph [CAN1]` | 鏌ョ湅宸叉敹褰曞璁惧強鍏跺瘎瀛樺櫒鍦板潃 |

## 2. 鐜瑕佹眰

- 寮€鍙戞澘宸蹭笂鐢碉紝CMSIS-DAP 鎺㈤拡宸茶繛鎺ワ紙缁跨伅锛夈€?- OpenOCD锛歚D:\GIT-SPACE\D00\tools\xpack-openocd-0.12.0-7\bin\openocd.exe`銆?- 绗﹀彿琛細璇诲彇 `APP\APP\MDK-ARM\APP\APP.map` 涓?`BOOT.map`锛堟瀯寤哄悗鑷姩鏇存柊锛夈€?- **DAP 鏄崟杩炴帴璁惧**锛氬悓涓€鏃跺埢鍙兘鏈変竴涓?OpenOCD 浼氳瘽锛屼弗绂佸苟琛岃皟鐢ㄦ湰宸ュ叿銆?
## 3. 蹇€熶笂鎵?
```powershell
# 鐪嬪綋鍓嶆墽琛屼綅缃紙鏈€鏈夌敤鐨勭涓€鏉″懡浠わ級
D:\Python\python.exe D:\GIT-SPACE\D00\workflow\dap_debug.py pclist

# 璇诲璁惧瘎瀛樺櫒
D:\Python\python.exe D:\GIT-SPACE\D00\workflow\dap_debug.py read GPIOB_ODR
D:\Python\python.exe D:\GIT-SPACE\D00\workflow\dap_debug.py read I2C1_SR1
D:\Python\python.exe D:\GIT-SPACE\D00\workflow\dap_debug.py read USART3_CR1

# 鎵归噺璇诲唴瀛橈紙4 瀛楄妭瀵归綈鐨勮繛缁瓧锛?D:\Python\python.exe D:\GIT-SPACE\D00\workflow\dap_debug.py read 0x20000000 16

# 瑙ｇ爜鏁呴殰
D:\Python\python.exe D:\GIT-SPACE\D00\workflow\dap_debug.py fault
```

## 4. 鍏稿瀷鎺掓煡鍦烘櫙

### 4.1 绯荤粺鍗℃ / 宕╂簝鍚庢棤鏃ュ織

```powershell
# 1) 鐪?CPU 鐜板湪鍋滃湪鍝紙绗﹀彿鍖栵級
dap_debug.py pclist
# 2) 瑙ｇ爜纭欢鏁呴殰锛堝惈寮傚父鏍堝抚涓庡穿婧冪偣锛?dap_debug.py fault
# 3) 鏍堝洖婧壘璋冪敤閾?dap_debug.py stack --depth 32
```

`fault` 杈撳嚭瑕佺偣锛?- `CFSR` 闈為浂浣嶈鏄庢晠闅滅被鍨嬶紙鎬荤嚎閿欒 / 鐢ㄦ硶閿欒 / 鏍堥敊璇瓑锛夛紱
- `BFAR`/`MMFAR` 鏄嚭閿欏湴鍧€锛岃嫢鍦?flash 浼氭樉绀哄搴旂鍙凤紱
- `MSP/PSP exception frame` 涓殑 `PC` 鍗冲穿婧冪偣锛?- CFSR/HFSR 鍏?0 琛ㄧず**褰撳墠鏃犳椿鍔ㄦ晠闅?*锛堝瘎瀛樺櫒涓哄巻鍙叉畫鐣欙級锛屽簲缁撳悎
  `pclist` 鍒ゆ柇鏄惁鍙槸閫昏緫姝诲惊鐜€?
### 4.2 鎺掓煡澶栬"涓轰粈涔堟病宸ヤ綔"

澶栬闂 90% 鏄椂閽熸病寮€銆佸紩鑴氭ā寮忛敊銆佷娇鑳戒綅娌＄疆锛?
```powershell
# 鏃堕挓鏍戯細妫€鏌ュ搴旀€荤嚎浣胯兘浣?dap_debug.py read RCC_AHB1ENR   # GPIO 鏃堕挓
dap_debug.py read RCC_APB1ENR   # I2C/USART2/3/CAN 鏃堕挓
dap_debug.py read RCC_APB2ENR   # USART1/SPI1 鏃堕挓

# GPIO 鐘舵€侊細MODER 妯″紡 / ODR 杈撳嚭鐢靛钩 / IDR 杈撳叆鐢靛钩
dap_debug.py read GPIOB_MODER
dap_debug.py read GPIOB_IDR
dap_debug.py read GPIOB_ODR

# 澶栬鑷韩鐘舵€?dap_debug.py read I2C1_SR1      # 鏍囧織浣嶏紙BUSY/ADDR/TXE/RXNE锛?dap_debug.py read CAN1_ESR      # CAN 閿欒鐘舵€侊紙LEC/BOFF锛?dap_debug.py read USART3_SR     # 涓插彛鐘舵€?dap_debug.py read TIM2_CNT      # 瀹氭椂鍣ㄨ鏁版槸鍚﹀湪璧?```

### 4.3 鏂偣鎺㈡祴锛?杩欐浠ｇ爜鍒板簳璺戞病璺戯紵"

瀵圭鍙疯缃柇鐐癸紝宸ュ叿浼氳繍琛岀瓑寰呭苟鎶ュ憡鏄惁鍛戒腑锛岀劧鍚庤嚜鍔ㄦ竻鐞嗐€佹仮澶嶇郴缁燂細

```powershell
# 2 绉掑唴 vTaskSwitchContext 鏄惁琚皟鐢?dap_debug.py bp vTaskSwitchContext --wait 2000

# 鑷畾涔夌瓑寰呮椂闂达紙姣锛?dap_debug.py bp 0x0803BD60 --len 2 --wait 5000
```

鍛戒腑杈撳嚭绀轰緥锛?`HIT bp@vTaskSwitchContext: pc=0x08069FE4 <vTaskSwitchContext+0x0>`

鏈懡涓鏄庤璺緞鏈墽琛岋紙鎴栫瓑寰呯獥鍙ｅお鐭級锛屽彲鍔犲ぇ `--wait`銆?
### 4.4 浜や簰寮忛€愭璋冭瘯锛堟渶寮哄ぇ妯″紡锛?
```powershell
dap_debug.py debug
```

杩涘叆鎸佷箙浼氳瘽鍚?halt/鏂偣/鍗曟鐘舵€?*璺ㄥ懡浠や繚鎸?*锛岀瓑鍚?Keil 鍦ㄧ嚎璋冭瘯锛?
```
dap> bp 0x08069FE4 2 hw      # 璁剧‖浠舵柇鐐癸紙娉ㄦ剰 OpenOCD 璇硶蹇呴』甯﹂暱搴︼級
dap> resume                  # 缁х画杩愯
dap> sleep 1200              # 绛?1.2 绉掞紙OpenOCD 鍛戒护锛屾湡闂存柇鐐瑰懡涓細鏆傚仠锛?dap> reg pc                  # 鍛戒腑鍚?PC 鍋滃湪鏂偣
dap> step                    # 鍗曟涓€鏉℃寚浠?dap> reg r0 r1               # 鐪嬪嚱鏁板弬鏁?dap> rbp all                 # 娓呴櫎鎵€鏈夋柇鐐?dap> resume                  # 鎭㈠绯荤粺杩愯
dap> quit                    # 閫€鍑猴紙鑷姩 shutdown锛?```

甯哥敤 OpenOCD 鍛戒护锛歚reg`銆乣mdw/mdh/mdb`銆乣bp <addr> <len> hw`銆乣wp <addr> <len> rw`銆?`step`銆乣resume`銆乣halt`銆乣reset`銆乣rbp all`銆乣shutdown`銆?
### 4.5 杩借釜鍑芥暟璋冪敤锛堟爤涓婄殑杩斿洖鍦板潃锛?
```powershell
dap_debug.py stack --depth 32
```

flash 鑼冨洿鍐呯殑瀛椾細鑷姩鏍囨敞绗﹀彿锛堝 `<modules_init+0x39>`锛夛紝鍙嵁姝ら噸寤鸿皟鐢ㄩ摼銆?
## 5. 瀹夊叏绾﹀畾锛堥噸瑕侊級

1. **鐪嬮棬鐙楄嚜鍔ㄥ喕缁?*锛氬彂甯冩瀯寤猴紙`APP_DEBUG_MODE=0`锛塈WDG 寮€鍚紝鏅€?halt 瓒呰繃
   鐪嬮棬鐙楀懆鏈熶細澶嶄綅鏁存澘銆傛湰宸ュ叿鍦ㄦ瘡涓細璇濊嚜鍔ㄥ啓 `DBGMCU_CR=0x1F`锛宧alt 鏈熼棿
   鍐荤粨 IWDG/WWDG/瀹氭椂鍣紝**鏂偣鎸傚涔呴兘涓嶄細琚浣嶆墦鏂?*銆傝浣嶅彧鍦?halt 鏃剁敓鏁堬紝
   姝ｅ父杩愯涓嶅彈褰卞搷銆?2. **鍗曟浼氳瘽鑷姩鎭㈠**锛歚pclist`/`read`/`fault`/`stack` 绛夊懡浠ら噰闆嗗畬鏁版嵁鍗?   `resume` 骞舵柇寮€锛屾澘瀛愮户缁甯歌繍琛岋紝涓嶄細鍋滅暀鍦ㄦ寕璧锋€併€傞渶瑕佹寔缁寕璧锋帓鏌ユ椂
   浣跨敤 `debug` 浜や簰妯″紡銆?3. **DAP 鍗曡繛鎺?*锛氱粷涓嶅苟琛屾墽琛屼袱涓懡浠わ紙浼氫簰鏂ヨ秴鏃讹級銆?4. **鍐欐搷浣滆皑鎱?*锛歚write` 鐩存帴鏀圭‖浠剁姸鎬侊紝纭鍦板潃涓庡€兼棤璇悗鍐嶆墽琛岋紱寤鸿鍙啓
   璋冭瘯瀵勫瓨鍣ㄦ垨鏄庣‘宸茬煡鐨?RAM 鍙橀噺銆?5. **Thumb 浣嶅凡灞忚斀**锛歮ap 绗﹀彿鍦板潃鍚?bit0=1锛圱humb 鏍囪锛夛紝宸ュ叿宸茶嚜鍔ㄥ睆钄斤紝
   `bp`/`read` 鐢ㄥ伓鍦板潃锛屾棤闇€鎵嬪姩澶勭悊銆?6. **璋冭瘯鍚庣‘璁ょ郴缁熸仮澶?*锛氫氦浜掍細璇濋€€鍑哄墠鎵ц `resume` 鎴?`reset`锛涜嫢鏉垮瓙琛屼负
   寮傚父锛堝鍋滃湪 BOOT锛夛紝鍏?`reset` 涓€娆″啀瑙傚療銆?
## 6. 涓?AI 宸ヤ綔娴佺殑閰嶅悎

- 澶嶇幇 BUG 鍚?*绗竴鍔ㄤ綔**鏄?`pclist` + `fault`锛岀敤璇佹嵁瀹氫綅锛岃€屼笉鏄洸鍔犳墦鍗般€?- 鎬€鐤戞煇涓矾寰勬湭鎵ц 鈫?`bp` 鎺㈡祴锛涢渶瑕侀€愭瑙傚療 鈫?`debug` 浜や簰浼氳瘽銆?- 瀵勫瓨鍣ㄨ瘉鎹璁板綍鍒?`ENGINEERING_LOG.md`锛堝惈鍦板潃銆佸€笺€佺鍙峰寲缁撴灉锛夈€?- 娑夊強鍗忚/鏃跺簭/骞跺彂鐨勯棶棰橈紝鎸?`docs\ISSUE_POSTMORTEM_TEMPLATE.md` 澶嶇洏銆?
## 7. 工程约定（2026-08-16 更新）

1. **垫片只放项目空间**：OpenOCD 0.12 的 mem2array/array2mem 原生实现无效，
   兼容垫片位于 workflow/tcl/mem_helper.tcl（dap_debug.py 已把该目录放在
   -s 搜索路径首位）。严禁在用户目录（如 %APPDATA%\OpenOCD）再建脚本。
2. **DBGMCU_CR 冻结位不残留**：halt 时注入 0x7F 冻结 IWDG/WWDG/TIM，
   会话结束 resume 前自动写回 0x00000000，避免冻结位残留影响运行态。
3. **发布构建下 DAP 会话后系统可能软复位**：实测每次 halt/resume 会话后
   link uptime 归零、uwTick 重新计数（任务级软件看门狗 WDOG 判定静默超时）。
   需要挂起排查时优先用 debug 交互模式并尽量缩短 halt 时间，或使用
   APP_DEBUG_MODE=1 构建（WDOG/IWDG 关闭）。

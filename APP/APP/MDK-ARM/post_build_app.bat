@echo off
rem ============================================================
rem Keil APP post-build: merge two load regions into APP.bin
rem   ER_IROM1 (main image) + ER_FOOTER (magic/version, 8B)
rem   dump to temp dir -> concatenate in address order -> cleanup
rem ============================================================
if exist .\Output\_img rd /s /q .\Output\_img
D:\MDK\CORE\ARM\ARMCC\bin\fromelf.exe --bin --output=.\Output\_img .\APP\APP.axf
copy /b .\Output\_img\ER_IROM1 + .\Output\_img\ER_FOOTER .\Output\APP.bin >nul
rd /s /q .\Output\_img

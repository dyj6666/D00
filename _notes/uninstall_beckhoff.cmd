@echo off
title Beckhoff TwinCAT + VC2010 Uninstaller (Run as Administrator)
echo ============================================
echo  Beckhoff TwinCAT family + VC2010 Express
echo  Please run as Administrator
echo ============================================
echo.
echo [1/22] TwinCAT 3 x64 Driver Package
msiexec /x {E0AA1DBD-698A-4530-AB4B-FC97310A872B} /qn /norestart
echo [2/22] TwinCAT PnP Driver Package
msiexec /x {F6B5CC53-7465-44A0-B6FE-2E801AB63AE7} /qn /norestart
echo [3/22] TwinCAT 3 Application Runtime Libraries
msiexec /x {13C3C61F-A932-4E59-80EF-4F7880F2CC3F} /qn /norestart
echo [4/22] TwinCAT 3 Type System (1)
msiexec /x {585E57B8-D549-4C04-AB05-33FFF82E540E} /qn /norestart
echo [5/22] TwinCAT 3 Type System (2)
msiexec /x {6FF40CEB-0D0B-4D02-9A0B-C8568701071F} /qn /norestart
echo [6/22] TwinCAT 3 Type System (3)
msiexec /x {92921D3C-8DB7-49C1-88C1-AF721B4D9B87} /qn /norestart
echo [7/22] TF3110 TC3 Filter Designer
msiexec /x {12A9A57E-F7A5-4728-A073-4289F143111B} /qn /norestart
echo [8/22] TwinCAT 3 BlockDiagram
msiexec /x {37830FCE-19CF-496B-BB6A-195BF05F637F} /qn /norestart
echo [9/22] TE9000 TwinSAFE Editor
msiexec /x {3E3F1B8F-2BA9-4266-9459-ECDD2B7DE914} /qn /norestart
echo [10/22] TwinCAT Multiuser
msiexec /x {52E7525C-F201-46A3-8B1D-AB44088F077F} /qn /norestart
echo [11/22] TF5210 CNC Export
msiexec /x {58405806-ACB6-46DA-9555-C4BBBDBAF78C} /qn /norestart
echo [12/22] TF3300 Scope Server
msiexec /x {7B1EBA0F-5491-4FD2-9297-C65D6C37D6ED} /qn /norestart
echo [13/22] TwinCAT 3 Measurement Base
msiexec /x {9843E26D-15B0-451D-8B30-0A76C7AAA9B4} /qn /norestart
echo [14/22] TC3 Measurement (InstallShield)
"C:\Program Files (x86)\InstallShield Installation Information\{9E3D9E3D-D0F8-448D-A601-4DE6C3855A94}\TC3-Measurement-Update.exe" -remove -runfromtemp
echo [15/22] Target Browser
msiexec /x {9EA36188-09C8-45DF-88D7-C9DBAC634D81} /qn /norestart
echo [16/22] TE132x Bode Plot
msiexec /x {D8AF4BCC-6BBB-44E5-9E5B-B7D24C984CF1} /qn /norestart
echo [17/22] Support Info Report
msiexec /x {DB18944B-3A70-487E-B1BF-CAF69D6AC308} /qn /norestart
echo [18/22] AML DataExchange
msiexec /x {E6EABCDB-DE91-40D2-A2B9-41D6B3FD562D} /qn /norestart
echo [19/22] TE130x Scope View
msiexec /x {E993A44C-BF16-4F90-96BE-BE9218786140} /qn /norestart
echo [20/22] TwinCAT XAE Shell
msiexec /x {AD9B222F-C4E5-4899-BAFB-4DDAC583B2DD} /qn /norestart
echo [21/22] TwinCAT 3.1 Core (Build 4024)
msiexec /x {80B1FE5D-7E0B-443A-89A2-60CEBA996692} /qn /norestart
echo [22/22] VC2010 Express IDE
msiexec /x {29882478-05D7-3781-AC85-F7828260D3C8} /qn /norestart
echo.
echo ============================================
echo  Done. Error 1605/1619 means already removed.
echo  REBOOT the PC to finish driver removal.
echo ============================================
pause

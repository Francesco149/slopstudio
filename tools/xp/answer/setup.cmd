@echo off
REM slopstudio XP capture harness — first-logon setup (run once via winnt.sif [GuiRunOnce]).
REM Bakes in the agent-less prereqs and stages the probe tools, all from the floppy (A:).

REM --- stage the probe tools to C:\probe ---
mkdir C:\probe 2>nul
mkdir C:\probe\out 2>nul
copy /y A:\*.exe C:\probe\ >nul

REM --- firewall OFF (XP's default-on firewall blocks inbound SMB:445) ---
netsh firewall set opmode disable

REM --- allow blank-password NETWORK logon + classic (non-guest) auth ---
REM    (LimitBlankPasswordUse=0 lets smbexec/iexec authenticate as Administrator with
REM     a blank password; forceguest=0 keeps classic auth so it isn't downgraded to Guest)
reg add "HKLM\SYSTEM\CurrentControlSet\Control\Lsa" /v LimitBlankPasswordUse /t REG_DWORD /d 0 /f
reg add "HKLM\SYSTEM\CurrentControlSet\Control\Lsa" /v forceguest /t REG_DWORD /d 0 /f

REM --- ensure the file-sharing server starts (smbexec/smbclient target ADMIN$/C$) ---
sc config LanmanServer start= auto
net start LanmanServer 2>nul

REM --- persistent autologon as Administrator (blank pw) so a console session always
REM     exists for iexec / QMP screendump, surviving reboots ---
reg add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon" /v AutoAdminLogon /t REG_SZ /d 1 /f
reg add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon" /v DefaultUserName /t REG_SZ /d Administrator /f
reg add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon" /v DefaultPassword /t REG_SZ /d "" /f
reg delete "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon" /v AutoLogonCount /f 2>nul

REM --- a couple of capture-friendly tweaks: no screensaver, solid desktop ---
reg add "HKCU\Control Panel\Desktop" /v ScreenSaveActive /t REG_SZ /d 0 /f

REM --- completion marker the host polls for ---
echo done > C:\probe\out\setup.done

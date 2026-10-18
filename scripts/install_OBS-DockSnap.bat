@echo off
setlocal EnableExtensions DisableDelayedExpansion
title OBS-DockSnap Setup
set "snap_source=%~dp0obs-docksnap"
if not defined ProgramData (
    set "snap_error=Cannot locate the system ProgramData directory."
    goto failed
)
set "snap_target=%ProgramData%\obs-studio\plugins\obs-docksnap"
if not exist "%snap_source%\bin\64bit\obs-docksnap.dll" goto missing_payload
if not exist "%snap_source%\data\locale\en-US.ini" goto missing_payload
if not exist "%snap_source%\data\locale\zh-CN.ini" goto missing_payload
if not exist "%snap_source%\data\locale\zh-TW.ini" goto missing_payload
tasklist /fi "IMAGENAME eq obs64.exe" /nh 2>nul | find /i "obs64.exe" >nul
if not errorlevel 1 (
    set "snap_error=Close OBS Studio, then run this installer again."
    goto failed
)
echo Installing OBS-DockSnap...
echo Destination: "%snap_target%"
robocopy "%snap_source%" "%snap_target%" /e /copy:DAT /dcopy:DAT /r:1 /w:1 /njh /njs /nfl /ndl /np >nul 2>&1
if errorlevel 8 (
    set "snap_error=Copy failed. If access is denied, right-click the BAT and select Run as administrator."
    goto failed
)
echo.
echo Installation completed. Restart OBS and open Tools - DockSnap.
powershell.exe -NoLogo -NoProfile -Command "if([Console]::IsInputRedirected){exit}; for($i=5; $i -gt 0; $i--){[Console]::Write([char]13+'Closing in '+$i+' seconds. Press any key to exit... '); $until=[DateTime]::UtcNow.AddSeconds(1); do{if([Console]::KeyAvailable){$null=[Console]::ReadKey($true); [Console]::WriteLine(); exit}; Start-Sleep -Milliseconds 50}while([DateTime]::UtcNow -lt $until)}; [Console]::WriteLine()"
exit /b 0
:missing_payload
set "snap_error=Missing plugin files. Extract the entire ZIP and keep this BAT next to the obs-docksnap folder."
:failed
echo.
echo Installation failed: %snap_error%
echo Press any key to exit.
pause >nul
exit /b 1

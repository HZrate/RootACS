@echo off
setlocal EnableExtensions

set "SERVICE_NAME=RootACS-Service"
set "INSTALL_BASE=%ProgramFiles%"
if defined ProgramW6432 set "INSTALL_BASE=%ProgramW6432%"
set "INSTALL_ROOT=%INSTALL_BASE%\RootACS"

fltmc >nul 2>&1
if errorlevel 1 (
    powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath '%ComSpec%' -ArgumentList '/c """"%~f0""""' -Verb RunAs"
    exit /b 0
)

taskkill /f /im rootacs.exe >nul 2>&1

call :stop_service
sc delete "%SERVICE_NAME%" >nul 2>&1
call :wait_deleted
call :remove_path

powershell -NoProfile -ExecutionPolicy Bypass -Command "Add-Type -AssemblyName PresentationFramework; [System.Windows.MessageBox]::Show('Removal completed.','RootACS','OK','Information') | Out-Null"
start "" cmd.exe /c ping 127.0.0.1 -n 3 >nul ^& rd /s /q "%INSTALL_ROOT%"
exit /b 0

:stop_service
sc query "%SERVICE_NAME%" >nul 2>&1
if errorlevel 1060 exit /b 0

sc stop "%SERVICE_NAME%" >nul 2>&1
for /l %%I in (1,1,30) do (
    sc query "%SERVICE_NAME%" | find /I "STOPPED" >nul 2>&1 && exit /b 0
    timeout /t 1 /nobreak >nul
)
exit /b 0

:wait_deleted
for /l %%I in (1,1,30) do (
    sc query "%SERVICE_NAME%" >nul 2>&1
    if errorlevel 1060 exit /b 0
    timeout /t 1 /nobreak >nul
)
exit /b 0

:remove_path
powershell -NoProfile -ExecutionPolicy Bypass -Command "$target = [IO.Path]::GetFullPath('%INSTALL_ROOT%'); $machine = [Environment]::GetEnvironmentVariable('Path','Machine'); $parts = @(); if ($machine) { $parts = $machine -split ';' | Where-Object { $_ -and $_.Trim() } }; $filtered = $parts | Where-Object { $_.TrimEnd('\') -ine $target.TrimEnd('\') }; [Environment]::SetEnvironmentVariable('Path', ($filtered -join ';'), 'Machine')"
exit /b 0

@echo off
setlocal EnableExtensions

set "SERVICE_NAME=RootACS-Service"
set "DISPLAY_NAME=RootACS Service"
set "INSTALL_ROOT=%ProgramFiles%\RootACS"
set "SERVICE_EXE=%INSTALL_ROOT%\RootACS-Service.exe"
set "CLIENT_EXE=%INSTALL_ROOT%\rootacs.exe"
set "UNINSTALL_EXE=%INSTALL_ROOT%\uninstall.bat"
set "SCRIPT_ROOT=%~dp0"
set "SOURCE_SERVICE=%SCRIPT_ROOT%RootACS-Service.exe"
set "SOURCE_CLIENT=%SCRIPT_ROOT%rootacs.exe"
set "SOURCE_UNINSTALL=%SCRIPT_ROOT%uninstall.bat"

fltmc >nul 2>&1
if errorlevel 1 (
    powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath '%ComSpec%' -ArgumentList '/c """"%~f0""""' -Verb RunAs"
    exit /b 0
)

if not exist "%SOURCE_SERVICE%" (
    echo Missing file: "%SOURCE_SERVICE%"
    pause
    exit /b 1
)

if not exist "%SOURCE_CLIENT%" (
    echo Missing file: "%SOURCE_CLIENT%"
    pause
    exit /b 1
)

if not exist "%SOURCE_UNINSTALL%" (
    echo Missing file: "%SOURCE_UNINSTALL%"
    pause
    exit /b 1
)

if not exist "%INSTALL_ROOT%" (
    mkdir "%INSTALL_ROOT%" || goto :install_failed
)

call :stop_service || goto :install_failed

if /I not "%SCRIPT_ROOT:~0,-1%"=="%INSTALL_ROOT%" (
    copy /y "%SOURCE_SERVICE%" "%SERVICE_EXE%" >nul || goto :install_failed
    copy /y "%SOURCE_CLIENT%" "%CLIENT_EXE%" >nul || goto :install_failed
    copy /y "%SOURCE_UNINSTALL%" "%UNINSTALL_EXE%" >nul || goto :install_failed
)

sc query "%SERVICE_NAME%" >nul 2>&1
if errorlevel 1060 (
    sc create "%SERVICE_NAME%" binPath= "\"%SERVICE_EXE%\"" start= auto DisplayName= "%DISPLAY_NAME%" >nul || goto :install_failed
) else (
    sc config "%SERVICE_NAME%" binPath= "\"%SERVICE_EXE%\"" start= auto DisplayName= "%DISPLAY_NAME%" >nul || goto :install_failed
)

sc description "%SERVICE_NAME%" "RootACS background service" >nul 2>&1

call :ensure_path || goto :install_failed

sc start "%SERVICE_NAME%" >nul 2>&1
call :wait_running || goto :install_failed

powershell -NoProfile -ExecutionPolicy Bypass -Command "Add-Type -AssemblyName PresentationFramework; [System.Windows.MessageBox]::Show('Installation completed.','RootACS','OK','Information') ^| Out-Null"
if /I not "%SCRIPT_ROOT:~0,-1%"=="%INSTALL_ROOT%" (
    start "" cmd.exe /c ping 127.0.0.1 -n 3 >nul ^& del /f /q "%~f0"
)
exit /b 0

:stop_service
sc query "%SERVICE_NAME%" >nul 2>&1
if errorlevel 1060 exit /b 0

sc stop "%SERVICE_NAME%" >nul 2>&1
for /l %%I in (1,1,30) do (
    sc query "%SERVICE_NAME%" | find /I "STOPPED" >nul 2>&1 && exit /b 0
    timeout /t 1 /nobreak >nul
)
exit /b 1

:wait_running
for /l %%I in (1,1,30) do (
    sc query "%SERVICE_NAME%" | find /I "RUNNING" >nul 2>&1 && exit /b 0
    timeout /t 1 /nobreak >nul
)
exit /b 1

:ensure_path
powershell -NoProfile -ExecutionPolicy Bypass -Command "$target = [IO.Path]::GetFullPath('%INSTALL_ROOT%'); $machine = [Environment]::GetEnvironmentVariable('Path','Machine'); $parts = @(); if ($machine) { $parts = $machine -split ';' ^| Where-Object { $_ -and $_.Trim() } }; if (-not ($parts ^| Where-Object { $_.TrimEnd('\') -ieq $target.TrimEnd('\') })) { [Environment]::SetEnvironmentVariable('Path', (($parts + $target) -join ';'), 'Machine') }"
if errorlevel 1 exit /b 1
set "PATH=%PATH%;%INSTALL_ROOT%"
exit /b 0

:install_failed
echo RootACS installation failed.
pause
exit /b 1

@echo off
rem meter start | stop | restart | status: run the pixelbar column from any prompt.
rem %USERPROFILE%\.local\bin\meter.cmd (on PATH) forwards here. Keep this file ASCII + CRLF.
setlocal
set "EXE=%~dp0bin\pixelbar.exe"
if /i "%~1"=="start" goto start
if /i "%~1"=="stop" goto stop
if /i "%~1"=="restart" goto restart
if /i "%~1"=="status" goto status
echo usage: meter start ^| stop ^| restart ^| status
exit /b 1

:start
call :pid
if defined PID (
  echo pixelbar is already running ^(pid %PID%^)
  exit /b 0
)
:launch
rem Explorer starts it, so it outlives this prompt and any app the prompt runs inside.
explorer.exe "%EXE%"
for /l %%i in (1,1,10) do (
  call :pid
  if defined PID goto started
  ping -n 2 127.0.0.1 >nul
)
echo pixelbar did not start
exit /b 1
:started
echo pixelbar started ^(pid %PID%^)
exit /b 0

:stop
call :pid
if not defined PID (
  echo pixelbar is not running
  exit /b 0
)
rem --quit closes the running copy the way its own Quit does, handing back the reserved strip.
start "" /wait "%EXE%" --quit
call :pid
if defined PID (
  echo pixelbar did not close ^(pid %PID%^); taskkill /f /im pixelbar.exe forces it
  exit /b 1
)
echo pixelbar stopped
exit /b 0

:restart
call :pid
if defined PID start "" /wait "%EXE%" --quit
goto launch

:status
call :pid
if defined PID (echo pixelbar is running ^(pid %PID%^)) else (echo pixelbar is not running)
reg query "HKCU\Software\Microsoft\Windows\CurrentVersion\Run" /v pixelbar >nul 2>&1 && (echo starts with Windows: yes) || (echo starts with Windows: no)
exit /b 0

:pid
set "PID="
for /f "tokens=2 delims=," %%p in ('tasklist /fi "imagename eq pixelbar.exe" /fo csv /nh 2^>nul ^| find /i "pixelbar.exe"') do set "PID=%%~p"
exit /b 0

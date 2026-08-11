@echo off
REM project-atlas 생성기 일괄 실행. 순서: info -> db -> pkt. 단계 실패 시 즉시 중단.
REM 순서는 npm 의 gen:all 과 같다 — 입력 데이터가 스키마·패킷보다 앞선다(design 14).
REM 레포 루트를 cwd 로 잡고 실행한다 — config-loader 가 루트 기준으로 template.ini 를 찾는다.
REM GEN_BATCH_NO_PAUSE=1 이면 종료 시 대기하지 않는다(CI / 스크립트 호출용).
setlocal EnableExtensions DisableDelayedExpansion

set "SCRIPT_DIR=%~dp0"
set "REPO_ROOT=%SCRIPT_DIR%.."
set "BATCH_NAME=%~nx0"
set "LOG_DIR=%SCRIPT_DIR%logs"
if not exist "%LOG_DIR%" mkdir "%LOG_DIR%"

for /f %%I in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd-HHmmss"') do set "RUN_TS=%%I"
set "LOG_FILE=%LOG_DIR%\%~n0-%RUN_TS%.log"
set "EXIT_CODE=0"

cd /d "%REPO_ROOT%"
call :log "[%BATCH_NAME%] log=%LOG_FILE%"

call :run_step "gen-info" "tools/info_generator/info_generator.js"
if %ERRORLEVEL% neq 0 ( set "EXIT_CODE=%ERRORLEVEL%" & goto :finish )

call :run_step "gen-db" "tools/db_generator/db_generator.js"
if %ERRORLEVEL% neq 0 ( set "EXIT_CODE=%ERRORLEVEL%" & goto :finish )

call :run_step "gen-pkt" "tools/pkt_generator/pkt_generator.js"
if %ERRORLEVEL% neq 0 ( set "EXIT_CODE=%ERRORLEVEL%" & goto :finish )

:finish
call :log "[%BATCH_NAME%] exit_code=%EXIT_CODE%"
if not "%GEN_BATCH_NO_PAUSE%"=="1" (
    echo.
    echo Press any key to close...
    pause >nul
)
endlocal & exit /b %EXIT_CODE%

:run_step
call :log "[%BATCH_NAME%] step=%~1"
node "%~2"
exit /b %ERRORLEVEL%

:log
echo %~1
>>"%LOG_FILE%" echo %~1
exit /b 0

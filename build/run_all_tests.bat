@echo off
REM =============================================================================
REM RocketDB - Comprehensive Test Runner
REM =============================================================================
REM Purpose: Build and run ALL 7 test suites with timestamped logs and summary
REM Usage:   run_all_tests.bat [build|test|clean|help]
REM =============================================================================

setlocal enabledelayedexpansion

REM Configuration
set CC=D:\Programs\LLVM\bin\clang.exe
set CFLAGS=-Wall -Wextra -std=c99 -O2 -g -D_CRT_SECURE_NO_WARNINGS
set INCLUDES=-Isrc -Itest\sim
set OUTPUT_DIR=test\out

REM All 7 test executables (in run order: basic first, then stress, then integration/demo)
set TESTS=test_kvdb_basic test_kvdb_stress test_tsdb_basic test_tsdb_stress test_integration test_fault_injection test_example

REM Common source files needed by all tests
set BASE_SRCS=src/rocketdb_kvdb.c src/rocketdb_tsdb.c test\sim\test_framework.c test\sim\sim_flash.c test\sim\sim_fault.c test\sim\sim_crypto.c test\sim\sim_dist.c test\sim\sim_trace.c

set ACTION=%1
if "%ACTION%"=="" set ACTION=test
if "%ACTION%"=="help" goto show_help

REM cd to project root (script is in build\)
cd /d "%~dp0.."

REM Generate master timestamp for this run
for /f "tokens=1-3 delims=/- " %%a in ("%DATE%") do set MASTER_DATESTAMP=%%a%%b%%c
for /f "tokens=1-3 delims=:. " %%a in ("%TIME: =0%") do set MASTER_TIMESTAMP=%%a%%b%%c
set MASTER_TS=%MASTER_DATESTAMP%_%MASTER_TIMESTAMP%

set SUMMARY_LOG=%OUTPUT_DIR%\%MASTER_TS%_SUMMARY.log
set PASSED_TOTAL=0
set FAILED_TOTAL=0

if "%ACTION%"=="clean" goto clean_all
if not exist "%OUTPUT_DIR%" mkdir "%OUTPUT_DIR%"

REM Clean old intermediate files before building
if exist "%OUTPUT_DIR%\*.ilk" del /q "%OUTPUT_DIR%\*.ilk" 2>nul
if exist "%OUTPUT_DIR%\*.pdb" del /q "%OUTPUT_DIR%\*.pdb" 2>nul
if exist "%OUTPUT_DIR%\*.o"   del /q "%OUTPUT_DIR%\*.o"   2>nul

echo ================================================
echo   RocketDB Comprehensive Test Runner
echo   Started: %DATE% %TIME%
echo   Timestamp: %MASTER_TS%
echo ================================================
echo.

set PASS=0
set FAIL=0

if "%ACTION%"=="build" goto build_all

REM ── test (default): compile and run ──────────────────────────────────────

for %%t in (%TESTS%) do (
    call :compile_and_run "%%t" PASS FAIL
)

REM Write master summary
echo ================================================ > "%SUMMARY_LOG%"
echo RocketDB Comprehensive Test Summary >> "%SUMMARY_LOG%"
echo ================================================ >> "%SUMMARY_LOG%"
echo Date: %DATE% %TIME% >> "%SUMMARY_LOG%"
echo Total Passed: %PASS% >> "%SUMMARY_LOG%"
echo Total Failed: %FAIL% >> "%SUMMARY_LOG%"
echo ================================================ >> "%SUMMARY_LOG%"
echo Log files in: %OUTPUT_DIR% >> "%SUMMARY_LOG%"
for %%t in (%TESTS%) do (
    echo   %%t >> "%SUMMARY_LOG%"
)

echo.
echo ================================================
echo   MASTER SUMMARY
echo ================================================
echo   Total Passed:  %PASS%
echo   Total Failed:  %FAIL%
echo   Timestamp:     %MASTER_TS%
echo   Finished:      %DATE% %TIME%
echo ================================================

if %FAIL% GTR 0 (
    echo.
    echo [RESULT] %FAIL% TEST^(S^) FAILED
    echo Summary log: %SUMMARY_LOG%
    exit /b 1
)
echo.
echo [RESULT] ALL %PASS% TESTS PASSED
echo Summary log: %SUMMARY_LOG%
goto end

REM =============================================================================
REM build_all — compile only, no execution
REM =============================================================================
:build_all
echo.
echo [BUILD-ONLY MODE] Compiling all test suites...
echo.
for %%t in (%TESTS%) do (
    call :compile_only "%%t"
)
echo.
echo ================================================
echo   Build complete — all targets compiled
echo   Run with: build\run_all_tests.bat test
echo ================================================
goto end

REM =============================================================================
REM compile_only — build single test, no run
REM =============================================================================
:compile_only
set TEST_NAME=%~1
set TARGET=%OUTPUT_DIR%\%TEST_NAME%.exe
set SRCS=%BASE_SRCS% test\sim\%TEST_NAME%.c

%CC% %CFLAGS% %INCLUDES% -o %TARGET% %SRCS% 2>nul
if errorlevel 1 (
    echo   [BUILD FAIL] %TEST_NAME%
) else (
    echo   [BUILD OK]   %TEST_NAME%
)
goto :eof

REM =============================================================================
REM compile_and_run — build + execute single test, log with timestamp
REM =============================================================================
:compile_and_run
set TEST_NAME=%~1

REM Per-test timestamp
for /f "tokens=1-3 delims=/- " %%a in ("%DATE%") do set TS=%%a%%b%%c
for /f "tokens=1-3 delims=:. " %%a in ("%TIME: =0%") do set TS=!TS!_%%a%%b%%c

set TARGET=%OUTPUT_DIR%\%TEST_NAME%.exe
set LOG_FILE=%OUTPUT_DIR%\!TS!_%TEST_NAME%.log

REM Compile
set SRCS=%BASE_SRCS% test\sim\%TEST_NAME%.c
%CC% %CFLAGS% %INCLUDES% -o %TARGET% %SRCS% 2>"%LOG_FILE%.build_err"
if errorlevel 1 (
    echo   [FAIL] %TEST_NAME% — compile error
    type "%LOG_FILE%.build_err"
    del "%LOG_FILE%.build_err"
    set /a %~3+=1
    goto :eof
)
del "%LOG_FILE%.build_err" 2>nul

REM Run
echo ================================================ > "%LOG_FILE%"
echo RocketDB Test: %TEST_NAME% >> "%LOG_FILE%"
echo Timestamp: !TS! >> "%LOG_FILE%"
echo ================================================ >> "%LOG_FILE%"

%TARGET% >> "%LOG_FILE%" 2>&1
set TEST_ERR=%ERRORLEVEL%

if %TEST_ERR% NEQ 0 (
    echo   [FAIL] %TEST_NAME% — test returned %TEST_ERR%
    set /a %~3+=1
    goto :eof
)
echo   [PASS] %TEST_NAME%  —  !TS!_%TEST_NAME%.log
set /a %~2+=1
goto :eof

REM =============================================================================
:clean_all
echo Cleaning all test outputs...
if exist "%OUTPUT_DIR%" (
    del /q "%OUTPUT_DIR%\*.exe" 2>nul
    del /q "%OUTPUT_DIR%\*.log" 2>nul
    del /q "%OUTPUT_DIR%\*.pdb" 2>nul
    del /q "%OUTPUT_DIR%\*.ilk" 2>nul
    del /q "%OUTPUT_DIR%\*.o" 2>nul
)
echo Clean complete.
goto end

REM =============================================================================
:show_help
echo.
echo ================================================
echo   RocketDB Comprehensive Test Runner
echo ================================================
echo.
echo Usage: run_all_tests.bat [test^|build^|clean^|help]
echo.
echo   test   - Compile and run all 7 test suites (default)
echo   build  - Compile all test suites only (no run)
echo   clean  - Remove all test outputs
echo   help   - Show this help
echo.
echo Test suites (7 total):
echo   test_kvdb_basic        KVDB basic functionality
echo   test_kvdb_stress       KVDB stress + GC + power-loss
echo   test_tsdb_basic        TSDB append/query/epoch
echo   test_tsdb_stress       TSDB rotation stress + CRC + fault
echo   test_integration       KVDB+TSDB combined + mixed workload
echo   test_fault_injection   Fault injection demo
echo   test_example           Tutorial example
echo.
echo Log format: YYYYMMDD_HHMMSS_testname.log
echo Summary:     YYYYMMDD_HHMMSS_SUMMARY.log
echo.
goto end

:end
endlocal

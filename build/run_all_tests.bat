@echo off
REM =============================================================================
REM RocketDB - Comprehensive Test Runner
REM =============================================================================
REM Purpose: Build and run all test suites, then verify simulator Flash dumps
REM          through tools\rdbdump. All generated artifacts go to tests\out.
REM Usage:   run_all_tests.bat [build|test|clean|help]
REM =============================================================================

setlocal enabledelayedexpansion

REM Compiler discovery — try PATH first, then standard locations
set CC=clang.exe
where clang.exe >nul 2>nul
if errorlevel 1 (
    if exist "C:\Program Files\LLVM\bin\clang.exe" set CC=C:\Program Files\LLVM\bin\clang.exe
    if not exist "!CC!" if exist "D:\Programs\LLVM\bin\clang.exe" set CC=D:\Programs\LLVM\bin\clang.exe
    if not exist "!CC!" if exist "D:\Programs\w64devkit\bin\gcc.exe" set CC=D:\Programs\w64devkit\bin\gcc.exe
    if not exist "!CC!" set CC=gcc.exe
)
set CFLAGS=-Wall -Wextra -std=c99 -O2 -g -D_CRT_SECURE_NO_WARNINGS
set INCLUDES=-Isrc -Itests\sim
set OUTPUT_DIR=tests\out
set PYTHON=python

set TESTS=test_kvdb_basic test_kvdb_stress test_kvdb_cache test_tsdb_basic test_tsdb_stress test_integration test_fault_injection test_example
set BASE_SRCS=src\rocketdb_kvdb.c src\rocketdb_tsdb.c tests\sim\test_framework.c tests\sim\sim_flash.c tests\sim\sim_fault.c tests\sim\sim_crypto.c tests\sim\sim_dist.c tests\sim\sim_trace.c

set ACTION=%1
if "%ACTION%"=="" set ACTION=test
if "%ACTION%"=="help" goto show_help

cd /d "%~dp0.."

for /f %%a in ('powershell -NoProfile -Command "Get-Date -Format yyMMdd-HHmmss"') do set MASTER_TS=%%a

set SUMMARY_LOG=%OUTPUT_DIR%\%MASTER_TS%-SUMMARY.log

if "%ACTION%"=="clean" goto clean_all
if not exist "%OUTPUT_DIR%" mkdir "%OUTPUT_DIR%"

if exist "%OUTPUT_DIR%\*.ilk" del /q "%OUTPUT_DIR%\*.ilk" 2>nul
if exist "%OUTPUT_DIR%\*.pdb" del /q "%OUTPUT_DIR%\*.pdb" 2>nul
if exist "%OUTPUT_DIR%\*.o" del /q "%OUTPUT_DIR%\*.o" 2>nul

echo ================================================
echo   RocketDB Comprehensive Test Runner
echo   Started: %DATE% %TIME%
echo   Timestamp: %MASTER_TS%
echo   Output: %OUTPUT_DIR%
echo ================================================
echo.

set PASS=0
set FAIL=0

if "%ACTION%"=="build" goto build_all

for %%t in (%TESTS%) do (
    call :compile_and_run "%%t" PASS FAIL
)
call :run_rdbdump_verify PASS FAIL

echo ================================================ > "%SUMMARY_LOG%"
echo RocketDB Comprehensive Test Summary >> "%SUMMARY_LOG%"
echo ================================================ >> "%SUMMARY_LOG%"
echo Date: %DATE% %TIME% >> "%SUMMARY_LOG%"
echo Total Passed: %PASS% >> "%SUMMARY_LOG%"
echo Total Failed: %FAIL% >> "%SUMMARY_LOG%"
echo Output: %OUTPUT_DIR% >> "%SUMMARY_LOG%"
echo ================================================ >> "%SUMMARY_LOG%"
for %%t in (%TESTS%) do echo   %%t >> "%SUMMARY_LOG%"
echo   rdbdump_offline_verify >> "%SUMMARY_LOG%"

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

:build_all
echo.
echo [BUILD-ONLY MODE] Compiling all test suites...
echo.
for %%t in (%TESTS%) do (
    call :compile_only "%%t"
)
set TARGET=%OUTPUT_DIR%\test_rdbdump_dump.exe
set SRCS=src\rocketdb_kvdb.c src\rocketdb_tsdb.c tests\sim\sim_flash.c tests\sim\sim_fault.c tests\sim\sim_crypto.c tests\sim\sim_trace.c tests\sim\test_rdbdump_dump.c
%CC% %CFLAGS% %INCLUDES% -o %TARGET% %SRCS%
if errorlevel 1 (
    echo   [BUILD FAIL] test_rdbdump_dump
) else (
    echo   [BUILD OK]   test_rdbdump_dump
)
echo.
echo ================================================
echo   Build complete - all targets compiled
echo   Run with: build\run_all_tests.bat test
echo ================================================
goto end

:compile_only
set TEST_NAME=%~1
set TARGET=%OUTPUT_DIR%\%TEST_NAME%.exe
set SRCS=%BASE_SRCS% tests\sim\%TEST_NAME%.c
set EXTRA=
if "%TEST_NAME%"=="test_kvdb_cache" set EXTRA=-DRDB_KV_CACHE_SIZE=64
%CC% %CFLAGS% %EXTRA% %INCLUDES% -o %TARGET% %SRCS% 2>nul
if errorlevel 1 (
    echo   [BUILD FAIL] %TEST_NAME%
) else (
    echo   [BUILD OK]   %TEST_NAME%
)
goto :eof

:compile_and_run
set TEST_NAME=%~1
for /f %%a in ('powershell -NoProfile -Command "Get-Date -Format yyMMdd-HHmmss"') do set TS=%%a
set TARGET=%OUTPUT_DIR%\%TEST_NAME%.exe
set LOG_FILE=%OUTPUT_DIR%\!TS!-%TEST_NAME%.log
set SRCS=%BASE_SRCS% tests\sim\%TEST_NAME%.c
set EXTRA=
if "%TEST_NAME%"=="test_kvdb_cache" set EXTRA=-DRDB_KV_CACHE_SIZE=64

%CC% %CFLAGS% %EXTRA% %INCLUDES% -o %TARGET% %SRCS% 2>"%LOG_FILE%.build_err"
if errorlevel 1 (
    echo   [FAIL] %TEST_NAME% - compile error
    type "%LOG_FILE%.build_err"
    del "%LOG_FILE%.build_err" 2>nul
    set /a %~3+=1
    goto :eof
)
del "%LOG_FILE%.build_err" 2>nul

echo ================================================ > "%LOG_FILE%"
echo RocketDB Test: %TEST_NAME% >> "%LOG_FILE%"
echo Timestamp: !TS! >> "%LOG_FILE%"
echo ================================================ >> "%LOG_FILE%"

%TARGET% >> "%LOG_FILE%" 2>&1
set TEST_ERR=%ERRORLEVEL%
if %TEST_ERR% NEQ 0 (
    echo   [FAIL] %TEST_NAME% - test returned %TEST_ERR%
    set /a %~3+=1
    goto :eof
)
echo   [PASS] %TEST_NAME%  -  !TS!-%TEST_NAME%.log
set /a %~2+=1
goto :eof

:run_rdbdump_verify
for /f %%a in ('powershell -NoProfile -Command "Get-Date -Format yyMMdd-HHmmss"') do set TS=%%a
set TEST_NAME=rdbdump_offline_verify
set TARGET=%OUTPUT_DIR%\test_rdbdump_dump.exe
set LOG_FILE=%OUTPUT_DIR%\!TS!-%TEST_NAME%.log
set EXPORT_DIR=%OUTPUT_DIR%\rdbdump_export\!TS!

echo ================================================ > "%LOG_FILE%"
echo RocketDB Test: %TEST_NAME% >> "%LOG_FILE%"
echo Timestamp: !TS! >> "%LOG_FILE%"
echo Export: %EXPORT_DIR% >> "%LOG_FILE%"
echo ================================================ >> "%LOG_FILE%"

set SRCS=src\rocketdb_kvdb.c src\rocketdb_tsdb.c tests\sim\sim_flash.c tests\sim\sim_fault.c tests\sim\sim_crypto.c tests\sim\sim_trace.c tests\sim\test_rdbdump_dump.c
%CC% %CFLAGS% %INCLUDES% -o %TARGET% %SRCS% >> "%LOG_FILE%" 2>&1
if errorlevel 1 (
    echo   [FAIL] %TEST_NAME% - compile error
    set /a %~2+=1
    goto :eof
)

%TARGET% "%OUTPUT_DIR%" >> "%LOG_FILE%" 2>&1
if errorlevel 1 goto rdbdump_fail
if not exist "%EXPORT_DIR%\kvdb" mkdir "%EXPORT_DIR%\kvdb"
if not exist "%EXPORT_DIR%\tsdb" mkdir "%EXPORT_DIR%\tsdb"

%PYTHON% tools\rdbdump\rdbdump.py verify --strict --manifest "%OUTPUT_DIR%\rdbdump_kvdb.json" --input "%OUTPUT_DIR%\rdbdump_kvdb.bin" >> "%LOG_FILE%" 2>&1
if errorlevel 1 goto rdbdump_fail
%PYTHON% tools\rdbdump\rdbdump.py verify --strict --manifest "%OUTPUT_DIR%\rdbdump_tsdb.json" --input "%OUTPUT_DIR%\rdbdump_tsdb.bin" >> "%LOG_FILE%" 2>&1
if errorlevel 1 goto rdbdump_fail
%PYTHON% tools\rdbdump\rdbdump.py export --manifest "%OUTPUT_DIR%\rdbdump_kvdb.json" --input "%OUTPUT_DIR%\rdbdump_kvdb.bin" --out "%EXPORT_DIR%\kvdb" >> "%LOG_FILE%" 2>&1
if errorlevel 1 goto rdbdump_fail
%PYTHON% tools\rdbdump\rdbdump.py export --manifest "%OUTPUT_DIR%\rdbdump_tsdb.json" --input "%OUTPUT_DIR%\rdbdump_tsdb.bin" --out "%EXPORT_DIR%\tsdb" >> "%LOG_FILE%" 2>&1
if errorlevel 1 goto rdbdump_fail

echo   [PASS] %TEST_NAME%  -  !TS!-%TEST_NAME%.log
echo   [INFO] rdbdump export: %EXPORT_DIR%
set /a %~1+=1
goto :eof

:rdbdump_fail
echo   [FAIL] %TEST_NAME%
set /a %~2+=1
goto :eof

:clean_all
echo Cleaning all test outputs...
if exist "%OUTPUT_DIR%" (
    del /q "%OUTPUT_DIR%\*.exe" 2>nul
    del /q "%OUTPUT_DIR%\*.log" 2>nul
    del /q "%OUTPUT_DIR%\*.pdb" 2>nul
    del /q "%OUTPUT_DIR%\*.ilk" 2>nul
    del /q "%OUTPUT_DIR%\*.o" 2>nul
    del /q "%OUTPUT_DIR%\*.bin" 2>nul
    del /q "%OUTPUT_DIR%\rdbdump_*.json" 2>nul
    if exist "%OUTPUT_DIR%\rdbdump_export" rmdir /s /q "%OUTPUT_DIR%\rdbdump_export"
)
echo Clean complete.
goto end

:show_help
echo.
echo ================================================
echo   RocketDB Comprehensive Test Runner
echo ================================================
echo.
echo Usage: run_all_tests.bat [test^|build^|clean^|help]
echo.
echo   test   - Compile and run all 8 suites, then rdbdump offline verification
echo   build  - Compile all test suites and rdbdump fixture generator only
echo   clean  - Remove generated files in tests\out
echo   help   - Show this help
echo.
echo Output: tests\out
echo Logs:   YYMMDD-HHMMSS-testname.log
echo rdbdump export: tests\out\rdbdump_export\YYMMDD-HHMMSS\
echo.
goto end

:end
endlocal

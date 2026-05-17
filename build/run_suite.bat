@echo off
REM =============================================================================
REM RocketDB - Module Test Suite Runner
REM =============================================================================
REM Purpose: Build and run KVDB-only or TSDB-only tests
REM Usage:   run_suite.bat [kvdb|tsdb] [build|test|clean]
REM =============================================================================

setlocal enabledelayedexpansion

set CC=clang.exe
if exist "D:\Programs\LLVM\bin\clang.exe" set CC=D:\Programs\LLVM\bin\clang.exe
set CFLAGS=-Wall -Wextra -std=c99 -O2 -g -D_CRT_SECURE_NO_WARNINGS
set INCLUDES=-Isrc -Itest\sim
set OUTPUT_DIR=test\out
set BASE_SRCS=src/rocketdb_kvdb.c src/rocketdb_tsdb.c test\sim\test_framework.c test\sim\sim_flash.c test\sim\sim_fault.c test\sim\sim_crypto.c test\sim\sim_dist.c test\sim\sim_trace.c

set SUITE=%1
set ACTION=%2
if "%ACTION%"=="" set ACTION=test

REM cd to project root (script is in build\)
cd /d "%~dp0.."

if "%SUITE%"=="kvdb" set TESTS=test_kvdb_basic test_kvdb_stress
if "%SUITE%"=="tsdb" set TESTS=test_tsdb_basic test_tsdb_stress
if "%TESTS%"=="" (
    echo [ERROR] Unknown suite: %SUITE%
    exit /b 1
)

if "%ACTION%"=="clean" (
    echo Cleaning %SUITE% test outputs...
    for %%t in (%TESTS%) do (
        if exist "%OUTPUT_DIR%\%%t.exe" del /q "%OUTPUT_DIR%\%%t.exe"
    )
    if exist "%OUTPUT_DIR%\*.ilk" del /q "%OUTPUT_DIR%\*.ilk" 2>nul
    if exist "%OUTPUT_DIR%\*.pdb" del /q "%OUTPUT_DIR%\*.pdb" 2>nul
    if exist "%OUTPUT_DIR%\*.o"   del /q "%OUTPUT_DIR%\*.o"   2>nul
    echo Clean complete.
    goto end
)

if not exist "%OUTPUT_DIR%" mkdir "%OUTPUT_DIR%"

REM Clean old intermediate files before building
if exist "%OUTPUT_DIR%\*.ilk" del /q "%OUTPUT_DIR%\*.ilk" 2>nul
if exist "%OUTPUT_DIR%\*.pdb" del /q "%OUTPUT_DIR%\*.pdb" 2>nul
if exist "%OUTPUT_DIR%\*.o"   del /q "%OUTPUT_DIR%\*.o"   2>nul

set PASS=0
set FAIL=0

for %%t in (%TESTS%) do (
    set TARGET=%OUTPUT_DIR%\%%t.exe
    set SRCS=%BASE_SRCS% test\sim\%%t.c

    echo [%%t] Building...
    %CC% %CFLAGS% %INCLUDES% -o !TARGET! !SRCS! 2>nul
    if errorlevel 1 (
        echo [%%t] Compile FAILED
        set /a FAIL+=1
    ) else (
        echo [%%t] Compile OK
        if "%ACTION%"=="test" (
            REM Per-test locale-independent timestamp: YYMMDD-HHMMSS
            for /f %%a in ('powershell -NoProfile -Command "Get-Date -Format yyMMdd-HHmmss"') do set TS=%%a
            set LOG_FILE=%OUTPUT_DIR%\!TS!-%%t.log

            echo ================================================ > "!LOG_FILE!"
            echo RocketDB Test: %%t >> "!LOG_FILE!"
            echo Timestamp: !TS! >> "!LOG_FILE!"
            echo ================================================ >> "!LOG_FILE!"

            !TARGET! >> "!LOG_FILE!" 2>&1
            if errorlevel 1 (
                echo [%%t] Test FAILED
                set /a FAIL+=1
            ) else (
                echo [%%t] Test PASSED  -  !TS!-%%t.log
                set /a PASS+=1
            )
        ) else (
            set /a PASS+=1
        )
    )
)

echo.
echo ========================================
echo   %SUITE% Results: %PASS% passed, %FAIL% failed
echo ========================================

if %FAIL% GTR 0 exit /b 1
goto end

:end
endlocal

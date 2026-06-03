@echo off
REM =============================================================================
REM RocketDB - Performance Benchmark Runner
REM =============================================================================
REM Usage: build_perf.bat [build|run|clean|distclean]
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

set ACTION=%1
if "%ACTION%"=="" set ACTION=build

REM cd to project root (script is in build\)
cd /d "%~dp0.."

if not exist "tests\perf" (
    echo [ERROR] tests\perf directory not found!
    exit /b 1
)
if not exist "%OUTPUT_DIR%" mkdir "%OUTPUT_DIR%"

if "%ACTION%"=="help" (
    echo.
    echo Usage: build_perf.bat [build^|run^|clean^|distclean]
    echo.
    echo Commands:
    echo   build      - Compile performance benchmark
    echo   run        - Build and run benchmark
    echo   clean      - Remove build artifacts
    echo   distclean  - Remove all artifacts and results
    goto end
)

if "%ACTION%"=="build" goto build_perf
if "%ACTION%"=="run" goto run_perf
if "%ACTION%"=="clean" goto clean_perf
if "%ACTION%"=="distclean" goto distclean_perf
echo [ERROR] Unknown action: %ACTION%
exit /b 1

REM =============================================================================
:build_perf
echo [*] Building performance benchmark...

set SRCS=src\rocketdb_kvdb.c src\rocketdb_tsdb.c tests\sim\sim_flash.c tests\sim\sim_fault.c tests\sim\sim_trace.c tests\sim\sim_crypto.c tests\perf\scenarios.c
set TARGET=%OUTPUT_DIR%\benchmark.exe

%CC% %CFLAGS% %INCLUDES% -o %TARGET% %SRCS%
if errorlevel 1 (
    echo [ERROR] Compilation failed!
    exit /b 1
)
echo [+] Successfully built benchmark.exe
goto end

REM =============================================================================
:run_perf
call :build_perf
if errorlevel 1 exit /b 1

echo [*] Running performance benchmark...
for /f %%a in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd_HHmmss"') do set TS=%%a
set RESULT_FILE=%OUTPUT_DIR%\results_!TS!.csv

%OUTPUT_DIR%\benchmark.exe > "%RESULT_FILE%" 2>&1
if errorlevel 1 (
    echo [ERROR] Benchmark execution failed!
    exit /b 1
)
echo [+] Results saved to: %RESULT_FILE%
type "%RESULT_FILE%"
goto end

REM =============================================================================
:clean_perf
echo [*] Cleaning performance build artifacts...
if exist "%OUTPUT_DIR%\benchmark.exe" del /q "%OUTPUT_DIR%\benchmark.exe"
if exist "%OUTPUT_DIR%\benchmark.ilk" del /q "%OUTPUT_DIR%\benchmark.ilk"
if exist "%OUTPUT_DIR%\benchmark.pdb" del /q "%OUTPUT_DIR%\benchmark.pdb"
echo [+] Cleaned.
goto end

REM =============================================================================
:distclean_perf
call :clean_perf
for /f %%f in ('dir /b %OUTPUT_DIR%\results_*.* 2^>nul') do (
    del "%OUTPUT_DIR%\%%f"
    echo   Removed %%f
)
echo [+] Cleaned all artifacts and results.
goto end

:end
endlocal

@echo off
REM Batch file for running RocketDB performance benchmarks on Windows

setlocal enabledelayedexpansion

cd /d "%~dp0"

echo.
echo =========================================
echo  RocketDB Performance Benchmark Builder
echo =========================================
echo.

REM Check for compiler
where gcc >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] GCC not found. Please install MinGW or GCC.
    exit /b 1
)

REM Compiler setup
set CC=gcc
set CFLAGS=-O2 -Wall -Wextra -std=c99
set INC=-I../../ -I../sim

REM Source files
set SRCS=scenarios.c ..\sim\sim_flash.c
set TARGET=benchmark.exe

echo [*] Compiling %TARGET%...
%CC% %CFLAGS% %INC% -o %TARGET% %SRCS%

if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] Compilation failed!
    exit /b 1
)

echo [+] Successfully built %TARGET%!
echo.

echo [*] Running performance benchmark...
echo.

REM Generate timestamp for results file
for /f "tokens=2-4 delims=/ " %%a in ('date /t') do (set mydate=%%c%%a%%b)
for /f "tokens=1-2 delims=/:" %%a in ('time /t') do (set mytime=%%a%%b)

set RESULT_FILE=results_%mydate%_%mytime%.txt

echo %TARGET% | tee %RESULT_FILE%
%TARGET% >> %RESULT_FILE% 2>&1

if %ERRORLEVEL% EQU 0 (
    echo.
    echo [+] Benchmark completed successfully!
    echo [+] Results saved to: %RESULT_FILE%
) else (
    echo [ERROR] Benchmark execution failed!
    exit /b 1
)

exit /b 0

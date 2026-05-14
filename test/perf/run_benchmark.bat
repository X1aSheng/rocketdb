@echo off
REM Batch file for running RocketDB performance benchmarks on Windows

setlocal enabledelayedexpansion

cd /d "%~dp0..\.."

echo.
echo =========================================
echo  RocketDB Performance Benchmark Builder
echo =========================================
echo.

call build\build_perf.bat run
exit /b %ERRORLEVEL%

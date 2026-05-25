@echo off
REM =============================================================================
REM RocketDB - Main Build Entry Point
REM =============================================================================
REM Purpose: One-click build/run all tests or module-specific tests
REM Usage:   build.bat [kvdb|tsdb|all|help] [build|test|clean]
REM =============================================================================

setlocal enabledelayedexpansion

REM cd to project root (script is in build\)
cd /d "%~dp0.."

set MODULE=%1
set ACTION=%2

if "%MODULE%"=="" set MODULE=all
if "%ACTION%"=="" set ACTION=test

if "%MODULE%"=="help" goto show_help
if "%MODULE%"=="kvdb" goto kvdb
if "%MODULE%"=="tsdb" goto tsdb
if "%MODULE%"=="all" goto all
if "%MODULE%"=="perf" goto perf
goto unknown

REM =============================================================================
:kvdb
echo.
echo ========================================
echo   RocketDB KVDB Tests
echo ========================================
echo.
call build\run_suite.bat kvdb %ACTION%
exit /b %ERRORLEVEL%

REM =============================================================================
:tsdb
echo.
echo ========================================
echo   RocketDB TSDB Tests
echo ========================================
echo.
call build\run_suite.bat tsdb %ACTION%
exit /b %ERRORLEVEL%

REM =============================================================================
:all
echo.
echo ========================================
echo   RocketDB Full Test Suite
echo ========================================
echo.
call build\run_all_tests.bat %ACTION%
exit /b %ERRORLEVEL%

REM =============================================================================
:perf
echo.
echo ========================================
echo   RocketDB Performance Benchmarks
echo ========================================
echo.
call build\build_perf.bat %ACTION%
exit /b %ERRORLEVEL%

REM =============================================================================
:show_help
echo.
echo ========================================
echo   RocketDB Build System Help
echo ========================================
echo.
echo Usage:
echo   build.bat [module] [action]
echo.
echo Modules:
echo   kvdb        - KVDB test suites (3 tests)
echo   tsdb        - TSDB test suites (2 tests)
echo   all         - All test suites (8 tests, default)
echo   perf        - Performance benchmarks
echo   help        - Show this help
echo.
echo Actions:
echo   test        - Compile and run tests (default)
echo   build       - Compile only
echo   clean       - Remove build artifacts
echo.
echo Examples:
echo   build.bat                  # Build and run all tests
echo   build.bat kvdb test        # Build and run KVDB tests
echo   build.bat tsdb build       # Compile TSDB tests only
echo   build.bat all clean        # Clean all outputs
echo   build.bat perf run         # Run performance benchmarks
echo.
goto end

REM =============================================================================
:unknown
echo.
echo [ERROR] Unknown module: %MODULE%
echo Run 'build.bat help' for usage.
exit /b 1

:end
endlocal

@echo off
REM =============================================================================
REM RocketDB - TSDB Test Runner (convenience wrapper)
REM =============================================================================
REM Usage: build_tsdb_all.bat [build|test|clean|help]
REM Delegates to run_suite.bat for actual build/run logic.
REM =============================================================================

cd /d "%~dp0.."

if "%1"=="help" goto show_help
call build\run_suite.bat tsdb %1
exit /b %ERRORLEVEL%

:show_help
echo.
echo ========================================
echo   RocketDB TSDB Test Runner
echo ========================================
echo.
echo Usage: build_tsdb_all.bat [test^|build^|clean^|help]
echo.
echo   test   - Compile and run TSDB tests (default)
echo   build  - Compile TSDB tests only
echo   clean  - Remove TSDB build artifacts
echo   help   - Show this help
echo.
echo TSDB test suites (2):
echo   test_tsdb_basic   - TSDB append/query, epoch switch, recount, write_gran, max_boundaries
echo   test_tsdb_stress  - TSDB rotation stress, append fail, CRC corruption, degraded epoch recovery
echo.
goto end

:end
endlocal

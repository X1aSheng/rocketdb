@echo off
REM =============================================================================
REM RocketDB - Unified Test Runner
REM =============================================================================
REM Usage: build_unified.bat [build|test|clean|help]
REM Runs all 8 test suites in one batch.
REM =============================================================================

cd /d "%~dp0.."

if "%1"=="help" goto show_help
call build\run_all_tests.bat %1
exit /b %ERRORLEVEL%

:show_help
echo.
echo ========================================
echo   RocketDB Unified Test Runner
echo ========================================
echo.
echo Usage: build_unified.bat [test^|build^|clean^|help]
echo.
echo   test   - Build and run all 8 test suites plus rdbdump verification (default)
echo   build  - Build all test suites only
echo   clean  - Remove all build artifacts
echo   help   - Show this help
echo.
echo Output: tests\out
echo.
echo Test suites (8 total):
echo   test_kvdb_basic        KVDB basic functionality
echo   test_kvdb_stress       KVDB stress + GC + power-loss
echo   test_kvdb_cache        KVDB key cache + TSDB safety fixes
echo   test_tsdb_basic        TSDB append/query/epoch
echo   test_tsdb_stress       TSDB rotation + CRC + fault
echo   test_integration       KVDB+TSDB combined + mixed workload
echo   test_fault_injection   Fault injection demo
echo   test_example           Tutorial example
echo.
goto end

:end
endlocal

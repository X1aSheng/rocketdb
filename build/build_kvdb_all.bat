@echo off
REM =============================================================================
REM RocketDB - KVDB Test Runner (convenience wrapper)
REM =============================================================================
REM Usage: build_kvdb_all.bat [build|test|clean|help]
REM Delegates to run_suite.bat for actual build/run logic.
REM =============================================================================

cd /d "%~dp0.."

if "%1"=="help" goto show_help
call build\run_suite.bat kvdb %1
exit /b %ERRORLEVEL%

:show_help
echo.
echo ========================================
echo   RocketDB KVDB Test Runner
echo ========================================
echo.
echo Usage: build_kvdb_all.bat [test^|build^|clean^|help]
echo.
echo   test   - Compile and run KVDB tests (default)
echo   build  - Compile KVDB tests only
echo   clean  - Remove KVDB build artifacts
echo   help   - Show this help
echo.
echo KVDB test suites (2):
echo   test_kvdb_basic   - KVDB set/get/update/delete, write_gran, seq_wrap, mixed_len, corrupt, capacity
echo   test_kvdb_stress  - KVDB GC stress, iterator under GC, power-loss, corrupt sector recovery
echo.
goto end

:end
endlocal

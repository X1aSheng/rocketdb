# RocketDB Project Review - 260517-160748

## Review Scope

- Reviewed repository file list: 86 tracked project files, 26,853 lines.
- Reviewed core engine: `src/rocketdb.h`, `src/rocketdb_kvdb.c`, `src/rocketdb_tsdb.c`.
- Reviewed HAL/template and W25QXX guidance: `interface/`, `docs/HAL_REFERENCE.md`, `docs/W25QXX_GUIDE.md`.
- Reviewed examples, simulation framework, functional tests, performance benchmarks, CMake, Makefile, and Windows batch scripts.
- No `tests/` or `scripts/` directory exists in this repository. The equivalent project directories are `test/` and `build/`.

## Baseline Test Runs

| Command | Result |
|---------|--------|
| `build\run_all_tests.bat test` | PASS, 7/7 suites |
| `build\build.bat all test` | PASS, 7/7 suites |
| `cmake -S . -B .cmake-review -G Ninja ... -DCMAKE_RC_COMPILER=...` + `ctest` | PASS, 9/9 tests |
| `build\build_perf.bat run` | PASS, benchmark CSV generated |

Observed baseline issue: Windows Clang CMake configure fails without an explicit resource compiler (`CMAKE_RC_COMPILER`), even though `llvm-rc.exe` is present beside LLVM.

## Confirmed Defects And Plan

### P1 - TSDB Large Payload HAL Write Is Not SPI NOR Friendly

- Evidence: added `ts_large_payload_bounded_program_chunks` to reject TSDB HAL writes larger than `RDB_STACK_BUF_SIZE`; pre-fix `rdb_tsdb_append()` failed for a 600-byte payload.
- Root cause: the TSDB large-record path wrote the whole data payload in a single HAL call, unlike KVDB's chunked large-value path.
- Risk: W25QXX-style drivers must split page programs and small embedded HALs often allocate small transfer buffers; one large write violates the documented stack-buffer behavior.
- Fix plan: stream TSDB data plus padding through bounded chunks, preserving `write_gran` alignment and checking every write result.
- Verification: `build\run_suite.bat tsdb test`, then full test matrix.

### P2 - Windows Clang CMake Requires Manual Resource Compiler

- Evidence: `cmake -S . -B .cmake-review -G Ninja -DCMAKE_C_COMPILER=...` failed with `No CMAKE_RC_COMPILER could be found`.
- Root cause: CMake's Windows Clang platform setup needs `rc`/`llvm-rc`, but the project does not auto-discover the LLVM resource compiler.
- Fix plan: detect `llvm-rc` before `project()` on Windows hosts when `CMAKE_RC_COMPILER` is not already set.
- Verification: configure a fresh CMake build dir without passing `CMAKE_RC_COMPILER`, build, and run CTest.

### P3 - Documentation Is Stale Or Misleading

- Evidence:
  - `README.md` links `docs/rocketdb%20design.md`, which does not exist; current design manual is `docs/Architecture.md`.
  - `README.md` and `docs/BUILD_SCRIPTS.md` refer to `project/build/`; actual scripts live in `build/`.
  - `docs/W25QXX_GUIDE.md` says RocketDB write calls are always `<=64` and therefore need no W25QXX page split; HAL examples should still split at 256-byte page boundaries.
  - TSDB write granularity test text says 1/2/4/8B, while implementation currently exercises supported 1/2B only.
- Fix plan: update README, build script docs, W25QXX/HAL guidance, test plan, and simulator README to match current behavior and test results.
- Verification: full test matrix plus link/path grep for stale names.

### P4 - Local CMake Build Directory Is Not Ignored

- Evidence: `.cmake-review/` appears as untracked generated files after CMake validation.
- Fix plan: add `.cmake-*/` to `.gitignore` or clean generated output before final status.
- Verification: `git status --short --untracked-files=all`.

## Completion Criteria

1. Each confirmed defect has a targeted regression test or command-line reproduction.
2. Each fix is verified by the targeted test first, then full project automation.
3. Each completed fix is committed separately.
4. Documentation is synchronized after code fixes and verified by grep/build/test.

## Completion Record

| Item | Status | Commit | Verification |
|------|--------|--------|--------------|
| P1 TSDB large payload HAL write | Done | `c94e534` | Pre-fix `test_tsdb_basic` failed on `ts_large_payload_bounded_program_chunks`; post-fix `build\run_suite.bat tsdb test` passed 2/2 |
| P2 Windows Clang CMake RC detection | Done | `05f54ec` | Fresh configure without `CMAKE_RC_COMPILER`, build, and `ctest` passed 9/9 |
| P3 Documentation sync | Done | `60a0463` | Stale-path grep clean for active docs; `build\run_all_tests.bat test` passed 7/7 |
| P4 Ignore local CMake review builds | Done | `77a49bb` | `git status --short --untracked-files=all` no longer reports `.cmake-*` generated files |

Final verification is run after this record update so the review artifact reflects the completed state.

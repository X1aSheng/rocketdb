# Contributing to RocketDB

Thanks for your interest in contributing! Here's how to get started.

## How to Contribute

### Reporting Bugs

1. Check the [docs/architecture/ARCHITECTURE.md](docs/architecture/ARCHITECTURE.md) for design overview
2. Search existing issues to avoid duplicates
3. Include: RocketDB version, compiler version, target platform, minimal reproduction steps

### Feature Requests

1. Check the [design manual](docs/architecture/ARCHITECTURE.md) to understand existing architecture
2. Open an issue with: use case, proposed API, impact on existing features

### Pull Requests

1. Fork the repository
2. Create a feature branch (`git checkout -b feature/my-feature`)
3. Make your changes following our coding style:
   - C99 standard
   - 4-space indentation
   - Doxygen-style function comments
   - Zero dynamic memory allocation
4. Run the test suite before submitting:
   ```bash
   cd build && run_all_tests.bat
   ```
5. Submit a PR with a clear description

## Coding Standards

- **Language**: C99 (no C11/C17 features)
- **Allocation**: Zero dynamic memory — all buffers caller-provided
- **Flash safety**: All writes follow NOR 1→0 bit-flip semantics
- **Comments**: Doxygen `/** ... */` format for all public functions
- **Error handling**: Return `rdb_err_t` error codes, never crash

## Directory Structure

```
rocketdb/
├── src/           ← Core engine source
├── interface/     ← HAL porting template
├── examples/      ← Usage examples
├── tests/         ← Test suites & benchmarks
│   ├── sim/       ← Flash simulator + test cases (8 suites)
│   ├── perf/      ← Performance benchmarks
│   └── out/       ← Build/test output (generated)
├── docs/          ← Documentation (architecture, guides, reports)
├── build/         ← Windows build/test scripts
├── tools/rdbdump/ ← Offline Flash dump inspector
└── zephyr/        ← Zephyr OS port layer
```

## Getting Help

- Read the [README](README.md) for quick start
- Review [docs/architecture/ARCHITECTURE.md](docs/architecture/ARCHITECTURE.md) for architecture details
- See [docs/architecture/TEST_PLAN.md](docs/architecture/TEST_PLAN.md) for test coverage

## License

By contributing, you agree that your contributions will be licensed under the MIT License.

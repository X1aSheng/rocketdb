# Contributing to RocketDB

Thanks for your interest in contributing! Here's how to get started.

## Code of Conduct

Please read [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md) before contributing.

## How to Contribute

### Reporting Bugs

1. Check the [Troubleshooting Guide](docs/TROUBLESHOOTING.md) first
2. Search existing issues to avoid duplicates
3. Include: RocketDB version, compiler version, target platform, minimal reproduction steps

### Feature Requests

1. Check the [design manual](docs/rocketdb%20design.md) to understand existing architecture
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
├── test/          ← Test suites
│   ├── sim/       ← Flash simulator + test cases
│   └── perf/      ← Performance benchmarks
├── docs/          ← Documentation
└── project/       ← Platform-specific builds
```

## Getting Help

- Read the [README](README.md) for quick start
- Review [rocketdb design.md](docs/rocketdb%20design.md) for architecture details
- See [docs/architecture/TEST_PLAN.md](docs/architecture/TEST_PLAN.md) for test coverage

## License

By contributing, you agree that your contributions will be licensed under the MIT License.

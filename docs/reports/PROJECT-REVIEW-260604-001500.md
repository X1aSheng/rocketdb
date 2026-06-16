# RocketDB Zephyr Port Review — 260604

## Scope

Focused review of `zephyr/` port layer — Kconfig, CMake, port adapter.

## Findings

### F1 — New compile-time macros not exposed in Zephyr Kconfig

**Severity**: Medium — Zephyr users cannot configure bloom filter, GC weights,
flash page size, or KV cache through the standard menuconfig system.

**Files**: `zephyr/Kconfig`, `zephyr/CMakeLists.txt`

The following macros were added as generic `#ifndef` in `rocketdb.h` but
never wired through the Zephyr build:
- `RDB_BLOOM_BITS`
- `RDB_FLASH_PAGE_SIZE`
- `RDB_KV_CACHE_SIZE`
- `RDB_GC_W_GARBAGE`, `RDB_GC_W_WEAR`, `RDB_GC_W_CAPACITY`

**Fix**: Added Kconfig symbols + CMake `zephyr_compile_definitions` entries
for all six macros.  Committed in `41a86a1`.

### F2 — Zephyr device_is_ready() deprecation (advisory)

Zephyr ≥ 4.0 deprecates `device_is_ready()` in favour of `z_device_is_ready()`.
The current code uses `device_is_ready()` which compiles on both 3.7 LTS and
4.x (with a deprecation warning on 4.x).  A future update should add a version
check:

```c
#if defined(CONFIG_ZEPHYR_VERSION_4_0_PLUS)
    if (!z_device_is_ready(c->dev))
#else
    if (!device_is_ready(c->dev))
#endif
```

Deferred — no functional impact.

### F3 — Bloom filter macros hardcoded for 256 bits (advisory)

The `RDB_BLOOM_SET` / `RDB_BLOOM_MAYBE` macros hardcode byte-index shifts
(`0x1F`, `>>5`) that assume a 256-bit (32-byte) bitmap.  Configuring
`RDB_BLOOM_BITS=512` would leave the upper 32 bytes unused.  A compile-time
derivation from `RDB_BLOOM_BITS` would resolve this but is non-trivial in C99.

**Mitigation**: Kconfig range restricted to 0 or 256 by help-text guidance.
Users who need other sizes define the macro directly.

## Completion

| ID | Fix | Commit | Status |
|----|-----|--------|--------|
| F1 | Kconfig + CMake for 6 new macros | `41a86a1` | ✅ Pushed |
| F2 | Advisory — deferred | — | 📝 Noted |
| F3 | Advisory — Kconfig restricted | — | 📝 Noted |

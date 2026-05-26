# rdbdump

`rdbdump` parses a raw RocketDB Flash partition image on a PC/server. It is intended for field diagnostics after an embedded device reads out a full Flash partition or for checking simulator dumps after the test suite runs.

## Commands

```bash
python tools/rdbdump/rdbdump.py inspect --manifest tests/out/rdbdump_kvdb.json
python tools/rdbdump/rdbdump.py verify --strict --manifest tests/out/rdbdump_kvdb.json
python tools/rdbdump/rdbdump.py export --manifest tests/out/rdbdump_kvdb.json --out tests/out/rdbdump_export/260526-094809/kvdb
```

Common options:

- `--input`: raw Flash partition image.
- `--manifest`: JSON file with `kind`, `sector_size`, `write_gran`, `base_addr`, `total_size`, and optional relative `input`.
- `--kind {auto,kvdb,tsdb}`: force or auto-detect engine type.
- `--sector-size`, `--write-gran`, `--base-addr`, `--total-size`: geometry overrides.

## Export Layout

`export` writes layered datasets:

- `observable_dataset.json/csv`: every observable sector header, record, erased tail, and scan anomaly.
- `valid_dataset.json/csv`: logical valid data only. KVDB contains current key-values; TSDB contains valid CRC-checked samples.
- `summary.json`: counts and geometry summary.
- `anomalies.json`: CRC/header/scan problems.
- `kv_current.json`, `kv_records.csv`, `ts_records.csv`: compatibility/convenience views.

The integrated test flow writes timestamped output below `tests/out/rdbdump_export/<YYMMDD-HHMMSS>/`.

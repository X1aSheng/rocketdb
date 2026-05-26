#!/usr/bin/env python3
"""Offline RocketDB Flash dump inspector."""

from __future__ import annotations

import argparse
import csv
import json
import os
import struct
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any


KV_SECTOR_MAGIC = 0x4B564442
TS_SECTOR_MAGIC = 0x54534442
KV_RECORD_MAGIC = 0xA5
TS_RECORD_MAGIC = 0xB6
STATE_WRITING = 0xFF
STATE_VALID = 0xFE
STATE_DEAD = 0xFC


def align_up(value: int, align: int) -> int:
    return (value + align - 1) & ~(align - 1)


def crc16_modbus(data: bytes, seed: int = 0xFFFF) -> int:
    crc = seed
    for b in data:
        crc ^= b
        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ 0xA001
            else:
                crc >>= 1
            crc &= 0xFFFF
    return crc


def hex_prefix(data: bytes, limit: int = 64) -> str:
    return data[:limit].hex()


def is_erased(data: bytes) -> bool:
    return all(b == 0xFF for b in data)


def read_manifest(path: str | None) -> dict[str, Any]:
    if not path:
        return {}
    with open(path, "r", encoding="utf-8") as fp:
        return json.load(fp)


def option(args: argparse.Namespace, manifest: dict[str, Any], name: str, default: Any) -> Any:
    value = getattr(args, name)
    if value is not None:
        return value
    return manifest.get(name, default)


@dataclass
class DumpConfig:
    input_path: Path
    kind: str
    sector_size: int
    write_gran: int
    base_addr: int
    total_size: int

    @property
    def gran_bytes(self) -> int:
        return 1 << self.write_gran


def make_config(args: argparse.Namespace) -> DumpConfig:
    manifest = read_manifest(args.manifest)
    input_value = args.input if args.input is not None else (manifest.get("input") or manifest.get("input_path", ""))
    input_path = Path(input_value)
    if not input_path:
        raise SystemExit("input path is required")
    if args.input is None and args.manifest and not input_path.is_absolute():
        input_path = Path(args.manifest).resolve().parent / input_path
    size = input_path.stat().st_size
    total_size = int(option(args, manifest, "total_size", size))
    return DumpConfig(
        input_path=input_path,
        kind=str(option(args, manifest, "kind", "auto")).lower(),
        sector_size=int(option(args, manifest, "sector_size", 4096)),
        write_gran=int(option(args, manifest, "write_gran", 0)),
        base_addr=int(option(args, manifest, "base_addr", 0)),
        total_size=total_size,
    )


def detect_kind(raw: bytes, cfg: DumpConfig) -> str:
    if cfg.kind != "auto":
        return cfg.kind
    kv = ts = 0
    for off in range(0, len(raw), cfg.sector_size):
        magic = struct.unpack_from("<I", raw, off)[0]
        kv += int(magic == KV_SECTOR_MAGIC)
        ts += int(magic == TS_SECTOR_MAGIC)
    if kv == 0 and ts == 0:
        return "unknown"
    return "kvdb" if kv >= ts else "tsdb"


def add_anomaly(anomalies: list[dict[str, Any]], severity: str, message: str, **extra: Any) -> None:
    item = {"severity": severity, "message": message}
    item.update(extra)
    anomalies.append(item)


def parse_kvdb(raw: bytes, cfg: DumpConfig) -> dict[str, Any]:
    observable: list[dict[str, Any]] = []
    records: list[dict[str, Any]] = []
    anomalies: list[dict[str, Any]] = []
    sector_count = cfg.total_size // cfg.sector_size
    data_start = align_up(16, cfg.gran_bytes)

    for sec in range(sector_count):
        base = sec * cfg.sector_size
        sector = raw[base:base + cfg.sector_size]
        if len(sector) < cfg.sector_size:
            add_anomaly(anomalies, "error", "short sector", sector=sec)
            continue
        magic, version, hdr_crc, erase_cnt, create_seq = struct.unpack_from("<IHHII", sector, 0)
        erased = is_erased(sector)
        hdr_crc_calc = crc16_modbus(sector[:6])
        status = "ERASED" if erased else ("KVDB" if magic == KV_SECTOR_MAGIC else "CORRUPT")
        observable.append({
            "type": "sector_header", "kind": "kvdb", "sector": sec, "offset": base,
            "status": status, "magic": magic, "version": version, "erase_cnt": erase_cnt,
            "create_seq": create_seq, "hdr_crc": hdr_crc, "hdr_crc_calc": hdr_crc_calc,
            "hdr_crc_ok": hdr_crc == hdr_crc_calc, "raw_hex_prefix": hex_prefix(sector[:16]),
        })
        if status == "CORRUPT":
            add_anomaly(anomalies, "warn", "invalid KVDB sector header", sector=sec)
            continue
        if erased:
            continue
        off = data_start
        while off + 16 <= cfg.sector_size:
            chunk = sector[off:off + 16]
            if is_erased(sector[off:]):
                observable.append({
                    "type": "erased_tail", "kind": "kvdb", "sector": sec,
                    "offset": base + off, "length": cfg.sector_size - off,
                })
                break
            magic_b, state, klen, _pad0, vlen, key_hash, seq, data_crc, _pad1 = struct.unpack_from("<BBBBHHIHH", sector, off)
            if magic_b != KV_RECORD_MAGIC or klen > 32:
                observable.append({
                    "type": "scan_error", "kind": "kvdb", "sector": sec,
                    "offset": base + off, "reason": "invalid record header",
                    "raw_hex_prefix": hex_prefix(chunk),
                })
                add_anomaly(anomalies, "warn", "invalid KVDB record header", sector=sec, offset=base + off)
                off += align_up(16, cfg.gran_bytes)
                continue
            key_off = off + 16
            val_off = key_off + align_up(klen, cfg.gran_bytes)
            next_off = val_off + align_up(vlen, cfg.gran_bytes)
            if next_off > cfg.sector_size:
                add_anomaly(anomalies, "warn", "KVDB record exceeds sector", sector=sec, offset=base + off)
                break
            key = sector[key_off:key_off + klen]
            val = sector[val_off:val_off + vlen]
            crc_calc = crc16_modbus(key + val)
            rec = {
                "type": "record", "kind": "kvdb", "sector": sec, "offset": base + off,
                "state": state, "state_name": state_name(state), "key_len": klen,
                "value_len": vlen, "key_hash": key_hash, "seq": seq, "data_crc": data_crc,
                "data_crc_calc": crc_calc, "crc_ok": data_crc == crc_calc,
                "key": key.decode("utf-8", "replace"), "key_hex": key.hex(),
                "value_hex": val.hex(), "record_size": next_off - off,
                "raw_hex_prefix": hex_prefix(sector[off:next_off]),
            }
            observable.append(rec)
            records.append(rec)
            if state == STATE_VALID and data_crc != crc_calc:
                add_anomaly(anomalies, "error", "KVDB VALID record CRC mismatch", sector=sec, offset=base + off)
            off = next_off

    current: dict[str, dict[str, Any]] = {}
    for rec in records:
        if rec["state"] == STATE_VALID and rec["crc_ok"]:
            key = rec["key"]
            if key not in current or rec["seq"] > current[key]["seq"]:
                current[key] = rec

    valid = [{
        "key": key, "value_hex": rec["value_hex"], "value_len": rec["value_len"],
        "seq": rec["seq"], "sector": rec["sector"], "offset": rec["offset"],
    } for key, rec in sorted(current.items())]
    return {
        "summary": {"kind": "kvdb", "sectors": sector_count, "records": len(records), "valid_current": len(valid), "anomalies": len(anomalies)},
        "observable": observable,
        "valid": valid,
        "records": records,
        "anomalies": anomalies,
    }


def state_name(state: int) -> str:
    if state == STATE_WRITING:
        return "WRITING"
    if state == STATE_VALID:
        return "VALID"
    if state == STATE_DEAD:
        return "DEAD"
    return "UNKNOWN"


def parse_tsdb(raw: bytes, cfg: DumpConfig) -> dict[str, Any]:
    observable: list[dict[str, Any]] = []
    records: list[dict[str, Any]] = []
    anomalies: list[dict[str, Any]] = []
    sector_count = cfg.total_size // cfg.sector_size
    data_start = align_up(20, cfg.gran_bytes)

    for sec in range(sector_count):
        base = sec * cfg.sector_size
        sector = raw[base:base + cfg.sector_size]
        magic, erase_cnt, time_base, seq, count, end_off, hdr_crc = struct.unpack_from("<IIIHHHH", sector, 0)
        hdr_crc_calc = crc16_modbus(sector[:18])
        erased = is_erased(sector)
        if erased:
            status = "ERASED"
        elif magic != TS_SECTOR_MAGIC:
            status = "CORRUPT"
        elif hdr_crc == hdr_crc_calc and count != 0xFFFF and end_off != 0xFFFF:
            status = "SEALED"
        else:
            status = "ACTIVE" if time_base != 0xFFFFFFFF else "ACTIVE_EMPTY"
        observable.append({
            "type": "sector_header", "kind": "tsdb", "sector": sec, "offset": base,
            "status": status, "magic": magic, "erase_cnt": erase_cnt, "time_base": time_base,
            "seq": seq, "count": count, "end_off": end_off, "hdr_crc": hdr_crc,
            "hdr_crc_calc": hdr_crc_calc, "hdr_crc_ok": hdr_crc == hdr_crc_calc,
            "raw_hex_prefix": hex_prefix(sector[:20]),
        })
        if status == "CORRUPT":
            add_anomaly(anomalies, "warn", "invalid TSDB sector header", sector=sec)
            continue
        if erased:
            continue
        off = data_start
        scan_limit = end_off if status == "SEALED" and end_off != 0xFFFF else cfg.sector_size
        while off + 12 <= min(scan_limit, cfg.sector_size):
            if is_erased(sector[off:]):
                observable.append({
                    "type": "erased_tail", "kind": "tsdb", "sector": sec,
                    "offset": base + off, "length": cfg.sector_size - off,
                })
                break
            magic_b, state, dlen, time_delta, data_crc, _pad = struct.unpack_from("<BBHIHH", sector, off)
            if magic_b != TS_RECORD_MAGIC:
                observable.append({
                    "type": "scan_error", "kind": "tsdb", "sector": sec,
                    "offset": base + off, "reason": "invalid record header",
                    "raw_hex_prefix": hex_prefix(sector[off:off + 12]),
                })
                add_anomaly(anomalies, "warn", "invalid TSDB record header", sector=sec, offset=base + off)
                off += align_up(12, cfg.gran_bytes)
                continue
            data_off = off + 12
            next_off = data_off + align_up(dlen, cfg.gran_bytes)
            if next_off > cfg.sector_size:
                add_anomaly(anomalies, "warn", "TSDB record exceeds sector", sector=sec, offset=base + off)
                break
            data = sector[data_off:data_off + dlen]
            crc_calc = crc16_modbus(data)
            abs_time = None if time_base == 0xFFFFFFFF else (time_base + time_delta) & 0xFFFFFFFF
            rec = {
                "type": "record", "kind": "tsdb", "sector": sec, "offset": base + off,
                "state": state, "state_name": state_name(state), "data_len": dlen,
                "time_base": time_base, "time_delta": time_delta, "time": abs_time,
                "data_crc": data_crc, "data_crc_calc": crc_calc, "crc_ok": data_crc == crc_calc,
                "data_hex": data.hex(), "record_size": next_off - off,
                "raw_hex_prefix": hex_prefix(sector[off:next_off]),
            }
            observable.append(rec)
            records.append(rec)
            if state == STATE_VALID and data_crc != crc_calc:
                add_anomaly(anomalies, "error", "TSDB VALID record CRC mismatch", sector=sec, offset=base + off)
            off = next_off

    valid = [rec for rec in records if rec["state"] == STATE_VALID and rec["crc_ok"]]
    valid.sort(key=lambda rec: (rec["time"] if rec["time"] is not None else -1, rec["sector"], rec["offset"]))
    return {
        "summary": {"kind": "tsdb", "sectors": sector_count, "records": len(records), "valid_records": len(valid), "anomalies": len(anomalies)},
        "observable": observable,
        "valid": valid,
        "records": records,
        "anomalies": anomalies,
    }


def write_json(path: Path, data: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w", encoding="utf-8") as fp:
        json.dump(data, fp, indent=2, ensure_ascii=False)
        fp.write("\n")


def write_csv(path: Path, rows: list[dict[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    keys: list[str] = []
    for row in rows:
        for key in row:
            if key not in keys:
                keys.append(key)
    with open(path, "w", newline="", encoding="utf-8") as fp:
        writer = csv.DictWriter(fp, fieldnames=keys)
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


def run_parse(args: argparse.Namespace) -> tuple[DumpConfig, dict[str, Any]]:
    cfg = make_config(args)
    raw = cfg.input_path.read_bytes()[:cfg.total_size]
    kind = detect_kind(raw, cfg)
    if kind == "kvdb":
        return cfg, parse_kvdb(raw, cfg)
    if kind == "tsdb":
        return cfg, parse_tsdb(raw, cfg)
    raise SystemExit("unable to detect dump kind")


def cmd_inspect(args: argparse.Namespace) -> int:
    _cfg, result = run_parse(args)
    print(json.dumps(result["summary"], indent=2))
    return 0


def cmd_verify(args: argparse.Namespace) -> int:
    _cfg, result = run_parse(args)
    summary = result["summary"]
    print(json.dumps(summary, indent=2))
    errors = [a for a in result["anomalies"] if a["severity"] == "error"]
    if errors:
        print(f"verify failed: {len(errors)} error anomaly/anomalies", file=sys.stderr)
        return 1
    if getattr(args, "strict", False) and result["anomalies"]:
        print(f"strict verify failed: {len(result['anomalies'])} anomaly/anomalies", file=sys.stderr)
        return 1
    return 0


def cmd_export(args: argparse.Namespace) -> int:
    _cfg, result = run_parse(args)
    out = Path(args.out)
    write_json(out / "summary.json", result["summary"])
    write_json(out / "anomalies.json", result["anomalies"])
    write_json(out / "observable_dataset.json", result["observable"])
    write_csv(out / "observable_dataset.csv", result["observable"])
    write_json(out / "valid_dataset.json", result["valid"])
    write_csv(out / "valid_dataset.csv", result["valid"])
    if result["summary"]["kind"] == "kvdb":
        write_json(out / "kv_current.json", result["valid"])
        write_csv(out / "kv_records.csv", result["records"])
    else:
        write_csv(out / "ts_records.csv", result["records"])
    print(f"exported {result['summary']['kind']} dataset to {out}")
    return 0


def add_common(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--manifest")
    parser.add_argument("--kind", choices=["auto", "kvdb", "tsdb"])
    parser.add_argument("--sector-size", type=int, dest="sector_size")
    parser.add_argument("--write-gran", type=int, dest="write_gran")
    parser.add_argument("--base-addr", type=lambda x: int(x, 0), dest="base_addr")
    parser.add_argument("--total-size", type=lambda x: int(x, 0), dest="total_size")
    parser.add_argument("--input")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="rdbdump", description="Inspect and export RocketDB raw Flash partition dumps")
    sub = parser.add_subparsers(dest="cmd", required=True)
    p_inspect = sub.add_parser("inspect")
    add_common(p_inspect)
    p_inspect.set_defaults(func=cmd_inspect)
    p_verify = sub.add_parser("verify")
    add_common(p_verify)
    p_verify.add_argument("--strict", action="store_true")
    p_verify.set_defaults(func=cmd_verify)
    p_export = sub.add_parser("export")
    add_common(p_export)
    p_export.add_argument("--out", required=True)
    p_export.set_defaults(func=cmd_export)
    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())

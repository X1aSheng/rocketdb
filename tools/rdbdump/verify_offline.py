#!/usr/bin/env python3
"""Generate sim dumps, then verify/export them through rdbdump."""

from __future__ import annotations

import argparse
import datetime as dt
import subprocess
import sys
from pathlib import Path


def run(cmd: list[str], cwd: Path) -> None:
    print("+ " + " ".join(cmd))
    subprocess.run(cmd, cwd=cwd, check=True)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--generator", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--rdbdump", required=True)
    parser.add_argument("--repo-root", required=True)
    args = parser.parse_args(argv)

    repo = Path(args.repo_root)
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    stamp = dt.datetime.now().strftime("%y%m%d-%H%M%S")
    export_root = out / "rdbdump_export" / stamp

    run([args.generator, str(out)], repo)
    for kind in ("kvdb", "tsdb"):
        manifest = out / f"rdbdump_{kind}.json"
        image = out / f"rdbdump_{kind}.bin"
        run([sys.executable, args.rdbdump, "verify", "--strict", "--manifest", str(manifest), "--input", str(image)], repo)
        run([sys.executable, args.rdbdump, "export", "--manifest", str(manifest), "--input", str(image), "--out", str(export_root / kind)], repo)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

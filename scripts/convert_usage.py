#!/usr/bin/env python3
"""Convert ~/.claude-usage/ JSONL history to CSV files for the SD card.

Output: one CSV per month in output_dir/ccusage/usage_YYYY-MM.csv
Format: timestamp,five_hour,seven_day  (matching usage_store on device)

usage_progress from JSONL maps to seven_day; five_hour is set to 0
(not available in historical data).
"""

import json
import sys
from collections import defaultdict
from datetime import datetime, timezone
from pathlib import Path

USAGE_DIR = Path.home() / ".claude-usage"
OUTPUT_DIR = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("sd_output")


def main():
    if not USAGE_DIR.exists():
        print(f"Error: {USAGE_DIR} not found")
        sys.exit(1)

    # Group rows by YYYY-MM
    monthly: dict[str, list[tuple[int, float]]] = defaultdict(list)

    for jsonl_file in sorted(USAGE_DIR.glob("*.jsonl")):
        with open(jsonl_file) as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                try:
                    rec = json.loads(line)
                except json.JSONDecodeError:
                    continue
                ts_str = rec["timestamp"]
                usage = rec.get("usage_progress", 0.0)

                dt = datetime.fromisoformat(ts_str)
                epoch = int(dt.timestamp())
                month_key = dt.astimezone(timezone.utc).strftime("%Y-%m")

                monthly[month_key].append((epoch, usage))

    out_dir = OUTPUT_DIR / "ccusage"
    out_dir.mkdir(parents=True, exist_ok=True)

    total = 0
    for month_key in sorted(monthly):
        rows = sorted(monthly[month_key])
        path = out_dir / f"usage_{month_key}.csv"
        with open(path, "w") as f:
            f.write("timestamp,five_hour,seven_day\n")
            for epoch, usage in rows:
                f.write(f"{epoch},0.0,{usage:.1f}\n")
        total += len(rows)
        print(f"  {path}  ({len(rows)} rows)")

    print(f"\nTotal: {total} data points across {len(monthly)} months")
    print(f"Copy {out_dir}/ to SD card root as /ccusage/")


if __name__ == "__main__":
    main()

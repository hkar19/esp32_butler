#!/usr/bin/env python3
"""Turns butler.env section 2 into an NVS image: provision-out/nvs.csv -> provision-out/nvs.bin.

Run by ./bt.sh provision with IDF's Python (it needs $IDF_PATH for nvs_partition_gen.py).
"""
import argparse
import csv
import os
import subprocess
import sys

# key in NVS, --option, min, max
TUNABLES = [
    ("led_bright", "led_brightness_pct", 1, 100),
    ("work_stale", "working_stale_s", 60, 86400),
    ("session_ttl", "session_ttl_s", 300, 604800),
]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for _, opt, _, _ in TUNABLES:
        p.add_argument("--" + opt.replace("_", "-"), type=int)
    p.add_argument("--size", default="0x6000", help="size of the nvs partition in partitions.csv")
    p.add_argument("--out", default="provision-out")
    a = p.parse_args()

    rows = [("bt", "namespace", "", "")]
    for key, opt, lo, hi in TUNABLES:
        v = getattr(a, opt)
        if v is None:
            continue
        if not lo <= v <= hi:
            sys.exit(f"provision: BT_{opt.upper()}={v} is out of range {lo}-{hi}")
        rows.append((key, "data", "u32", str(v)))

    os.makedirs(a.out, mode=0o700, exist_ok=True)
    csv_path, bin_path = os.path.join(a.out, "nvs.csv"), os.path.join(a.out, "nvs.bin")
    with open(csv_path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(("key", "type", "encoding", "value"))
        w.writerows(rows)
    os.chmod(csv_path, 0o600)
    gen = os.path.join(os.environ["IDF_PATH"], "components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py")
    subprocess.run([sys.executable, gen, "generate", csv_path, bin_path, a.size], check=True,
                   stdout=subprocess.DEVNULL)
    print(f"provision: wrote {bin_path} ({', '.join(r[0] for r in rows[1:]) or 'no settings: all defaults'})")


if __name__ == "__main__":
    main()

"""Zeste hardware runner: the same test ROM on the console and in Zeste, results side by side.

The console part goes through Cement (the FTP app on the 3DS) with baguette-cli. Cement cannot
start titles, so a run has two steps:

  python tools/zeste/hwrun.py send ROM.cia
      Uploads the CIA to sdmc:/cia. Install it from Cement's bottom screen, start it, let it
      finish: it writes its results to a folder of the SD card (results.csv, log.txt).

  python tools/zeste/hwrun.py compare ROM.cia --sd-dir sd:/zeste-sdbench
      Downloads that folder from the console, runs the ROM headless in Zeste (its SD folder is
      emptied first), and compares the two results.csv files row by row. Everything lands in
      build/hwruns/<rom>-<time>/ (console/, zeste/, compare.md).

The ROM contract: it writes <sd-dir>/results.csv (a header line, then rows; text columns name the
row, number columns are measurements) and a log whose last line contains --done-text. The default
Zeste script presses A at frame 60 (what the Zeste test ROMs wait for) and waits for that line;
--script replaces it.
"""

import argparse
import csv
import datetime
import os
import shutil
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
DEFAULT_ZESTE = os.path.join(ROOT, "build", "bin", "Release", "zeste.exe")
DEFAULT_BAGUETTE = os.environ.get("BAGUETTE_CLI", "baguette-cli")


def baguette(args, exe):
    print("baguette-cli " + " ".join(args))
    result = subprocess.run([exe, "--quiet"] + args)
    if result.returncode != 0:
        sys.exit(f"baguette-cli failed with exit code {result.returncode}")


def sd_path(path):
    path = path.replace("\\", "/")
    if path.startswith("sd:"):
        path = path[3:]
    return "/" + path.strip("/")


def is_number(text):
    try:
        float(text)
        return True
    except ValueError:
        return False


def read_rows(path, key_columns=None):
    with open(path, newline="", encoding="utf-8", errors="replace") as f:
        rows = list(csv.DictReader(f))
    if not rows:
        return [], [], {}
    columns = list(rows[0].keys())
    numeric = [c for c in columns if all(is_number(r[c]) for r in rows if r[c] != "")]
    keys = key_columns or [c for c in columns if c not in numeric]
    numeric = [c for c in numeric if c not in keys]
    keyed = {}
    for r in rows:
        key = tuple(r[c] for c in keys)
        n = 2
        base = key
        while key in keyed:  # repeated names get #2, #3...
            key = base + (f"#{n}",)
            n += 1
        keyed[key] = r
    return columns, numeric, keyed


def compare(console_csv, zeste_csv, value_columns, key_columns):
    _, numeric_c, rows_c = read_rows(console_csv, key_columns)
    _, numeric_z, rows_z = read_rows(zeste_csv, key_columns)
    numeric = [c for c in numeric_c if c in numeric_z]
    if value_columns:
        numeric = [c for c in value_columns if c in numeric]
    lines = ["| row | " + " | ".join(f"{c}: 3DS | Zeste | Zeste/3DS" for c in numeric) + " |",
             "|---|" + "---|---|---|" * len(numeric)]
    for key in list(rows_c) + [k for k in rows_z if k not in rows_c]:
        c, z = rows_c.get(key), rows_z.get(key)
        cells = []
        for col in numeric:
            vc = float(c[col]) if c and c[col] != "" else None
            vz = float(z[col]) if z and z[col] != "" else None
            ratio = f"{vz / vc:.2f}" if vc and vz is not None else "-"
            cells += [f"{vc:g}" if vc is not None else "-", f"{vz:g}" if vz is not None else "-",
                      ratio]
        lines.append("| " + " ".join(k for k in key if k) + " | " + " | ".join(cells) + " |")
    return "\n".join(lines) + "\n"


def cmd_send(a):
    baguette(["put", a.rom, "sd:/cia"], a.baguette)
    print(f"{os.path.basename(a.rom)} is in sdmc:/cia: install it from Cement's bottom screen "
          "(A), run it on the console, then run `hwrun.py compare`.")


def cmd_compare(a):
    sd_dir = sd_path(a.sd_dir)
    name = os.path.splitext(os.path.basename(a.rom))[0]
    out = a.out or os.path.join(ROOT, "build", "hwruns",
                                f"{name}-{datetime.datetime.now():%Y%m%d-%H%M%S}")
    console_dir = os.path.join(out, "console")
    zeste_dir = os.path.join(out, "zeste")
    os.makedirs(console_dir, exist_ok=True)

    if a.console_results:
        shutil.copytree(a.console_results, console_dir, dirs_exist_ok=True)
    else:
        baguette(["--contents", "get", "sd:" + sd_dir, console_dir], a.baguette)

    # Zeste: its SD card is the user folder next to the exe (portable install).
    zeste_sd = os.path.join(os.path.dirname(os.path.abspath(a.zeste)), "user", "sdmc")
    emulated = os.path.join(zeste_sd, sd_dir.strip("/"))
    if os.path.isdir(emulated):
        shutil.rmtree(emulated)
    script = a.script
    if not script:
        script = os.path.join(out, "run.zs")
        with open(script, "w", encoding="utf-8") as f:
            f.write(f"@60 press A 2\n"
                    f"@61 wait-log timeout={a.timeout}s file=sdmc:{sd_dir}/{a.log_name} "
                    f"{a.done_text}\n"
                    f"@62 quit 0\n")
    rom_args = ["--boot-cia", a.rom] if a.rom.lower().endswith(".cia") else [a.rom]
    command = [a.zeste, "--headless", "--script", script] + a.zeste_args + rom_args
    print(" ".join(command))
    run = subprocess.run(command, capture_output=True, text=True, errors="replace")
    with open(os.path.join(out, "zeste_stdout.txt"), "w", encoding="utf-8") as f:
        f.write(run.stdout + run.stderr)
    if run.returncode != 0:
        print(run.stdout[-2000:])
        sys.exit(f"Zeste exited with {run.returncode} (see {out}\\zeste_stdout.txt)")
    shutil.copytree(emulated, zeste_dir, dirs_exist_ok=True)

    console_csv = os.path.join(console_dir, a.csv_name)
    zeste_csv = os.path.join(zeste_dir, a.csv_name)
    for path in (console_csv, zeste_csv):
        if not os.path.isfile(path):
            sys.exit(f"missing {path}")
    table = compare(console_csv, zeste_csv, a.columns.split(",") if a.columns else None,
                    a.keys.split(",") if a.keys else None)
    report = (f"# {name}: console vs Zeste\n\n- ROM: {a.rom}\n- SD folder: {sd_dir}\n"
              f"- Zeste: {a.zeste} {' '.join(a.zeste_args)}\n\n{table}")
    with open(os.path.join(out, "compare.md"), "w", encoding="utf-8") as f:
        f.write(report)
    print(report)
    print(f"Saved in {out}")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    p.add_argument("--baguette", default=DEFAULT_BAGUETTE,
                   help="baguette-cli (default: BAGUETTE_CLI, else the one on PATH)")
    sub = p.add_subparsers(dest="command", required=True)

    s = sub.add_parser("send", help="upload the ROM's CIA to the console (sdmc:/cia)")
    s.add_argument("rom")
    s.set_defaults(func=cmd_send)

    c = sub.add_parser("compare", help="fetch the console results, run Zeste, compare")
    c.add_argument("rom", help=".cia or .3dsx, the one that ran on the console")
    c.add_argument("--sd-dir", required=True,
                   help="results folder on the SD card, e.g. sd:/zeste-sdbench (the sd: form "
                        "keeps Git Bash from rewriting the path)")
    c.add_argument("--csv-name", default="results.csv")
    c.add_argument("--log-name", default="log.txt")
    c.add_argument("--done-text", default="Done", help="the log line that ends the run")
    c.add_argument("--timeout", type=int, default=900, help="seconds Zeste waits for it")
    c.add_argument("--script", help="Zeste script instead of the default (press A, wait)")
    c.add_argument("--columns", help="number columns to compare, comma separated (default: all)")
    c.add_argument("--keys", help="columns that name a row, comma separated (default: the text "
                                  "columns; repeated names get #2, #3...)")
    c.add_argument("--zeste", default=DEFAULT_ZESTE)
    c.add_argument("--zeste-args", nargs=argparse.REMAINDER, default=[],
                   help="more Zeste arguments (last on the line), e.g. --zeste strict")
    c.add_argument("--console-results", help="use this local folder instead of downloading")
    c.add_argument("--out", help="output folder (default build/hwruns/<rom>-<time>)")
    c.set_defaults(func=cmd_compare)

    a = p.parse_args()
    a.func(a)


if __name__ == "__main__":
    main()

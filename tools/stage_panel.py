#!/usr/bin/env python3
"""Stage the 5.79" (L5) application: a copy of the tracked tree (index
included) in OUT, a git repository of its own because the stack's build
takes an application's tracked files, whose cc2530-app.json adds
EPD_PANEL_579 and leaves out CC2530_APP_MGMT_LQI (the L5 header shows no
LQI).

  stage_panel.py OUT
"""
import json
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def git(cwd, *args):
    return subprocess.run(["git", *args], cwd=cwd, check=True, capture_output=True, text=True).stdout


def main():
    out = Path(sys.argv[1]).resolve()
    if out == ROOT or ROOT not in out.parents:
        sys.exit("OUT must be inside the repository (build/...)")
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True)
    names = [n for n in git(ROOT, "ls-files", "-z", "--cached").split("\0") if n]
    for name in names:
        src = ROOT / name
        if src.is_file() and not src.is_symlink():
            (out / name).parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, out / name)
    manifest = json.loads((ROOT / "cc2530-app.json").read_text())
    manifest["description"] = manifest["description"].replace(
        'L8 dashboard on a Waveshare 4.26" SSD1677', 'L5 dashboard on a Waveshare 5.79" two-SSD1683')
    manifest["defines"] = [d for d in manifest["defines"] if d != "-DCC2530_APP_MGMT_LQI"] + ["-DEPD_PANEL_579"]
    (out / "cc2530-app.json").write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n")
    head = git(ROOT, "rev-parse", "HEAD").strip()
    dirty = bool(git(ROOT, "status", "--porcelain", "--untracked-files=no").strip())
    git(out, "init", "-q")
    git(out, "add", "-A")
    git(out, "-c", "user.name=stage_panel", "-c", "user.email=stage_panel@localhost", "commit", "-q",
        "-m", f"L5 stage of {head}{' (dirty)' if dirty else ''}")
    print(f"{out}: L5 stage of {head}{' (dirty)' if dirty else ''}")


if __name__ == "__main__":
    main()

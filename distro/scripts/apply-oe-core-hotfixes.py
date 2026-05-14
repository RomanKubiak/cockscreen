#!/usr/bin/env python3
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]

HOTFIXES = [
    {
        "path": ROOT / "oe-core/meta/classes-global/package_pkgdata.bbclass",
        "old": """                    if l.endswith(\"/\"):\n                        staging_copydir(l, targetdir, dest, seendirs)\n                        continue\n                    staging_copyfile(l, targetdir, dest, postinsts, seendirs)\n""",
        "new": """                    if l.endswith(\"/\"):\n                        staging_copydir(l, targetdir, dest, seendirs)\n                        continue\n                    try:\n                        staging_copyfile(l, targetdir, dest, postinsts, seendirs)\n                    except FileExistsError:\n                        continue\n""",
    },
    {
        "path": ROOT / "oe-core/meta/lib/oe/package.py",
        "old": """            mkdir_recurse(dvar, root, os.path.dirname(file))\n            fpath = os.path.join(root,file)\n            if not cpath.islink(file):\n                os.link(file, fpath)\n                continue\n""",
        "new": """            mkdir_recurse(dvar, root, os.path.dirname(file))\n            fpath = os.path.join(root,file)\n            if not cpath.islink(file):\n                try:\n                    os.link(file, fpath)\n                except FileExistsError:\n                    continue\n                continue\n""",
    },
]


def apply_hotfix(path: Path, old: str, new: str) -> None:
    text = path.read_text()
    if new in text:
        return
    if old not in text:
        raise RuntimeError(f"Expected upstream snippet not found in {path}")
    path.write_text(text.replace(old, new, 1))


for hotfix in HOTFIXES:
    apply_hotfix(hotfix["path"], hotfix["old"], hotfix["new"])

sys.exit(0)

#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build a Debian 13 package using the shared unprivileged sbuild workflow."""

import argparse
from pathlib import Path
import shutil
import subprocess
import tarfile
import time

ROOT = Path(__file__).resolve().parents[1]
EXCLUDED = {"win32", "winamp_plugin", "makedist_win.bat"}


def source_package(output):
    version = (ROOT / "VERSION").read_text().strip().replace("-", "~")
    revision = subprocess.check_output(["dpkg-parsechangelog", "-S", "Version"], cwd=ROOT).decode().strip()
    if revision.rsplit("-", 1)[0] != version:
        raise ValueError("VERSION and debian/changelog disagree")
    epoch = int(subprocess.check_output(["dpkg-parsechangelog", "-S", "Timestamp"], cwd=ROOT))
    if epoch > time.time():
        raise ValueError("debian/changelog has a future timestamp")
    source = output / f"streamripper-{version}"
    source.mkdir()
    paths = subprocess.check_output(
        ["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard"], cwd=ROOT
    ).decode().split("\0")
    for name in sorted(set(filter(None, paths))):
        relative = Path(name)
        if relative.parts[0] in EXCLUDED:
            continue
        path = ROOT / relative
        if not path.is_file():
            continue
        target = source / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, target)
    def metadata(item):
        item.uid = item.gid = 0
        item.uname = item.gname = "root"
        item.mtime = epoch
        return item

    archive = output / f"streamripper_{version}.orig.tar.xz"
    with tarfile.open(archive, "w:xz") as bundle:
        for path in sorted(source.iterdir()):
            if path.name != "debian":
                bundle.add(path, arcname=f"streamripper-{version}/{path.name}", filter=metadata)
    subprocess.run(["dpkg-source", "--build", str(source)], cwd=output, check=True)
    return output / f"streamripper_{revision}.dsc"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--chroot", type=Path, default=Path.home() / ".cache/sbuild/trixie-amd64.tar")
    parser.add_argument("--output", type=Path, default=ROOT / "build/debian-trixie")
    parser.add_argument("--jobs", type=int, choices=range(1, 17), default=2)
    args = parser.parse_args()
    chroot = args.chroot.resolve(strict=True)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    dsc = source_package(output)
    subprocess.run([
        "sbuild", "--chroot-mode=unshare", "--dist=trixie", "--arch=amd64",
        f"--jobs={args.jobs}", "--no-run-lintian", f"--chroot={chroot}", str(dsc)
    ], cwd=output, check=True)
    print(output)


if __name__ == "__main__":
    main()

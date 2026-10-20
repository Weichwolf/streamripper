#!/usr/bin/env python3
"""Run Clang's core, dead-code and POSIX analyzers on every compiled C file."""
import concurrent.futures
import json
from pathlib import Path
import subprocess
import sys


def main():
    build, executable = sys.argv[1:3]
    commands = json.loads((Path(build) / "compile_commands.json").read_text())
    sources = sorted({command["file"] for command in commands})

    def analyze(source):
        return subprocess.run([
            executable, "-p", build, source,
            "--checks=-*,clang-analyzer-core.*,clang-analyzer-deadcode.*,clang-analyzer-unix.*",
            "--warnings-as-errors=*",
        ], capture_output=True, text=True)

    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        results = list(pool.map(analyze, sources))
    for result in results:
        if result.stdout or result.stderr:
            print(result.stdout + result.stderr, end="")
    failures = sum(result.returncode != 0 for result in results)
    print(f"Analyzed {len(sources)} translation units; {failures} failed.")
    return bool(failures)


if __name__ == "__main__":
    sys.exit(main())

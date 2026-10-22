#!/usr/bin/env python3
"""Keep one compile command per file, using the first g++ on PATH.

That g++ is ccache. clangd will not ask ccache for gcc's built-in include
paths, so the C++ standard library headers are written into the command.
"""

import json
import shutil
import subprocess
import sys


def system_include_dirs(compiler):
    result = subprocess.run(
        [compiler, "-E", "-x", "c++", "-v", "-"],
        input="",
        text=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        check=True,
    )
    lines = result.stderr.splitlines()
    try:
        start = lines.index("#include <...> search starts here:") + 1
        end = lines.index("End of search list.")
    except ValueError:
        sys.stderr.write("could not read system include paths from g++\n")
        return None
    return [line.strip() for line in lines[start:end] if line.strip()]


def main():
    path = sys.argv[1]
    compiler = shutil.which("g++")
    if compiler is None:
        sys.stderr.write("g++ was not found on PATH\n")
        return 1
    includes = system_include_dirs(compiler)
    if not includes:
        return 1
    with open(path, "r", encoding="utf-8") as handle:
        entries = json.load(handle)
    kept = []
    seen = set()
    for entry in entries:
        filename = entry["file"]
        if filename in seen:
            continue
        seen.add(filename)
        arguments = [compiler]
        skip_next = False
        saw_std = False
        for argument in entry["arguments"][1:]:
            if skip_next:
                skip_next = False
                continue
            if argument == "-isystem":
                skip_next = True
                continue
            if argument.startswith("-isystem"):
                continue
            if argument.startswith("-std="):
                saw_std = True
            arguments.append(argument)
        if not saw_std:
            arguments.insert(1, "-std=gnu++17")
        source = arguments[-1]
        body = arguments[:-1]
        for directory in includes:
            body.extend(["-isystem", directory])
        body.append(source)
        entry["arguments"] = body
        kept.append(entry)
    with open(path, "w", encoding="utf-8") as handle:
        json.dump(kept, handle, indent=2)
        handle.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())

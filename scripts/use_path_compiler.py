#!/usr/bin/env python3
"""Keep one compile command per file, using the first gcc or g++ on PATH.

Those compilers are ccache. clangd will not ask ccache for the built-in
include paths, so the system headers are written into the command. C sources
stay on gcc; C++ sources stay on g++.
"""

import json
from pathlib import Path
import shutil
import subprocess
import sys


def system_include_dirs(compiler, language):
    result = subprocess.run(
        [compiler, "-E", "-x", language, "-v", "-"],
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
        sys.stderr.write(
            "could not read system include paths from %s\n" % compiler
        )
        return None
    return [line.strip() for line in lines[start:end] if line.strip()]


def main():
    path = sys.argv[1]
    gcc = shutil.which("gcc")
    gxx = shutil.which("g++")
    if gcc is None or gxx is None:
        sys.stderr.write("gcc and g++ were not both found on PATH\n")
        return 1
    c_includes = system_include_dirs(gcc, "c")
    cxx_includes = system_include_dirs(gxx, "c++")
    if not c_includes or not cxx_includes:
        return 1
    with open(path, "r", encoding="utf-8") as handle:
        entries = json.load(handle)
    kept = []
    seen = set()
    for entry in entries:
        filename = entry["file"]
        if filename in seen or not Path(filename).is_file():
            continue
        seen.add(filename)
        c_source = filename.endswith(".c")
        # Bare name. clangd and make both take the first gcc/g++ on PATH.
        # The absolute path is ccache, which clangd will not query.
        compiler = "gcc" if c_source else "g++"
        includes = c_includes if c_source else cxx_includes
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
            arguments.insert(1, "-std=c11" if c_source else "-std=gnu++17")
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

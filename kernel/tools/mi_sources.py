#!/usr/bin/env python3
"""
mi_sources.py -- MI source list for build-mi.sh.

Parses mach3-src/kernel/conf/files (the authoritative upstream build
list) and prints the .c files to compile, honouring `optional`
conditions against the option values in gen_config.py:

  standard            -> always compiled
  optional <conds...> -> compiled iff every condition holds
                         (condition `cpus` is always true;
                          `mach_xxx` maps to gen_config OPTIONS)

Files present in the source dirs but absent from conf/files (upstream
dead code):
  kern/server_loop.c -> compiled (needs -DSERVER_NAME, see build-mi.sh)
  kern/lock_mon.c    -> EXCLUDED (includes <mach/i386/vm_types.h>)
  kern/profile.c     -> EXCLUDED (references removed thread struct fields)

Usage: mi_sources.py <mach3-src-kernel-dir>
Prints one relative path per line, e.g. "kern/ast.c".
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen_config import OPTIONS  # noqa: E402

MI_DIRS = ("kern", "ipc", "vm", "device", "ddb")

# dead-code strays: (action, reason)
STRAY_OK = {"kern/server_loop.c": "needs -DSERVER_NAME (provided by build)"}
STRAY_SKIP = {
    "kern/lock_mon.c": "includes <mach/i386/vm_types.h> and <sys/types.h>; "
                       "not in conf/files upstream",
    "kern/profile.c": "references removed struct thread fields "
                      "(profil_buffer, thread_profiled); not in conf/files",
}


def cond_holds(cond):
    if cond == "cpus":
        return True
    opt = cond.upper()
    if opt in OPTIONS:
        return bool(OPTIONS[opt][0])
    # machine-specific pseudo-device conditions (e.g. from MD sections);
    # MI files should not have these, but fail safe: unknown -> False.
    sys.stderr.write("mi_sources.py: warning: unknown condition '%s'\n"
                     % cond)
    return False


def main():
    kerneldir = sys.argv[1]
    files_path = os.path.join(kerneldir, "conf", "files")
    selected = []
    with open(files_path) as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            parts = line.split()
            path = parts[0]
            if not path.endswith(".c"):
                continue
            d = path.split("/")[0]
            if d not in MI_DIRS:
                continue
            kind = parts[1] if len(parts) > 1 else "standard"
            if kind == "standard":
                selected.append(path)
            elif kind == "optional":
                if all(cond_holds(c) for c in parts[2:]):
                    selected.append(path)
                else:
                    sys.stderr.write("mi_sources.py: skip %s (option off: %s)\n"
                                     % (path, " ".join(parts[2:])))
            else:
                sys.stderr.write("mi_sources.py: warning: odd line: %s\n"
                                 % line)
    # strays
    for path, why in STRAY_OK.items():
        full = os.path.join(kerneldir, path)
        if os.path.exists(full):
            selected.append(path)
        else:
            sys.stderr.write("mi_sources.py: stray missing: %s\n" % path)
    for path, why in STRAY_SKIP.items():
        sys.stderr.write("mi_sources.py: skip dead stray %s (%s)\n"
                         % (path, why))

    for path in sorted(selected):
        print(path)


if __name__ == "__main__":
    main()

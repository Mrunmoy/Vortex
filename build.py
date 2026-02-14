#!/usr/bin/env python3
"""
Build script for ms-runloop.

Usage:
  python build.py                 # build only
  python build.py -c              # clean build
  python build.py -t              # build + run tests
  python build.py -e              # build + examples
  python build.py -c -t -e        # clean build + tests + examples
"""

import argparse
import os
import shutil
import subprocess
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
BUILD_DIR = os.path.join(SCRIPT_DIR, "build")


def run(cmd, **kwargs):
    print(f">>> {' '.join(cmd)}")
    result = subprocess.run(cmd, **kwargs)
    if result.returncode != 0:
        sys.exit(result.returncode)


def clean():
    if os.path.isdir(BUILD_DIR):
        shutil.rmtree(BUILD_DIR)
        print(f">>> Removed {BUILD_DIR}")
    else:
        print(">>> Nothing to clean")


def configure(examples=False):
    os.makedirs(BUILD_DIR, exist_ok=True)
    cmd = [
        "cmake", "-B", BUILD_DIR,
        "-DCMAKE_BUILD_TYPE=Release",
    ]
    if examples:
        cmd.append("-DMS_RUNLOOP_BUILD_EXAMPLES=ON")
    run(cmd, cwd=SCRIPT_DIR)


def build():
    run(["cmake", "--build", BUILD_DIR,
         "-j{}".format(os.cpu_count() or 1)],
        cwd=SCRIPT_DIR)


def test():
    run(["ctest", "--test-dir", BUILD_DIR, "--output-on-failure"],
        cwd=SCRIPT_DIR)


def main():
    parser = argparse.ArgumentParser(description="Build ms-runloop")
    parser.add_argument("-c", "--clean", action="store_true",
                        help="Clean build directory")
    parser.add_argument("-t", "--test", action="store_true",
                        help="Build and run tests")
    parser.add_argument("-e", "--examples", action="store_true",
                        help="Build examples")
    args = parser.parse_args()

    if args.clean:
        clean()
        if not args.test and not args.examples:
            return

    configure(examples=args.examples)
    build()

    if args.test:
        test()


if __name__ == "__main__":
    main()

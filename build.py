#!/usr/bin/env python3
"""
Build script for Vortex.

Usage:
  python build.py                 # build only
  python build.py -c              # clean build
  python build.py -t              # build + run tests
  python build.py -e              # build + examples
  python build.py -p              # build + package SDK
  python build.py -c -t -e        # clean build + tests + examples
"""

import argparse
import platform
import re
import os
import shutil
import subprocess
import sys
import tarfile

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
BUILD_DIR = os.path.join(SCRIPT_DIR, "build")
DIST_DIR = os.path.join(SCRIPT_DIR, "dist")


def read_version():
    env_version = os.environ.get("VORTEX_SDK_VERSION")
    if env_version:
        return env_version

    cmake_path = os.path.join(SCRIPT_DIR, "CMakeLists.txt")
    with open(cmake_path, "r", encoding="utf-8") as fh:
        contents = fh.read()

    match = re.search(r"project\s*\(\s*Vortex\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)", contents)
    if not match:
        raise RuntimeError("Could not determine Vortex version from CMakeLists.txt")
    return match.group(1)


def sdk_platform():
    if sys.platform.startswith("linux"):
        return "linux"
    if sys.platform == "darwin":
        return "macos"
    if sys.platform in ("win32", "cygwin"):
        return "windows"
    return sys.platform


def sdk_arch():
    machine = platform.machine().lower()
    aliases = {
        "amd64": "x86_64",
        "x64": "x86_64",
        "arm64": "aarch64",
    }
    return aliases.get(machine, machine)


def sdk_root_name():
    return f"vortex-sdk-{read_version()}-{sdk_platform()}-{sdk_arch()}"


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
        "cmake",
        "-B",
        BUILD_DIR,
        "-DCMAKE_BUILD_TYPE=Release",
    ]
    if examples:
        cmd.append("-DVORTEX_BUILD_EXAMPLES=ON")
    run(cmd, cwd=SCRIPT_DIR)


def build():
    run(["cmake", "--build", BUILD_DIR, "-j{}".format(os.cpu_count() or 1)], cwd=SCRIPT_DIR)


def test():
    run(["ctest", "--test-dir", BUILD_DIR, "--output-on-failure"], cwd=SCRIPT_DIR)


def remove_if_exists(path):
    if os.path.islink(path) or os.path.isfile(path):
        os.unlink(path)
    elif os.path.isdir(path):
        shutil.rmtree(path)


def write_sdk_example_cmake(example_dir):
    contents = """cmake_minimum_required(VERSION 3.14)
project(vortex_sdk_example LANGUAGES C)

find_package(vortex CONFIG REQUIRED
    PATHS "${CMAKE_CURRENT_LIST_DIR}/../lib/cmake/vortex"
    NO_DEFAULT_PATH
)

add_executable(c_timer_loop timer_loop.c)
target_link_libraries(c_timer_loop PRIVATE vortex::vortex)
"""
    with open(os.path.join(example_dir, "CMakeLists.txt"), "w", encoding="utf-8") as fh:
        fh.write(contents)


def write_sdk_readme(staging_dir):
    version = read_version()
    contents = f"""# Vortex SDK

Vortex {version} is a C API wrapper and event loop SDK package.

## Contents

- `include/vortex.h`: stable C API
- `lib/libvortex.a`: static library
- `lib/libvortex.so*`: shared library
- `lib/pkgconfig/vortex.pc`: pkg-config metadata
- `lib/cmake/vortex/`: CMake package config
- `example/timer_loop.c`: standalone C example

## Build the example

```bash
cd example
cmake -B build
cmake --build build
./build/c_timer_loop
```
"""
    with open(os.path.join(staging_dir, "README.md"), "w", encoding="utf-8") as fh:
        fh.write(contents)


def cleanup_test_artifacts(staging_dir):
    lib_dir = os.path.join(staging_dir, "lib")
    if os.path.isdir(lib_dir):
        for name in os.listdir(lib_dir):
            if name.startswith(("libgtest", "libgmock")):
                remove_if_exists(os.path.join(lib_dir, name))

    cmake_dir = os.path.join(lib_dir, "cmake")
    if os.path.isdir(cmake_dir):
        for name in os.listdir(cmake_dir):
            if "gtest" in name.lower() or "gmock" in name.lower():
                remove_if_exists(os.path.join(cmake_dir, name))

    pkgconfig_dir = os.path.join(lib_dir, "pkgconfig")
    if os.path.isdir(pkgconfig_dir):
        for name in os.listdir(pkgconfig_dir):
            if "gtest" in name.lower() or "gmock" in name.lower():
                remove_if_exists(os.path.join(pkgconfig_dir, name))


def package_sdk():
    root_name = sdk_root_name()
    os.makedirs(DIST_DIR, exist_ok=True)
    staging_dir = os.path.join(DIST_DIR, root_name)
    archive_path = os.path.join(SCRIPT_DIR, f"{root_name}.tar.gz")

    remove_if_exists(staging_dir)
    remove_if_exists(archive_path)

    run(["cmake", "--install", BUILD_DIR, "--prefix", staging_dir], cwd=SCRIPT_DIR)
    cleanup_test_artifacts(staging_dir)

    example_dir = os.path.join(staging_dir, "example")
    os.makedirs(example_dir, exist_ok=True)
    shutil.copy2(os.path.join(SCRIPT_DIR, "example", "c-usage", "timer_loop.c"),
                 os.path.join(example_dir, "timer_loop.c"))
    write_sdk_example_cmake(example_dir)

    shutil.copy2(os.path.join(SCRIPT_DIR, "LICENSE"), os.path.join(staging_dir, "LICENSE"))
    write_sdk_readme(staging_dir)

    with tarfile.open(archive_path, "w:gz") as tar:
        tar.add(staging_dir, arcname=root_name)
    print(f">>> Created {archive_path}")


def main():
    parser = argparse.ArgumentParser(description="Build Vortex")
    parser.add_argument("-c", "--clean", action="store_true", help="Clean build directory")
    parser.add_argument("-t", "--test", action="store_true", help="Build and run tests")
    parser.add_argument("-e", "--examples", action="store_true", help="Build examples")
    parser.add_argument("-p", "--package", action="store_true", help="Install and package the SDK")
    args = parser.parse_args()

    if args.clean:
        clean()
        if not args.test and not args.examples and not args.package:
            return

    configure(examples=args.examples)
    build()

    if args.test:
        test()
    if args.package:
        package_sdk()


if __name__ == "__main__":
    main()

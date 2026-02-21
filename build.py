#!/usr/bin/env python3
import argparse
import os
import subprocess
import sys
from datetime import datetime
import glob

def detect_src_dir():
    if os.path.isdir("src"):
        return "src"
    if os.path.isdir("source"):
        return "source"
    print("ERROR: cannot find source directory: 'src' or 'source'", file=sys.stderr)
    sys.exit(1)

def run(cmd, **kwargs):
    print(" ".join(cmd))
    subprocess.run(cmd, check=True, **kwargs)

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--img", default=os.environ.get("IMG", "image.img"))
    parser.add_argument("--size", default=os.environ.get("SIZE", "64MiB"))
    parser.add_argument("--iters", type=int, default=int(os.environ.get("ITERS", "100")))
    parser.add_argument("--mode", choices=["release", "debug"], default=os.environ.get("MODE", "release"))
    parser.add_argument("--out", default=os.environ.get("OUT", "results.txt"))

    parser.add_argument("--do-build", action="store_true")
    parser.add_argument("--do-image", action="store_true")
    parser.add_argument("--do-run", action="store_true")
    parser.add_argument("--clean", action="store_true")

    args = parser.parse_args()

    do_build = args.do_build
    do_image = args.do_image
    do_run   = args.do_run

    if not (do_build or do_image or do_run):
        do_build = True
        do_image = True
        do_run   = True

    src_dir = detect_src_dir()
    src_files = sorted(glob.glob(os.path.join(src_dir, "*.c")))
    if not src_files:
        print(f"ERROR: no .c files found in {src_dir}", file=sys.stderr)
        sys.exit(1)

    if args.mode == "debug":
        cflags = ["-std=c11", "-O0", "-g", "-Wall", "-Wextra"]
    else:
        cflags = ["-std=c11", "-O2", "-DNDEBUG", "-Wall", "-Wextra"]

    if do_image:
        print(f"[image] creating {args.img} (size={args.size})")
        run(["python3", "fat_builder.py", "--size", args.size, "--output", args.img, "--init-root-dir"])

    if do_build:
        print(f"[build] gcc {args.mode}")
        run(["gcc", *cflags, "-Iinclude", "main.c", *src_files, "-o", "bench.bin"])

    if do_run:
        if not os.path.isfile(args.img):
            print(f"ERROR: image not found: {args.img} (run with --do-image first)", file=sys.stderr)
            sys.exit(1)
        if not os.path.isfile("./bench.bin"):
            print("ERROR: bench.bin not found (run with --do-build first)", file=sys.stderr)
            sys.exit(1)

        print(f"[run] ./bench.bin {args.iters} {args.img}")

        header = [
            "-----",
            f"date: {datetime.now().isoformat(timespec='seconds')}",
            f"mode: {args.mode}",
            f"iters: {args.iters}",
            f"image: {args.img}",
            f"size: {args.size}",
            "-----",
        ]

        with open(args.out, "a", encoding="utf-8") as f:
            for line in header:
                print(line)
                f.write(line + "\n")

            p = subprocess.Popen(["./bench.bin", str(args.iters), '64', args.img],
                                 stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT,
                                 text=True)
            for line in p.stdout:
                print(line, end="")
                f.write(line)
            p.wait()

            print()
            f.write("\n")

    if args.clean:
        print(f"[clean] removing bench.bin and {args.img}")
        try:
            os.remove("bench.bin")
        except FileNotFoundError:
            pass
        try:
            os.remove(args.img)
        except FileNotFoundError:
            pass

if __name__ == "__main__":
    main()
"""Identify a local custom binary without pretending dirty sources are a commit."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import re
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def git(*args):
    return subprocess.check_output(["git", "-c", "safe.directory=" + str(ROOT).replace("\\", "/"),
                                    "-c", "core.safecrlf=false", *args], cwd=ROOT)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--build-info", type=Path, help="Compiler/platform JSON recorded inside a separate build environment")
    args = parser.parse_args()
    commit = git("rev-parse", "HEAD").decode().strip()
    patch = git("diff", "--binary", "HEAD")
    digest = hashlib.sha256(patch)
    untracked = sorted(p for p in git("ls-files", "--others", "--exclude-standard", "-z").decode().split("\0") if p)
    for name in untracked:
        digest.update(name.encode() + b"\0" + (ROOT / name).read_bytes() + b"\0")
    dirty = bool(patch or untracked)
    manifest = {
        "source_commit": commit, "source_dirty": dirty,
        "working_tree_patch_sha256": digest.hexdigest(),
        "artifact_id": commit[:12] + ("-dirty-" + digest.hexdigest()[:12] if dirty else ""),
        "binary": args.binary.name, "binary_sha256": hashlib.sha256(args.binary.read_bytes()).hexdigest(),
        "platform": platform.platform(),
        "compiler": subprocess.check_output(["g++", "--version"]).decode().splitlines()[0],
        "dependencies": {},
    }
    if args.build_info:
        info = json.loads(args.build_info.read_text())
        manifest["compiler"], manifest["platform"] = info["compiler"], info["platform"]
        manifest["build_environment"] = info
    try:
        symbols = subprocess.check_output(["readelf", "--version-info", str(args.binary)]).decode()
        manifest["minimum_symbol_versions"] = {}
        for prefix in ("GLIBC", "GLIBCXX", "CXXABI"):
            versions = set(re.findall(prefix + r"_([0-9.]+)", symbols))
            if versions:
                manifest["minimum_symbol_versions"][prefix] = max(versions, key=lambda value: tuple(map(int, value.split("."))))
    except FileNotFoundError:
        pass
    for name in ("", "/sourcesdk-minimal", "/detouring", "/detouring/minhook", "/scanning"):
        directory = ROOT / ("development/garrysmod_common" + name)
        if directory.exists():
            manifest["dependencies"]["garrysmod_common" + name] = subprocess.check_output(
                ["git", "-c", "safe.directory=" + str(directory).replace("\\", "/"), "rev-parse", "HEAD"], cwd=directory).decode().strip()
    output = args.output or args.binary.with_suffix(".manifest.json")
    output.write_text(json.dumps(manifest, indent=2) + "\n")
    print(output)


if __name__ == "__main__":
    main()

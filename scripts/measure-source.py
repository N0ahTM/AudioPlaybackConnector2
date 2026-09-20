"""Measure tracked production source at immutable Git revisions; no checkout/build needed."""

import argparse
import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PRODUCT_ROOTS = (
    "AudioPlaybackConnector2/",
    "AudioPlaybackConnector2.Control/",
    "AudioPlaybackConnector2.CoreRuntime/",
)
EXTENSIONS = {".cpp", ".hpp", ".h"}


def git(*args):
    return subprocess.check_output(["git", *args], cwd=ROOT)


def measure(revision):
    commit = git("rev-parse", "--verify", "--end-of-options", revision + "^{commit}").decode().strip()
    entries = git("ls-tree", "-r", "-z", commit).split(b"\0")
    sources = []
    for entry in entries:
        if not entry:
            continue
        metadata, raw_path = entry.split(b"\t", 1)
        mode, kind, object_id = metadata.split()
        path = raw_path.decode("utf-8")
        if (kind == b"blob" and mode in {b"100644", b"100755"}
                and path.startswith(PRODUCT_ROOTS) and Path(path).suffix in EXTENSIONS):
            sources.append((path, object_id))

    # Object IDs, not shell-expanded paths; one Git process for the whole tree.
    objects = subprocess.run(
        ["git", "cat-file", "--batch"], cwd=ROOT,
        input=b"".join(object_id + b"\n" for _, object_id in sources),
        capture_output=True, check=True,
    ).stdout
    offset = 0
    files = []
    for path, object_id in sources:
        end = objects.index(b"\n", offset)
        actual_id, kind, size = objects[offset:end].split()
        if actual_id != object_id or kind != b"blob":
            raise ValueError("Unexpected Git object response for " + path)
        size = int(size)
        content = objects[end + 1:end + 1 + size]
        offset = end + 2 + size
        lines = content.splitlines()
        files.append({"path": path, "bytes": size, "lines": len(lines),
                      "nonblank_lines": sum(bool(line.strip()) for line in lines)})
    if offset != len(objects):
        raise ValueError("Unexpected trailing Git object data")
    return {
        "commit": commit,
        "file_count": len(files),
        "bytes": sum(file["bytes"] for file in files),
        "lines": sum(file["lines"] for file in files),
        "nonblank_lines": sum(file["nonblank_lines"] for file in files),
        "largest_files": sorted(files, key=lambda file: (-file["lines"], file["path"]))[:10],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", required=True)
    parser.add_argument("--revision", default="HEAD")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    baseline = measure(args.baseline)
    current = measure(args.revision)
    report = {
        "schema_version": 1,
        "scope": {
            "roots": list(PRODUCT_ROOTS), "extensions": sorted(EXTENSIONS),
            "definition": "Physical lines including comments and includes; nonblank lines exclude only whitespace. "
                          "Tracked regular files only. Tests, benchmarks, resources and build outputs are excluded. "
                          "These counts do not measure executable logic, public APIs or complexity.",
        },
        "baseline": baseline,
        "current": current,
        "delta": {key: current[key] - baseline[key]
                  for key in ("file_count", "bytes", "lines", "nonblank_lines")},
    }
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"baseline": baseline["commit"], "current": current["commit"],
                      "delta": report["delta"]}))


if __name__ == "__main__":
    main()

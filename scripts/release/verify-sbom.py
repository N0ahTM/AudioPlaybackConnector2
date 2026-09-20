"""Verify SPDX file coverage, SHA256 hashes, and bundled external SPDX documents."""
import argparse
import hashlib
import json
from pathlib import Path


def verify(drop):
    drop = drop.resolve()
    document = json.loads((drop / "_manifest/spdx_2.2/manifest.spdx.json").read_text(encoding="utf-8-sig"))
    if document["spdxVersion"] != "SPDX-2.2":
        raise ValueError("Expected SPDX 2.2")
    actual = {p.relative_to(drop).as_posix(): p for p in drop.rglob("*")
              if p.is_file() and p.relative_to(drop).parts[0] != "_manifest"}
    seen = set()
    for entry in document["files"]:
        name = entry["fileName"].removeprefix("./")
        if name in seen or name not in actual:
            raise ValueError(f"Duplicate or unexpected SBOM file: {name}")
        seen.add(name)
        hashes = [h["checksumValue"].lower() for h in entry["checksums"] if h["algorithm"] == "SHA256"]
        digest = hashlib.sha256(actual[name].read_bytes()).hexdigest()
        if hashes != [digest]:
            raise ValueError(f"SHA256 mismatch: {name}")
    if seen != set(actual):
        raise ValueError(f"Files absent from SBOM: {sorted(set(actual) - seen)}")
    bundled = {}
    for path in (drop / "SBOM/vcpkg").rglob("vcpkg.spdx.json"):
        data = path.read_bytes()
        sbom = json.loads(data)
        bundled[(sbom["documentNamespace"], hashlib.sha1(data).hexdigest())] = path
    references = set()
    for ref in document.get("externalDocumentRefs", []):
        if ref["checksum"]["algorithm"] != "SHA1":
            raise ValueError("Unexpected external-document checksum algorithm")
        key = (ref["spdxDocument"], ref["checksum"]["checksumValue"].lower())
        if key not in bundled:
            raise ValueError("Referenced SPDX document is not bundled with matching content")
        references.add(key)
    if not bundled or references != set(bundled):
        raise ValueError("Bundled vcpkg SPDX documents are absent from generated SBOM references")
    return len(seen), len(references)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--drop", type=Path, required=True)
    args = parser.parse_args()
    files, references = verify(args.drop)
    print(f"Verified {files} payload hashes and {references} external SPDX documents.")

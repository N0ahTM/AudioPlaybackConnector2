"""Validate restored vcpkg licenses against the reviewed inventory and notices."""
import argparse
import hashlib
import json
from pathlib import Path
import re


def normalized(text):
    return re.sub(r"\s+", " ", text).strip()


def verify(root, triplet):
    inventory = json.loads((root / "config/dependency-licenses.json").read_text(encoding="utf-8"))["vcpkg"]
    expected = {entry["name"]: entry for entry in inventory}
    if len(expected) != len(inventory):
        raise ValueError("Duplicate dependency in license inventory")
    manifest = json.loads((root / "vcpkg.json").read_text(encoding="utf-8"))
    product = {name for name, entry in expected.items() if entry["scope"] == "product"}
    direct = {entry if isinstance(entry, str) else entry["name"] for entry in manifest["dependencies"]}
    if direct != product:
        raise ValueError("Direct dependencies differ from the reviewed product inventory")
    overrides = {entry["name"]: entry for entry in manifest["overrides"]}
    if set(overrides) != product:
        raise ValueError("Every product dependency needs exactly one reviewed version override")
    for name in product:
        pin = overrides[name]
        reviewed = expected[name]
        if pin["version"] != reviewed["version"] or pin.get("port-version", 0) != reviewed["portVersion"]:
            raise ValueError(f"Manifest pin differs from reviewed license version: {name}")
    installed = root / "vcpkg_installed" / triplet
    status = (installed / "vcpkg/status").read_text(encoding="utf-8")
    notices = normalized((root / "THIRD_PARTY_NOTICES.md").read_text(encoding="utf-8-sig"))
    seen = set()
    for paragraph in re.split(r"\n\s*\n", status):
        fields = dict(line.split(": ", 1) for line in paragraph.splitlines() if ": " in line and not line[0].isspace())
        if fields.get("Status") != "install ok installed":
            continue
        name = fields["Package"]
        if name not in expected:
            raise ValueError(f"Unreviewed restored dependency: {name}")
        if "Feature" in fields:
            raise ValueError(f"Unreviewed optional dependency feature: {name}:{fields['Feature']}")
        if name in seen:
            raise ValueError(f"Duplicate restored dependency: {name}")
        seen.add(name)
        entry = expected[name]
        if fields.get("Version") != entry["version"] or int(fields.get("Port-Version", 0)) != entry["portVersion"]:
            raise ValueError(f"Restored version differs from reviewed inventory: {name}")
        architecture = fields["Architecture"]
        required = triplet if entry["scope"] == "product" else "x64-windows"
        if architecture != required:
            raise ValueError(f"Unexpected dependency architecture: {name}:{architecture}")
        copyright_path = installed / architecture / "share" / name / "copyright"
        copyright_text = normalized(copyright_path.read_text(encoding="utf-8-sig"))
        digest = hashlib.sha256(copyright_text.encode("utf-8")).hexdigest()
        if digest != entry["copyrightSha256"]:
            raise ValueError(f"Copyright text changed; license review required: {name}")
        if copyright_text not in notices:
            raise ValueError(f"Complete reviewed copyright missing from notices: {name}")
    if seen != set(expected):
        raise ValueError(f"Reviewed dependencies missing from restore: {sorted(set(expected) - seen)}")
    return len(seen)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--triplet", choices=["x64-windows-static-md", "arm64-windows-static-md"], required=True)
    args = parser.parse_args()
    count = verify(args.root, args.triplet)
    print(f"Verified {count} restored dependencies and complete notices for {args.triplet}.")

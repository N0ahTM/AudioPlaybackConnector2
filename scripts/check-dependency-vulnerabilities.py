"""Query OSV for restored C++ upstream commits and exact project NuGet versions."""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import re
import runpy
import subprocess
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ET


def request_json(url, payload=None):
    data = None if payload is None else json.dumps(payload).encode()
    request = urllib.request.Request(url, data=data, headers={
        "User-Agent": "AudioPlaybackConnector2-dependency-check", "Content-Type": "application/json"})
    with urllib.request.urlopen(request, timeout=30) as response:
        return json.load(response)


def collect_queries(root, triplet, fetch=request_json):
    license_check = runpy.run_path(str(root / "scripts/verify-dependency-licenses.py"))["verify"]
    license_check(root, triplet)
    entries = json.loads((root / "config/dependency-licenses.json").read_text(encoding="utf-8"))["vcpkg"]
    queries = []
    labels = []
    for entry in entries:
        if entry["scope"] != "product":
            continue
        path = root / "vcpkg_installed" / triplet / triplet / "share" / entry["name"] / "vcpkg.spdx.json"
        resources = [p for p in json.loads(path.read_text(encoding="utf-8"))["packages"] if p["SPDXID"].startswith("SPDXRef-resource-")]
        if not resources:
            raise ValueError(f"No upstream source metadata: {entry['name']}")
        for resource in resources:
            match = re.fullmatch(r"git\+https://github.com/([\w.-]+/[\w.-]+)@(.+)", resource["downloadLocation"])
            if not match:
                raise ValueError(f"Unreviewed upstream source format: {resource['downloadLocation']}")
            repository, ref = match.groups()
            commit = fetch(f"https://api.github.com/repos/{repository}/commits/{urllib.parse.quote(ref, safe='')}")["sha"]
            if not re.fullmatch(r"[0-9a-f]{40}", commit):
                raise ValueError("Invalid upstream commit response")
            queries.append({"commit": commit})
            labels.append(f"{entry['name']} {entry['version']} ({repository}@{ref}, {commit})")
    configs = subprocess.check_output(["git", "-C", str(root), "ls-files", "**/packages.config"], text=True).splitlines()
    packages = set()
    for config in configs:
        packages.update((p.attrib["id"], p.attrib["version"]) for p in ET.parse(root / config).getroot())
    if not packages:
        raise ValueError("No project NuGet packages found")
    for name, version in sorted(packages):
        queries.append({"package": {"ecosystem": "NuGet", "name": name}, "version": version})
        labels.append(f"NuGet {name}@{version}")
    return labels, queries


def classify(labels, queries, response):
    results = response["results"]
    if len(results) != len(queries):
        raise ValueError("Incomplete OSV response")
    records = []
    for label, query, result in zip(labels, queries, results):
        if not isinstance(result, dict) or "error" in result or result.get("next_page_token"):
            raise ValueError("OSV query failed or returned incomplete paginated results")
        ids = sorted({v["id"] for v in result.get("vulns", [])})
        records.append({"dependency": label, "query": query, "advisories": ids})
    return records


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--triplet", choices=["x64-windows-static-md", "arm64-windows-static-md"], default="x64-windows-static-md")
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    labels, queries = collect_queries(args.root, args.triplet)
    records = classify(labels, queries, request_json("https://api.osv.dev/v1/querybatch", {"queries": queries}))
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps({"checkedAt": datetime.now(timezone.utc).isoformat(), "results": records}, indent=2) + "\n", encoding="utf-8")
    findings = [r for r in records if r["advisories"]]
    print(f"OSV: {len(records)} dependency queries, {len(findings)} with advisories. Report: {args.report}")
    for record in findings:
        print(record["dependency"] + ": " + ", ".join(record["advisories"]))
    raise SystemExit(1 if findings else 0)

"""Keep the CoreRuntime and CoreTests Solution Explorer folders in sync."""

from __future__ import annotations

import argparse
import pathlib
import sys
import uuid
import xml.etree.ElementTree as ET


ROOT = pathlib.Path(__file__).resolve().parent.parent
NS = "http://schemas.microsoft.com/developer/msbuild/2003"
ITEM_TYPES = {"ClCompile", "ClInclude", "None", "ResourceCompile"}
PROJECTS = ("AudioPlaybackConnector2.CoreRuntime", "AudioPlaybackConnector2.CoreTests")
ALL_PROJECTS = ("AudioPlaybackConnector2", "AudioPlaybackConnector2.Control", *PROJECTS)
VISIBLE_TYPES = ITEM_TYPES | {"ApplicationDefinition", "Content", "Manifest", "Midl", "Page"}
AREA_NAMES = {"app": "App", "control": "Control", "core": "Core", "services": "Services", "ui": "UI", "util": "Util"}

TEST_GROUPS = {
    "Application": (
        "Adaptive", "App", "LatestStartup", "PowerTransition", "RatingPrompt",
        "ResourcePressure", "SingleInstance", "StartupTask", "UiDispatcher", "UiRefresh",
    ),
    "Control": ("CliParser", "Command", "Control", "ProtocolBoundary"),
    "Devices": ("Device", "ReconnectPolicy", "TrayTooltip"),
    "Diagnostics": ("CrashHandler", "DiagnosticsLogCollector", "Logger", "RuntimeApartment", "SettingsDiagnosticsReportBuilder"),
    "Localization": ("StringResources",),
    "Notifications": ("ToastContentBuilder",),
    "Settings": ("ManualSettingsWakeup", "SettingsCodec", "SettingsLimits", "SettingsStore"),
    "Shared": ("Main", "TestCheck"),
}


def test_group(filename: str) -> str:
    if filename.startswith("DevicePicker"):
        return "Presentation"
    matches = [group for group, prefixes in TEST_GROUPS.items() if filename.startswith(prefixes)]
    if len(matches) != 1:
        raise ValueError(f"Test file needs one owner folder: {filename} ({matches})")
    return matches[0]


def filter_for(project: str, item_type: str, include: str) -> str:
    parts = include.replace("/", "\\").split("\\")
    if item_type == "None" and parts[-1] == "packages.config":
        return "Dependencies"
    if item_type == "ResourceCompile" and parts[-1] == "StringResourcesTests.rc":
        return "Tests\\Localization"
    if parts[0] == ".." and len(parts) >= 4 and parts[1] == "AudioPlaybackConnector2":
        area = parts[2]
        if area in ("src", "include"):
            root = "Source" if area == "src" else "Headers"
            return root + "\\" + AREA_NAMES[parts[3]]
    if parts[0] == "include":
        return "Headers"
    if parts[0] == "src":
        if project.endswith("CoreTests"):
            return "Tests\\" + test_group(parts[-1])
        return "Source"
    raise ValueError(f"No Solution Explorer folder for {project}: {item_type} {include}")


def render(project: str) -> bytes:
    path = ROOT / project / f"{project}.vcxproj"
    source = ET.parse(path).getroot()
    items = [(element.tag.rsplit("}", 1)[-1], element.attrib["Include"])
             for element in source.iter()
             if element.tag.rsplit("}", 1)[-1] in ITEM_TYPES and "Include" in element.attrib]
    if len(items) != len(set(items)):
        raise ValueError(f"Duplicate project item in {path}")
    mapped = [(filter_for(project, kind, include), kind, include) for kind, include in items]
    folders = {folder for folder, _, _ in mapped}
    folders.update(folder.split("\\")[0] for folder in tuple(folders) if "\\" in folder)

    ET.register_namespace("", NS)
    root = ET.Element(f"{{{NS}}}Project", {"ToolsVersion": "4.0"})
    definitions = ET.SubElement(root, f"{{{NS}}}ItemGroup")
    for folder in sorted(folders, key=str.casefold):
        entry = ET.SubElement(definitions, f"{{{NS}}}Filter", {"Include": folder})
        identifier = uuid.uuid5(uuid.NAMESPACE_URL, f"AudioPlaybackConnector2/{project}/{folder}")
        ET.SubElement(entry, f"{{{NS}}}UniqueIdentifier").text = "{" + str(identifier) + "}"
    group = ET.SubElement(root, f"{{{NS}}}ItemGroup")
    for folder, kind, include in sorted(mapped, key=lambda row: (row[0].casefold(), row[2].casefold())):
        entry = ET.SubElement(group, f"{{{NS}}}{kind}", {"Include": include})
        ET.SubElement(entry, f"{{{NS}}}Filter").text = folder
    ET.indent(root, space="  ")
    return ET.tostring(root, encoding="utf-8", xml_declaration=True).replace(b"\n", b"\r\n") + b"\r\n"


def validate_coverage(project: str) -> None:
    directory = ROOT / project
    source = ET.parse(directory / f"{project}.vcxproj").getroot()
    filtered = ET.parse(directory / f"{project}.vcxproj.filters").getroot()
    items = {(entry.tag.rsplit("}", 1)[-1], entry.attrib["Include"])
             for entry in source.iter()
             if entry.tag.rsplit("}", 1)[-1] in VISIBLE_TYPES and "Include" in entry.attrib}
    folders = {entry.attrib["Include"] for entry in filtered.iter()
               if entry.tag.rsplit("}", 1)[-1] == "Filter" and "Include" in entry.attrib}
    mappings = {}
    for entry in filtered.iter():
        kind = entry.tag.rsplit("}", 1)[-1]
        if kind not in VISIBLE_TYPES or "Include" not in entry.attrib:
            continue
        key = (kind, entry.attrib["Include"])
        if key in mappings:
            raise ValueError(f"Duplicate filter mapping in {project}: {key}")
        mapping = entry.find(f"{{{NS}}}Filter")
        mappings[key] = mapping.text if mapping is not None else None
    for item in sorted(items):
        if not mappings.get(item) or mappings[item] not in folders:
            raise ValueError(f"Missing Solution Explorer folder in {project}: {item}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true", help="Fail when generated filters differ")
    args = parser.parse_args()
    stale = []
    for project in PROJECTS:
        path = ROOT / project / f"{project}.vcxproj.filters"
        expected = render(project)
        if args.check:
            if not path.exists() or path.read_bytes().replace(b"\r\n", b"\n") != expected.replace(b"\r\n", b"\n"):
                stale.append(str(path.relative_to(ROOT)))
        else:
            path.write_bytes(expected)
            print(f"Updated {path.relative_to(ROOT)}")
    if stale:
        print("Outdated Visual Studio filters: " + ", ".join(stale), file=sys.stderr)
        return 1
    for project in ALL_PROJECTS:
        validate_coverage(project)
    if args.check:
        print("Visual Studio filters cover every project item.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

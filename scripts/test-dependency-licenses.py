"""Exercise license-gate rejection paths without a package restore."""
import hashlib
import json
from pathlib import Path
import runpy
import tempfile
import unittest

verify = runpy.run_path(str(Path(__file__).with_name("verify-dependency-licenses.py")))["verify"]
TRIPLET = "x64-windows-static-md"


class LicenseGateTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="apc-license-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / "config").mkdir()
        self.copyright = "MIT License\nCopyright Example\nPermission is granted.\n"
        self.manifest = {"dependencies": ["example"], "overrides": [{"name": "example", "version": "1.2.3"}]}
        self.write_json("vcpkg.json", self.manifest)
        entry = dict(name="example", version="1.2.3", portVersion=0, scope="product", license="MIT",
                     copyrightSha256=hashlib.sha256(" ".join(self.copyright.split()).encode()).hexdigest())
        self.write_json("config/dependency-licenses.json", {"vcpkg": [entry]})
        self.installed = self.root / "vcpkg_installed" / TRIPLET
        (self.installed / "vcpkg").mkdir(parents=True)
        self.status = self.installed / "vcpkg/status"
        self.status.write_text(f"Package: example\nVersion: 1.2.3\nArchitecture: {TRIPLET}\nStatus: install ok installed\n")
        self.license = self.installed / TRIPLET / "share/example/copyright"
        self.license.parent.mkdir(parents=True)
        self.license.write_text(self.copyright)
        (self.root / "THIRD_PARTY_NOTICES.md").write_text(self.copyright)

    def write_json(self, name, data):
        (self.root / name).write_text(json.dumps(data))

    def test_reviewed_restore(self):
        self.assertEqual(verify(self.root, TRIPLET), 1)

    def test_line_endings_and_spacing(self):
        self.license.write_bytes(self.copyright.replace("\n", "\r\n\r\n").encode())
        self.assertEqual(verify(self.root, TRIPLET), 1)

    def test_changed_license(self):
        self.license.write_text(self.copyright + "Additional terms")
        with self.assertRaisesRegex(ValueError, "Copyright text changed"):
            verify(self.root, TRIPLET)

    def test_missing_notice(self):
        (self.root / "THIRD_PARTY_NOTICES.md").write_text("MIT License")
        with self.assertRaisesRegex(ValueError, "copyright missing"):
            verify(self.root, TRIPLET)

    def test_changed_manifest_pin(self):
        self.manifest["overrides"][0]["version"] = "2.0.0"
        self.write_json("vcpkg.json", self.manifest)
        with self.assertRaisesRegex(ValueError, "Manifest pin differs"):
            verify(self.root, TRIPLET)

    def test_changed_restored_version(self):
        self.status.write_text(self.status.read_text().replace("1.2.3", "1.2.4"))
        with self.assertRaisesRegex(ValueError, "Restored version differs"):
            verify(self.root, TRIPLET)

    def test_new_transitive_dependency(self):
        self.status.write_text(self.status.read_text() + "\nPackage: unexpected\nStatus: install ok installed\n")
        with self.assertRaisesRegex(ValueError, "Unreviewed restored dependency"):
            verify(self.root, TRIPLET)

    def test_new_feature(self):
        self.status.write_text(self.status.read_text() + "\nPackage: example\nFeature: fmt\nStatus: install ok installed\n")
        with self.assertRaisesRegex(ValueError, "Unreviewed optional"):
            verify(self.root, TRIPLET)

    def test_missing_restore(self):
        self.status.write_text("")
        with self.assertRaisesRegex(ValueError, "missing from restore"):
            verify(self.root, TRIPLET)


if __name__ == "__main__":
    unittest.main()

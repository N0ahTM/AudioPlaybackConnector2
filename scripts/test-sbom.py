"""Test release SBOM coverage and tamper rejection without downloading tools."""
import hashlib
import json
from pathlib import Path
import runpy
import tempfile
import unittest

verify = runpy.run_path(str(Path(__file__).parent / "release/verify-sbom.py"))["verify"]


class SbomTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix="apc-sbom-test-")
        self.addCleanup(temp.cleanup)
        self.drop = Path(temp.name)
        (self.drop / "application.msix").write_bytes(b"package fixture")
        external = self.drop / "SBOM/vcpkg/example/vcpkg.spdx.json"
        external.parent.mkdir(parents=True)
        external.write_text(json.dumps({"documentNamespace": "https://example.test/component"}))
        self.document = {"spdxVersion": "SPDX-2.2", "files": [], "externalDocumentRefs": [
            {"spdxDocument": "https://example.test/component", "checksum":
             {"algorithm": "SHA1", "checksumValue": hashlib.sha1(external.read_bytes()).hexdigest()}}]}
        for path in self.drop.rglob("*"):
            if path.is_file():
                self.document["files"].append({"fileName": "./" + path.relative_to(self.drop).as_posix(),
                    "checksums": [{"algorithm": "SHA256", "checksumValue": hashlib.sha256(path.read_bytes()).hexdigest()}]})
        self.manifest = self.drop / "_manifest/spdx_2.2/manifest.spdx.json"
        self.manifest.parent.mkdir(parents=True)
        self.save()

    def save(self):
        self.manifest.write_text(json.dumps(self.document))

    def test_complete_drop(self):
        self.assertEqual(verify(self.drop), (2, 1))

    def test_changed_package(self):
        (self.drop / "application.msix").write_bytes(b"tampered")
        with self.assertRaisesRegex(ValueError, "SHA256 mismatch"):
            verify(self.drop)

    def test_added_file(self):
        (self.drop / "extra.msix").write_bytes(b"unlisted")
        with self.assertRaisesRegex(ValueError, "absent from SBOM"):
            verify(self.drop)

    def test_missing_file(self):
        (self.drop / "application.msix").unlink()
        with self.assertRaisesRegex(ValueError, "unexpected SBOM file"):
            verify(self.drop)

    def test_duplicate_file(self):
        self.document["files"].append(self.document["files"][0])
        self.save()
        with self.assertRaisesRegex(ValueError, "Duplicate"):
            verify(self.drop)

    def test_missing_reference(self):
        self.document["externalDocumentRefs"] = []
        self.save()
        with self.assertRaisesRegex(ValueError, "absent from generated SBOM"):
            verify(self.drop)

    def test_changed_reference(self):
        self.document["externalDocumentRefs"][0]["checksum"]["checksumValue"] = "0" * 40
        self.save()
        with self.assertRaisesRegex(ValueError, "not bundled with matching content"):
            verify(self.drop)


if __name__ == "__main__":
    unittest.main()

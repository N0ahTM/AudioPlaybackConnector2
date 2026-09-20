"""Check that advisory-query failures cannot produce a clean result."""
from pathlib import Path
import runpy
import unittest

classify = runpy.run_path(str(Path(__file__).with_name("check-dependency-vulnerabilities.py")))["classify"]


class VulnerabilityTests(unittest.TestCase):
    def test_no_advisories(self):
        result = classify(["example"], [{"commit": "abc"}], {"results": [{}]})
        self.assertEqual(result[0]["advisories"], [])
        self.assertEqual(result[0]["query"], {"commit": "abc"})

    def test_advisories_are_retained(self):
        result = classify(["example"], [{}], {"results": [{"vulns": [{"id": "GHSA-example"}, {"id": "CVE-example"}]}]})
        self.assertEqual(result[0]["advisories"], ["CVE-example", "GHSA-example"])

    def test_incomplete_response(self):
        with self.assertRaisesRegex(ValueError, "Incomplete"):
            classify(["example"], [{}], {"results": []})

    def test_pagination_is_not_clean(self):
        with self.assertRaisesRegex(ValueError, "incomplete paginated"):
            classify(["example"], [{}], {"results": [{"next_page_token": "more"}]})

    def test_query_error_is_not_clean(self):
        with self.assertRaisesRegex(ValueError, "query failed"):
            classify(["example"], [{}], {"results": [{"error": "unavailable"}]})


if __name__ == "__main__":
    unittest.main()

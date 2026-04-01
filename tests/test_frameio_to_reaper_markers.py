import csv
import importlib.util
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "tools" / "frameio_to_reaper_markers.py"
FIXTURES = ROOT / "tests" / "fixtures"


def load_module():
    spec = importlib.util.spec_from_file_location("frameio_to_reaper_markers", MODULE_PATH)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class FrameIoToReaperMarkersTests(unittest.TestCase):
    def test_convert_rows_uses_comment_and_timecode_columns(self):
        module = load_module()
        rows = module.read_frameio_rows(FIXTURES / "frameio_sample.csv")

        converted = module.convert_rows(rows)

        self.assertEqual(
            converted,
            [
                {"#": "M1", "Name": "note test 1 2", "Start": "00:15:44:18", "End": "", "Length": ""},
                {"#": "M2", "Name": "move this", "Start": "01:25:28:10", "End": "", "Length": ""},
                {"#": "M3", "Name": "blah", "Start": "01:29:43:13", "End": "", "Length": ""},
            ],
        )

    def test_cli_writes_expected_reaper_csv(self):
        expected = (FIXTURES / "reaper_expected.csv").read_text(encoding="utf-8")

        with tempfile.TemporaryDirectory() as temp_dir:
            output_path = Path(temp_dir) / "markers.csv"
            completed = subprocess.run(
                [
                    sys.executable,
                    str(MODULE_PATH),
                    str(FIXTURES / "frameio_sample.csv"),
                    str(output_path),
                ],
                cwd=ROOT,
                capture_output=True,
                text=True,
            )

            self.assertEqual(completed.returncode, 0, msg=completed.stderr)
            self.assertEqual(output_path.read_text(encoding="utf-8"), expected)

    def test_cli_rejects_missing_required_columns(self):
        module = load_module()

        with tempfile.TemporaryDirectory() as temp_dir:
            bad_csv = Path(temp_dir) / "bad.csv"
            with bad_csv.open("w", newline="", encoding="utf-8") as handle:
                writer = csv.DictWriter(handle, fieldnames=["Comment"])
                writer.writeheader()
                writer.writerow({"Comment": "missing timecode"})

            with self.assertRaisesRegex(ValueError, "Timecode"):
                module.read_frameio_rows(bad_csv)


if __name__ == "__main__":
    unittest.main()

import importlib.util
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "tools" / "reaper_marker_protocol.py"


def load_module():
    spec = importlib.util.spec_from_file_location("reaper_marker_protocol", MODULE_PATH)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class ReaperMarkerProtocolTests(unittest.TestCase):
    def test_exposes_subscription_command(self):
        module = load_module()

        self.assertEqual(module.SUBSCRIBE_MARKERS_COMMAND, "RS_SUBSCRIBE_MARKERS")

    def test_formats_marker_event_line(self):
        module = load_module()

        payload = '[{"guid":"abc","name":"Song 1"}]'

        self.assertEqual(module.format_marker_event_line(payload), f"EVENT MARKERS {payload}")


if __name__ == "__main__":
    unittest.main()

"""Do not turn truncated probe events into successful calibration input."""

import json
import unittest
from pathlib import Path
from unittest.mock import Mock

from .trace import load_chrome_trace_events


class TraceRepairTests(unittest.TestCase):
    def test_only_missing_footer_is_repaired(self) -> None:
        for prefix, footer in [(' {"traceEvents":[', "]}"), ("[", "]")]:
            for suffix, loaded in [
                (footer, True),
                ("", True),
                (',{"name":', False),
                (",garbage", False),
                (',{"args":{}', False),
            ]:
                with self.subTest(prefix=prefix, suffix=suffix):
                    path = Mock(spec=Path)
                    path.read_text.return_value = prefix + '{"name":"complete"}' + suffix
                    if loaded:
                        self.assertEqual(load_chrome_trace_events(path), [{"name": "complete"}])
                    else:
                        with self.assertRaises(json.JSONDecodeError):
                            load_chrome_trace_events(path)


if __name__ == "__main__":
    unittest.main()

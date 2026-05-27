from __future__ import annotations

import json
import tempfile
import unittest
from contextlib import redirect_stdout
from io import StringIO
from pathlib import Path

from livox_mid360_diagnostics.autoconfig import (
    is_mid360_config_file,
    parse_selection,
    print_config_table,
    read_config_lidar_ip,
    update_config,
)


class ConfigIpUpdateTest(unittest.TestCase):
    def test_ros_driver_config_lidar_ip(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "MID360_config.json"
            path.write_text(json.dumps({"lidar_configs": [{"ip": "192.168.1.12"}]}), encoding="utf-8")

            self.assertEqual(read_config_lidar_ip(path), "192.168.1.12")
            update_config(path, "192.168.1.122", "192.168.1.5")

            self.assertEqual(read_config_lidar_ip(path), "192.168.1.122")
            self.assertEqual(json.loads(path.read_text(encoding="utf-8"))["lidar_configs"][0]["ip"], "192.168.1.122")

    def test_sdk2_config_lidar_ip(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "mid360_config.json"
            path.write_text(
                json.dumps(
                    {
                        "MID360": {
                            "host_net_info": [
                                {
                                    "lidar_ip": ["192.168.1.12"],
                                    "host_ip": "192.168.1.10",
                                    "multicast_ip": "224.1.1.5",
                                }
                            ]
                        }
                    }
                ),
                encoding="utf-8",
            )

            self.assertEqual(read_config_lidar_ip(path), "192.168.1.12")
            update_config(path, "192.168.1.122", "192.168.1.5")

            data = json.loads(path.read_text(encoding="utf-8"))
            self.assertEqual(read_config_lidar_ip(path), "192.168.1.122")
            self.assertEqual(data["MID360"]["host_net_info"][0]["lidar_ip"][0], "192.168.1.122")
            self.assertEqual(data["MID360"]["host_net_info"][0]["host_ip"], "192.168.1.5")
            self.assertNotIn("multicast_ip", data["MID360"]["host_net_info"][0])

    def test_mid360s_config_lidar_ip(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "MID360s_config.json"
            path.write_text(
                json.dumps({"Mid360s": {"host_net_info": [{"lidar_ip": ["192.168.1.12"], "host_ip": "192.168.1.10"}]}}),
                encoding="utf-8",
            )

            self.assertEqual(read_config_lidar_ip(path), "192.168.1.12")
            update_config(path, "192.168.1.122", "192.168.1.5")

            data = json.loads(path.read_text(encoding="utf-8"))
            self.assertEqual(data["Mid360s"]["host_net_info"][0]["lidar_ip"][0], "192.168.1.122")
            self.assertEqual(data["Mid360s"]["host_net_info"][0]["host_ip"], "192.168.1.5")

    def test_selection_parser(self) -> None:
        self.assertEqual(parse_selection("", 4), [])
        self.assertEqual(parse_selection("1, 3 4", 4), [0, 2, 3])
        self.assertEqual(parse_selection("1-3", 4), [0, 1, 2])
        self.assertEqual(parse_selection("all", 3), [0, 1, 2])

    def test_config_table_omits_empty_input_when_filtered_by_caller(self) -> None:
        output = StringIO()
        states = [(Path("/tmp/MID360_config.json"), "192.168.1.12", "mismatch")]
        with redirect_stdout(output):
            print_config_table(states)

        text = output.getvalue()
        self.assertIn("current=192.168.1.12", text)
        self.assertNotIn("current=N/A", text)

    def test_mid360_config_detection(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            good = Path(tmp) / "mixed_HAP_MID360_config.json"
            bad = Path(tmp) / "other.json"
            good.write_text(json.dumps({"MID360": {"host_net_info": {}}}), encoding="utf-8")
            bad.write_text(json.dumps({"foo": "bar"}), encoding="utf-8")

            self.assertTrue(is_mid360_config_file(good))
            self.assertFalse(is_mid360_config_file(bad))


if __name__ == "__main__":
    unittest.main()

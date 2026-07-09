#!/usr/bin/env python3
"""Unit tests for buddy_sensecraft_bridge.normalize_config (stdlib unittest).

Run:  cd host && python3 -m unittest test_sensecraft_config -v
"""
from __future__ import annotations

import unittest

from buddy_sensecraft_bridge import DEFAULT_API_BASE, normalize_config


class TestNormalizeConfig(unittest.TestCase):
    def test_env_overrides_everything(self):
        cfg = normalize_config({"devices": [{"eui": "AAA", "key": "k"}]},
                               {"SENSECRAFT_DEVICE_EUI": "EEE",
                                "SENSECRAFT_DEVICE_KEY": "KKK"})
        self.assertEqual(cfg["devices"], [{"eui": "EEE", "key": "KKK"}])

    def test_devices_list(self):
        cfg = normalize_config(
            {"devices": [{"eui": "A", "key": "ka"}, {"eui": "B", "key": "kb"}]}, {})
        self.assertEqual([d["eui"] for d in cfg["devices"]], ["A", "B"])
        self.assertEqual([d["key"] for d in cfg["devices"]], ["ka", "kb"])

    def test_legacy_single_device(self):
        cfg = normalize_config({"eui": "A", "key": "ka"}, {})
        self.assertEqual(cfg["devices"], [{"eui": "A", "key": "ka"}])

    def test_defaults(self):
        cfg = normalize_config({"eui": "A", "key": "k"}, {})
        self.assertEqual(cfg["api_base"], DEFAULT_API_BASE)
        self.assertEqual(cfg["interval_s"], 10)

    def test_api_base_env_and_trailing_slash(self):
        cfg = normalize_config({"eui": "A", "key": "k"},
                               {"SENSECRAFT_API_BASE": "https://x.example/"})
        self.assertEqual(cfg["api_base"], "https://x.example")

    def test_missing_creds_raises(self):
        with self.assertRaises(SystemExit):
            normalize_config({}, {})

    def test_incomplete_device_raises(self):
        with self.assertRaises(SystemExit):
            normalize_config({"devices": [{"eui": "A"}]}, {})


if __name__ == "__main__":
    unittest.main()

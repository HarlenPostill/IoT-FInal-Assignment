"""Payload and control checks for the actuator's mock source."""
import importlib.util
import os
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('mock_microphone', ROOT / 'tools/mock_microphone/server.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class MockMicrophoneTest(unittest.TestCase):
    def setUp(self):
        os.environ['LAB_GROUP_ID'] = 'test-group'
        self.sim = module.Simulator()

    def test_source_identity_and_freshness(self):
        first, second = self.sim.payload(), self.sim.payload()
        self.assertRegex(first['boot_id'], r'^[0-9a-f]{16}$')
        self.assertEqual(first['boot_id'], second['boot_id'])
        self.assertEqual(second['sequence'], first['sequence']+1)
        self.assertEqual(first['device'], self.sim.device)
        self.assertEqual(first['location'], 'building_exterior')
        self.assertEqual(first['sample_age_ms'], 0)
        self.assertTrue(first['sensor_ok'])
        self.assertTrue(first['simulated'])
        self.assertIsInstance(first['estimated_spl_db'], float)
        self.assertFalse(self.sim.status()['enabled'])

    def test_fault_omits_sound_and_recovers(self):
        self.sim.control({'fault': True})
        fault = self.sim.payload()
        self.assertFalse(fault['sensor_ok'])
        self.assertNotIn('estimated_spl_db', fault)
        self.assertNotIn('dbfs', fault)
        self.sim.control({'fault': False, 'level': 100})
        self.assertEqual(self.sim.payload()['estimated_spl_db'], 100.0)

    def test_invalid_controls_do_not_start_publisher(self):
        for value in (float('nan'), float('inf'), 121, 29, True, '100', None):
            with self.assertRaises(ValueError):
                self.sim.control({'enabled': True, 'level': value})
            self.assertFalse(self.sim.enabled)
        for data in ({'enabled': 1}, {'fault': 'false'}, {'unexpected': True}, []):
            with self.assertRaises(ValueError):
                self.sim.control(data)


if __name__ == '__main__':
    unittest.main()

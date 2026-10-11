import copy
import json
from pathlib import Path
import unittest
from pose_trial import validate


class PlanValidation(unittest.TestCase):
    def test_shipped_plans(self):
        for path in (Path(__file__).parent / 'plans').glob('*.json'):
            validate(json.loads(path.read_text()))

    def test_reject_unsafe_or_ambiguous_input_before_adb(self):
        base = {'reason': 'test', 'seconds': 1, 'head': [0, 0, 1.5, 0, 0, 0, 1]}
        for change in ({'seconds': float('nan')}, {'seconds': 121}, {'reason': ''},
                       {'head': [0]*7}, {'head': [0,0,0,0,0,0,float('inf')]},
                       {'touch': [1,960,540]}, {'axes': [0,0,0,0,-1,0]},
                       {'buttons': -1}, {'hold_ms': 2001}, {'headset': []}):
            with self.subTest(change=change), self.assertRaises(ValueError):
                validate([{**copy.deepcopy(base), **change}])
        with self.assertRaises(ValueError):
            validate([{**base, 'seconds': 120}]*3)
        with self.assertRaises(ValueError):
            validate([])


if __name__ == '__main__':
    unittest.main()

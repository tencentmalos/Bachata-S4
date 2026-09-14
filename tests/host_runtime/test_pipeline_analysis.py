#!/usr/bin/env python3
import importlib.util
from pathlib import Path
import unittest
import sys

path = Path(__file__).resolve().parents[2] / 'scripts/analysis/analyze_pipeline_handoff.py'
spec = importlib.util.spec_from_file_location('handoff', path)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
sys.path.insert(0, str(path.parent))
from export_pipeline_timeline import export


def raw(events):
    return dict(schema='shadps4.pipeline-handoff.v1', clock='steady_ns', generation=1, pid=2,
                capture_id=3, run_uuid='run', start_ns=0, deadline_ns=100, complete=True,
                stop_reason='duration', columns=['time_ns','thread','object','token','value','kind','name','wait'], events=events)


def scope(a,b,tid,token,name='work',wait=False):
    return [[a,tid,0,token,0,'begin',name,wait], [b,tid,0,token,0,'end',name,wait]]


class Tests(unittest.TestCase):
    def test_projection_preserves_crossing_endpoints_and_nested_scopes(self):
        events = scope(1, 90, 1, 1) + scope(15, 30, 1, 2)
        events += [[t, 1, 0, t, 0, 'host_present', '', False] for t in (10, 50, 80)]
        chart, receipt = export(raw(events), dict(path='trace', sha256='a'*64), count=1)
        self.assertEqual(chart['window'], dict(start=10/1e6, end=50/1e6))
        self.assertEqual(chart['intervals'][0]['start'], 1/1e6)
        self.assertEqual(chart['intervals'][0]['end'], 90/1e6)
        self.assertEqual(set(receipt['retained_scope_tokens']), {1, 2})

    def test_projection_does_not_invent_missing_present_boundaries(self):
        with self.assertRaises(ValueError):
            export(raw(scope(10,20,1,1)), dict(path='trace', sha256='a'*64), count=1)

    def test_nested_physical_union_and_wait_subtraction(self):
        r=module.analyze(raw(scope(10,90,1,1)+scope(20,80,1,2)+scope(20,40,1,3,'wait',True)+scope(30,60,2,4)))
        self.assertEqual(r['physical_threads']['1']['observed_work_ms'],60/1e6)
        self.assertEqual(r['observed_work_lane_histogram_ms'],{'0':30/1e6,'1':50/1e6,'2':20/1e6})

    def test_touching_intervals_are_not_parallel(self):
        r=module.analyze(raw(scope(0,50,1,1)+scope(50,99,2,2)))
        self.assertNotIn('2',r['observed_work_lane_histogram_ms'])

    def test_scope_cross_thread_rejected(self):
        events=scope(10,20,1,1); events[1][1]=2
        with self.assertRaises(ValueError): module.analyze(raw(events))

    def test_duplicate_rejected(self):
        with self.assertRaises(ValueError): module.analyze(raw(scope(10,20,1,1)*2))

    def test_capacity_is_not_complete(self):
        r=raw([]); r['stop_reason']='capacity'
        with self.assertRaises(ValueError): module.analyze(r)

    def test_nearest_timestamp_is_not_a_join(self):
        r=module.analyze(raw([[10,1,0,1,0,'present_enqueue','',False],
                              [11,2,0,2,0,'present_dequeue','',False]]))
        self.assertEqual(r['joins']['present_queue_delay']['statistics']['count'],0)
        self.assertEqual(r['unknown']['present_queue_delay_unmatched'],2)

    def test_duplicate_handoff_rejected(self):
        with self.assertRaises(ValueError): module.analyze(raw([[10,1,0,1,0,'present_enqueue','',False]]*2))

    def test_wait_only_is_not_work(self):
        r=module.analyze(raw(scope(10,90,1,1,'wait',True)))
        self.assertEqual(r['physical_threads']['1']['observed_work_ms'],0)

    def test_boundary_scope_is_explicit_unknown(self):
        r=module.analyze(raw(scope(10,90,1,1)[:1]))
        self.assertEqual(r['unknown']['boundary_unpaired_scopes'],1)

    def test_cross_capture_identity_required(self):
        r=raw([]);r['run_uuid']=''
        with self.assertRaises(ValueError): module.analyze(r)


if __name__ == '__main__': unittest.main()

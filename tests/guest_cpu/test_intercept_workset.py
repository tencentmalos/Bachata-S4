import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[2]
S=importlib.util.spec_from_file_location('intercepts',ROOT/'tools/guest-functions/intercept-workset.py')
M=importlib.util.module_from_spec(S);S.loader.exec_module(M)

class InterceptWorksetTests(unittest.TestCase):
    def setUp(self):
        self.t=tempfile.TemporaryDirectory();self.addCleanup(self.t.cleanup);self.root=Path(self.t.name)
        ident=dict(title='TEST',module='eboot.bin',module_sha256='a'*64)
        analysis=dict(module='eboot.bin',sha256='b'*64,imagebase=0)
        semantic=dict(index_sha256='c'*64)
        self.w=dict(identity=ident,analysis=analysis,semantic_symbols=semantic,trace_sha256='d'*64,
                    root_offset=16,semantic_name='MeasuredRoot',unresolved_indirect_calls=[dict(callsite=32,calls=3)],
                    functions=[dict(offset=16,semantic_name='MeasuredRoot')])
        request=dict(identity=ident,analysis=analysis,architecture='x86_64',abi='x86_64-sysv',endianness='little',pointer_bits=64,
                     capture_evidence=[dict(trace_sha256='d'*64)])
        self.e=dict(request=request,semantic_symbols=semantic,functions=[dict(offset=16,semantic_name='MeasuredRoot',
            bytes=dict(hex='9090909090c3',returned=6),info=dict(size=6),interiorEntries=[],
            decompilerFacts=dict(instructions=[dict(ea=hex(i),size=1) for i in range(16,22)]))])
        self.base=dict(**ident,analysis=analysis,abi='x86_64-sysv',hooks=[],sources=[],counters=[])
    def generate(self,select=None):
        for name,obj in [('work',self.w),('facts',self.e),('base',self.base)]:
            (self.root/name).write_text(json.dumps(obj))
        return M.generate(self.root/'work',self.root/'facts',self.root/'base',self.root/'out',select)
    def test_visible_entry_has_editable_observer_and_unresolved_is_retained(self):
        r=self.generate();self.assertEqual(len(r['functions']),1)
        self.assertEqual(r['functions'][0]['mode'],'entry-observer-x86_64-avx')
        self.assertEqual(r['functions'][0]['runtime_status'],'not-tested')
        self.assertTrue((self.root/'out/MeasuredRoot.cpp').exists())
        self.assertEqual(r['unresolved_indirect_calls'],self.w['unresolved_indirect_calls'])
        h=json.loads((self.root/'out/frame_intercepts.recipe.json').read_text())['hooks'][0]
        self.assertEqual(h['prototype'],'opaque-machine-entry')
    def test_no_overwrite_of_edited_sources(self):
        self.generate()
        with self.assertRaisesRegex(ValueError,'fresh'):self.generate()
    def test_runtime_identity_mismatch_rejected(self):
        self.base['module_sha256']='e'*64
        with self.assertRaisesRegex(ValueError,'identity'):self.generate()
    def test_other_cpu_has_no_accidental_x86_adapter(self):
        self.e['request']['architecture']='aarch64'
        with self.assertRaisesRegex(ValueError,'adapter'):self.generate()
    def test_interior_entry_in_stolen_bytes_rejected(self):
        self.e['functions'][0]['interiorEntries']=[dict(to='0x12')]
        with self.assertRaisesRegex(ValueError,'stolen'):self.generate()
    def test_unknown_selection_rejected(self):
        with self.assertRaisesRegex(ValueError,'selected'):self.generate(['Absent'])
    def test_truncated_facts_rejected(self):
        self.e['functions'][0]['bytes']['returned']=5
        with self.assertRaisesRegex(ValueError,'truncated'):self.generate()
    def test_different_capture_rejected(self):
        self.w['trace_sha256']='e'*64
        with self.assertRaisesRegex(ValueError,'capture'):self.generate()

if __name__=='__main__':unittest.main()

#!/usr/bin/env python3
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[2]
SPEC=importlib.util.spec_from_file_location('guest_builder',ROOT/'tools/guest-functions/build.py')
BUILDER=importlib.util.module_from_spec(SPEC);SPEC.loader.exec_module(BUILDER)

class GuestBuilderTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.clang=Path(os.environ['GUEST_CLANG'])
        cls.tmp=tempfile.TemporaryDirectory();cls.root=Path(cls.tmp.name)
        subprocess.run(['python3',str(ROOT/'tests/guest_cpu/patch/make_fixture.py'),'--clang',str(cls.clang),
                        '--out',str(cls.root/'fixture')],check=True,stdout=subprocess.PIPE)
        cls.recipe=json.loads((cls.root/'fixture/recipe.json').read_text())
    @classmethod
    def tearDownClass(cls):cls.tmp.cleanup()
    def reject_source(self,source):
        d=self.root/self._testMethodName;d.mkdir();(d/'bad.cpp').write_text(source)
        recipe=dict(self.recipe);recipe['sources']=[str(d/'bad.cpp')]
        recipe['hooks']=[recipe['hooks'][0]];recipe['exports']=[]
        (d/'recipe.json').write_text(json.dumps(recipe))
        (d/'output').mkdir();(d/'output/patch.json').write_text('old PASS');(d/'output/build.json').write_text('old PASS')
        with self.assertRaises((ValueError,subprocess.CalledProcessError)):
            BUILDER.build(d/'recipe.json',self.clang,d/'output')
        self.assertFalse((d/'output/patch.json').exists());self.assertFalse((d/'output/build.json').exists())
    def test_mixed_sources_data_rebase_and_symbols(self):
        p=json.loads((self.root/'fixture/package/patch.json').read_text())
        self.assertEqual(len(p['segments']),2);self.assertTrue(p['rebase64'])
        self.assertIn('patch_varargs',p['exports']);self.assertTrue(p['segments'][1]['hex'])
    def test_source_dependency_hashes(self):
        b=json.loads((self.root/'fixture/package/build.json').read_text())
        for name in ('abi.h','shad_guest.h','runtime.c','helpers.S'):
            self.assertTrue(any(p.endswith('/'+name) for p in b['sources']))
    def test_duplicate_json(self):
        with self.assertRaises(ValueError):json.loads('{"abi":1,"abi":2}',object_pairs_hook=BUILDER.unique)
    def test_tls_rejected_without_stale_pass(self):
        self.reject_source('__thread U64 value; extern "C" U64 patch_sum8(U64,U64,U64,U64,U64,U64,U64,U64){return ++value;}')
    def test_external_runtime_rejected_without_stale_pass(self):
        self.reject_source('extern "C" void* malloc(__SIZE_TYPE__); extern "C" U64 patch_sum8(U64,U64,U64,U64,U64,U64,U64,U64){return (U64)malloc(32);}')
    def test_cpp_constructor_rejected_without_stale_pass(self):
        self.reject_source('volatile U64 x; struct X {X(){x=1;}}; X obj; extern "C" U64 patch_sum8(U64,U64,U64,U64,U64,U64,U64,U64){return x;}')
    def test_invalid_original_symbol(self):
        with self.assertRaises(ValueError):BUILDER.identifier('original; system("x")')
    def test_bound_data_is_const_when_read_only(self):
        self.reject_source('extern "C" U64 patch_sum8(U64,U64,U64,U64,U64,U64,U64,U64){*guest_bias=1;return 0;}')
    def test_binding_names_and_slots_are_distinct(self):
        p=json.loads((self.root/'fixture/package/patch.json').read_text())
        self.assertEqual({x['kind'] for x in p['bindings']},{'function','data'})
        slots=[x['slot'] for x in p['imports']]
        self.assertEqual(len(slots),len(set(slots)))
        header=(self.root/'fixture/package/shad_imports.h').read_text()
        self.assertIn('sizeof(U64) == 8',header)
    def test_wrong_data_extent_is_compile_error(self):
        d=self.root/self._testMethodName;d.mkdir()
        recipe=json.loads(json.dumps(self.recipe));recipe['bindings'][1]['size']=16
        path=d/'recipe.json';path.write_text(json.dumps(recipe))
        with self.assertRaises(subprocess.CalledProcessError):BUILDER.build(path,self.clang,d/'output')
        self.assertFalse((d/'output/patch.json').exists())
    def test_binding_import_alias_refused(self):
        d=self.root/self._testMethodName;d.mkdir()
        recipe=json.loads(json.dumps(self.recipe));recipe['bindings'][0]['name']='original_sum8'
        path=d/'recipe.json';path.write_text(json.dumps(recipe))
        with self.assertRaises(ValueError):BUILDER.build(path,self.clang,d/'output')
        self.assertFalse((d/'output/patch.json').exists())

if __name__=='__main__':unittest.main()

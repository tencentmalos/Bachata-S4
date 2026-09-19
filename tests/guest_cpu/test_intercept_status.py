import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[2]
S=importlib.util.spec_from_file_location('status',ROOT/'tools/guest-functions/intercept-status.py')
M=importlib.util.module_from_spec(S);S.loader.exec_module(M)

class InterceptStatusTests(unittest.TestCase):
    def setUp(self):
        self.t=tempfile.TemporaryDirectory();self.addCleanup(self.t.cleanup);self.root=Path(self.t.name).resolve()
        self.source=self.root/'Source.cpp';self.source.write_text('// code\n')
        self.recipe=self.root/'recipe.json';self.recipe.write_text('{}')
        self.index=self.root/'index.json';self.build=self.root/'build.json';self.status=self.root/'status.txt'
        self.index.write_text(json.dumps(dict(recipe='recipe.json',recipe_sha256=M.sha(self.recipe),functions=[
            dict(name='Entry',source='Source.cpp',source_sha256=M.sha(self.source),hook_name='Entry',counter_ids=[1],mode='entry-observer-x86_64-avx')])) )
        self.build.write_text(json.dumps(dict(package_sha256='a'*64,sources={str(self.source):M.sha(self.source),str(self.recipe):M.sha(self.recipe)})))
        self.status.write_text('sha256: '+'a'*64+'\ninstalled: 1\nfailed: 0\ncontext: 5\nlast_owner: 5:1:2:3\nhook: Entry entry=0x10 enabled=1\ncounter: 1 GuestPatch.Unit.Entry_entries samples=1 last=1\n')
    def run_update(self):return M.update(self.index,self.build,self.status)
    def test_actual_hit_and_html_refresh(self):
        self.assertEqual(self.run_update()['hit'],1)
        self.assertIn('hit',(self.root/'index.html').read_text())
    def test_installed_without_counter_is_not_hit(self):
        self.status.write_text(self.status.read_text().replace('samples=1 last=1','samples=0 last=0'))
        self.assertEqual(self.run_update()['installed_not_hit'],1)
    def test_different_package_rejected(self):
        self.status.write_text(self.status.read_text().replace('a'*64,'b'*64))
        with self.assertRaisesRegex(ValueError,'package'):self.run_update()
    def test_edited_source_rejected(self):
        self.source.write_text('// edited\n')
        with self.assertRaisesRegex(ValueError,'source changed'):self.run_update()
    def test_rebuilt_deployed_source_refreshes_index_without_manual_hashes(self):
        self.source.write_text('// rebuilt code\n')
        build=json.loads(self.build.read_text());build['sources'][str(self.source)]=M.sha(self.source)
        build['package_sha256']='b'*64;self.build.write_text(json.dumps(build))
        self.status.write_text(self.status.read_text().replace('a'*64,'b'*64))
        self.assertEqual(self.run_update()['hit'],1)
        self.assertEqual(json.loads(self.index.read_text())['functions'][0]['source_sha256'],M.sha(self.source))
    def test_no_service_is_not_acceptance(self):
        self.status.write_text('No services match: app')
        with self.assertRaisesRegex(ValueError,'missing'):self.run_update()

if __name__=='__main__':unittest.main()

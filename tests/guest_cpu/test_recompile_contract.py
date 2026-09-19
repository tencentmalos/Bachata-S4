#!/usr/bin/env python3
import hashlib,importlib.util,json,tempfile,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
S=importlib.util.spec_from_file_location('contract',ROOT/'tools/guest-functions/recompile_contract.py');M=importlib.util.module_from_spec(S);S.loader.exec_module(M)
class ContractTests(unittest.TestCase):
 def setUp(self):
  self.temp=tempfile.TemporaryDirectory();self.root=Path(self.temp.name)
  d=ROOT/'guest/games/CUSA50828/01.08'
  self.recipe=json.loads((d/'rooftop_recompiled.recipe.json').read_text())
  self.contract=json.loads((d/'rooftop-recompile.contract.json').read_text())
  for n in self.contract['source_sha256']:(self.root/n).write_bytes((d/n).read_bytes())
  e=(d/self.contract['evidence']['path']).read_bytes();(self.root/'evidence.json').write_bytes(e);self.contract['evidence']['path']='evidence.json'
  self.path=self.root/'recipe.json'
 def tearDown(self):self.temp.cleanup()
 def check(self):
  (self.root/'rooftop-recompile.contract.json').write_text(json.dumps(self.contract))
  return M.verify(self.path,self.recipe)
 def test_actual_reviewed_contract(self):self.assertEqual(len(self.check()[0]['replacements']),4)
 def test_unknown_abi(self):
  self.contract['functions'][0]['review']['abi']='unreviewed'
  with self.assertRaises(ValueError):self.check()
 def test_source_change(self):
  (self.root/'rooftop_recompiled.cpp').write_text('modified')
  with self.assertRaises(ValueError):self.check()
 def test_wrong_evidence(self):
  (self.root/'evidence.json').write_text('{}')
  with self.assertRaises(ValueError):self.check()
 def test_wrong_module(self):
  self.recipe['module_sha256']='0'*64
  with self.assertRaises(ValueError):self.check()
 def test_wrong_original_bytes(self):
  self.recipe['hooks'][5]['expected']='00'*32
  with self.assertRaises(ValueError):self.check()
 def test_changed_prototype(self):
  self.contract['functions'][0]['prototype']='void original_write_label(void)'
  with self.assertRaises(ValueError):self.check()
 def test_duplicate_function(self):
  self.contract['functions'].append(self.contract['functions'][0])
  with self.assertRaises(ValueError):self.check()
if __name__=='__main__':unittest.main()

#!/usr/bin/env python3
import importlib.machinery
import hashlib
import json
from pathlib import Path
import unittest
m=importlib.machinery.SourceFileLoader('guest_auto_tag',str(Path(__file__).with_name('guest-auto-tag'))).load_module()
class Checks(unittest.TestCase):
 def test_empty_service_label_preserves_first_field(self):
  v=m.fields('  Client:\n    package: test\n    context: 33\n')
  self.assertEqual(v,dict(Client='',package='test',context='33'))
 def test_capture_fields_preserve_equals_and_values(self):
  v=m.fields('  Client:\n    capture_active=1\n    capture_path=/data/a b.prof\n')
  self.assertEqual(v['capture_active'],'1');self.assertEqual(v['capture_path'],'/data/a b.prof')
 def test_bindings_require_current_package_and_exact_hooks(self):
  recipe=dict(id='p',title='t',module='m',module_sha256='a'*64,abi='x86_64-sysv',counters=[],hooks=[dict(name='frame',offset=3,expected='90')])
  b=json.dumps(recipe).encode();status=dict(package='p',enabled='1',failed='0',context='33',sha256=hashlib.sha256(b).hexdigest());profile=dict(identity=dict(module_sha256='a'*64))
  self.assertEqual(m.recipe_bindings(recipe,b,status,profile,33)[0]['offset'],3)
  wrong=dict(recipe,hooks=[dict(name='frame',offset=4,expected='90')])
  with self.assertRaises(ValueError):m.recipe_bindings(wrong,b,status,profile,33)
  with self.assertRaises(ValueError):m.recipe_bindings(recipe,b+b' ',status,profile,33)
  with self.assertRaises(ValueError):m.recipe_bindings(recipe,b,status,profile,34)
if __name__=='__main__':unittest.main()

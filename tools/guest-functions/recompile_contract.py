"""Admission consistency checks for reviewed selective guest recompiles.
This checks evidence/source/ABI identity, not semantic equivalence by assertion.
Runtime and differential replay acceptance remain separate recorded evidence.
"""
import hashlib
import json
from pathlib import Path


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def verify(recipe_path, recipe):
    if 'recompile_contract' not in recipe:
        return None, set()
    root = recipe_path.parent
    path = (root / recipe['recompile_contract']).resolve()
    root = path.parent
    contract = json.loads(path.read_text())
    if contract.get('schema') != 'spatial.guest-recompile.contract.v1' or contract.get('abi') != recipe['abi']:
        raise ValueError('recompile contract schema/ABI mismatch')
    for key in ('title', 'module', 'module_sha256', 'executable_sha256'):
        if contract['identity'].get(key) != recipe.get(key):
            raise ValueError('recompile contract identity mismatch: ' + key)
    evidence_path = (root / contract['evidence']['path']).resolve()
    if digest(evidence_path) != contract['evidence']['sha256']:
        raise ValueError('recompile evidence SHA mismatch')
    evidence = json.loads(evidence_path.read_text())
    if evidence.get('schema') not in ('spatial.guest-recompile.evidence.v1','spatial.guest-recompile.evidence.v2') or evidence['request']['abi'] != recipe['abi']:
        raise ValueError('wrong recompile evidence schema/ABI')
    if evidence['request']['identity']['module_sha256'] != recipe['module_sha256']:
        raise ValueError('recompile analysis/runtime binding mismatch')
    dependencies = {path, evidence_path}
    if not contract.get('source_sha256'):
        raise ValueError('missing reviewed source hashes')
    for name, expected in contract['source_sha256'].items():
        source = (root / name).resolve()
        if digest(source) != expected:
            raise ValueError('reviewed recompile source changed: ' + name)
        dependencies.add(source)
    hooks = {h['replacement']: h for h in recipe['hooks']}
    functions = {f['offset']: f for f in evidence['functions']}
    admitted = set()
    if not contract.get('functions'):
        raise ValueError('empty recompile contract')
    for item in contract['functions']:
        h = hooks[item['replacement']]
        if item['replacement'] in admitted or item['offset'] != h['offset'] or item['prototype'] != h['prototype']:
            raise ValueError('recompile function contract mismatch')
        admitted.add(item['replacement'])
        f = functions[item['offset']]
        if not f['bytes']['hex'].startswith(h['expected']) or f['bytes']['returned'] != f['info']['size']:
            raise ValueError('recompile original bytes mismatch')
        if not item.get('effects') or not item.get('dependencies') or not item.get('valid_domain'):
            raise ValueError('incomplete recompile behavior contract')
        for field in ('abi', 'memory_layout', 'indirect_calls', 'integer_fp', 'relocations_unwind'):
            if item['review'].get(field) != 'reviewed':
                raise ValueError('unreviewed recompile contract: ' + field)
    return dict(contract_sha256=digest(path), evidence_sha256=digest(evidence_path),
                replacements=sorted(admitted), status='reviewed-source-identity-checked'), dependencies

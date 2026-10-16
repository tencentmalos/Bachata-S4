"""Check reference overrides against supplied ORIGINAL UE headers; not a C++ build.

No header downloads or generated UE substitutes. Reports exact local file hashes.
"""
import argparse
import hashlib
import json
import re
from pathlib import Path


def strip_comments(text):
    return re.sub(r"//[^\n]*|/\*.*?\*/", lambda m: "\n" * m[0].count("\n"), text, flags=re.S)


def canonical_type(text):
    return re.sub(r"\s+", "", text)


def param_types(params):
    result = []
    for param in params.split(","):
        param = param.split("=", 1)[0].strip()
        if param and param != "void":
            # These selected public methods have named, non-template parameters.
            match = re.fullmatch(r"(.+?)[\s*&]+([A-Za-z_]\w*)", param)
            if not match:
                raise ValueError(f"Unsupported parameter syntax: {param}")
            result.append(canonical_type(param[:match.start(2)]))
    return result


def methods(text, overrides=False):
    suffix = r"override\b" if overrides else r"(?:=\s*0\s*;|[;{])"
    pattern = r"virtual\s+([\w\s:*&]+?)\s+(\w+)\s*\(([^()]*)\)\s*" + suffix
    for match in re.finditer(pattern, strip_comments(text)):
        if match[2] in {"RHISetComputeShader", "RHISetShaderParameter", "RHISetShaderUniformBuffer",
                        "RHIDispatchComputeShader", "RHIDispatchIndirectComputeShader", "GetName",
                        "RHIGetDefaultContext", "RHICreateUniformBuffer", "RHILockVertexBuffer",
                        "RHIUnlockVertexBuffer", "RHISetBoundShaderState", "RHISetStreamSource",
                        "RHIDrawPrimitive", "RHIDrawIndexedPrimitive", "RHISetShaderTexture",
                        "RHISetShaderSampler", "RHISetShaderResourceViewParameter"}:
            yield match[2], canonical_type(match[1]), param_types(match[3])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ue-runtime-root", type=Path, required=True)
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parent
    reference = (root / "GnmRHIReference.h").read_text(encoding="utf8")
    sources = {}
    for header in ("RHIContext.h", "DynamicRHI.h"):
        path = args.ue_runtime_root / "RHI/Public" / header
        raw = path.read_bytes()
        sources[header] = dict(path=str(path.resolve()), sha256=hashlib.sha256(raw).hexdigest(),
                               methods=list(methods(raw.decode("utf8"))))
    verified = []
    for name, result, params in methods(reference, overrides=True):
        expected = (name, result, params)
        owner = "DynamicRHI.h" if name in {
            "GetName", "RHIGetDefaultContext", "RHICreateUniformBuffer",
            "RHILockVertexBuffer", "RHIUnlockVertexBuffer"} else "RHIContext.h"
        if expected not in sources[owner]["methods"]:
            raise SystemExit(f"Interface mismatch in {owner}: {expected}")
        verified.append(dict(method=name, return_type=result, parameter_types=params, header=owner))
    if len(verified) != 29:
        raise SystemExit(f"Expected 29 checked overrides, parsed {len(verified)}")
    report = dict(check="public-signature-comparison-only", engine_compilation=False,
        reference_sha256=hashlib.sha256((root / "GnmRHIReference.h").read_bytes()).hexdigest(),
        headers={k: {key: val for key, val in v.items() if key != "methods"} for k, v in sources.items()},
        verified_overrides=verified)
    if args.report:
        args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf8")
    print(f"PASS: {len(verified)} override signatures match supplied UE headers; not an engine build.")


if __name__ == "__main__":
    main()

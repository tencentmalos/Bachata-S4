#!/usr/bin/env python3
"""Measure committed source text; not SLOC, compiled size, or a performance estimate."""

import argparse
import collections
import json
from pathlib import Path
import subprocess
import tarfile


ROOT = Path(__file__).resolve().parents[2]
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx",
                   ".inl", ".inc", ".s", ".asm"}
OLD_FEX = "f2b679f6028ce1c38875233aecfcf5d3f8ebecec"


def git(repo, *args):
    return subprocess.check_output(["git", "-C", str(repo), *args], text=True).strip()


def starts(path, *prefixes):
    return any(path.startswith(prefix) for prefix in prefixes)


def groups(kind, path):
    if kind == "fex":
        if not starts(path, "External/", "Source/Common/cpp-optparse/"):
            yield "project_source_including_tests"
        if starts(path, "External/"):
            yield "vendored_external_source_in_parent_git"
        if starts(path, "FEXCore/Source/", "FEXCore/include/"):
            yield "core"
        if starts(path, "FEXCore/Source/", "FEXCore/include/", "CodeEmitter/", "FEXHeaderUtils/"):
            yield "core_with_emitter_and_header_utils"
        for name, prefix in {
            "code_emitter": "CodeEmitter/", "header_utils": "FEXHeaderUtils/",
            "public_headers": "FEXCore/include/", "jit": "FEXCore/Source/Interface/Core/JIT/",
            "dispatcher": "FEXCore/Source/Interface/Core/Dispatcher/",
            "ir": "FEXCore/Source/Interface/IR/", "utils": "FEXCore/Source/Utils/",
            "linux_emulation": "Source/Tools/LinuxEmulation/",
            "host_common": "Source/Common/", "common_tools": "Source/Tools/CommonTools/",
            "thunks": "ThunkLibs/", "windows": "Source/Windows/",
        }.items():
            if starts(path, prefix):
                yield name
        if starts(path, "FEXCore/Source/Interface/Core/OpcodeDispatcher", "FEXCore/Source/Interface/Core/X86Tables/") or path in {
            "FEXCore/Source/Interface/Core/Frontend.cpp", "FEXCore/Source/Interface/Core/Frontend.h",
        }:
            yield "x86_frontend_and_semantics"
        if starts(path, "unittests/", "FEXCore/unittests/"):
            yield "tests"
    else:
        if not starts(path, "externals/"):
            yield "project_source_including_tests"
        else:
            yield "vendored_external_source_in_parent_git"
        if starts(path, "src/dynarmic/"):
            yield "core"
            if not starts(path, "src/dynarmic/backend/x64/", "src/dynarmic/backend/riscv64/"):
                yield "core_without_other_host_backends"
        for name, prefix in {
            "public_headers": "src/dynarmic/interface/", "a32_frontend": "src/dynarmic/frontend/A32/",
            "a64_frontend": "src/dynarmic/frontend/A64/", "frontend_all": "src/dynarmic/frontend/",
            "arm64_backend": "src/dynarmic/backend/arm64/", "x64_backend": "src/dynarmic/backend/x64/",
            "riscv64_backend": "src/dynarmic/backend/riscv64/", "ir": "src/dynarmic/ir/",
            "common": "src/dynarmic/common/", "tests": "tests/",
            "oaknut": "externals/oaknut/", "mcl": "externals/mcl/",
        }.items():
            if starts(path, prefix):
                yield name


def measure(repo, revision, kind):
    revision = git(repo, "rev-parse", revision)
    totals = collections.defaultdict(lambda: dict(files=0, lines=0, nonblank=0, bytes=0))
    largest = []
    by_extension = collections.Counter()
    proc = subprocess.Popen(["git", "-C", str(repo), "archive", revision], stdout=subprocess.PIPE)
    with tarfile.open(fileobj=proc.stdout, mode="r|*") as archive:
        for member in archive:
            suffix = Path(member.name).suffix.lower()
            if not member.isfile() or suffix not in SOURCE_SUFFIXES:
                continue
            blob = archive.extractfile(member).read()
            if b"\x00" in blob:
                continue
            lines = blob.decode("utf-8", errors="replace").splitlines()
            count = dict(files=1, lines=len(lines), nonblank=sum(bool(x.strip()) for x in lines), bytes=len(blob))
            for group in groups(kind, member.name):
                for key, value in count.items():
                    totals[group][key] += value
            by_extension[suffix] += 1
            if (kind == "fex" and starts(member.name, "FEXCore/Source/", "FEXCore/include/", "CodeEmitter/", "FEXHeaderUtils/")) or (
                kind == "dynarmic" and starts(member.name, "src/dynarmic/")
            ):
                largest.append(dict(path=member.name, **count))
    if proc.wait() != 0:
        raise RuntimeError(f"git archive failed: {repo}")
    submodules = [line.split("\t", 1)[1] for line in git(repo, "ls-tree", "-r", revision).splitlines() if line.startswith("160000 ")]
    return dict(path=str(repo.resolve()), revision=revision,
                subject=git(repo, "show", "-s", "--format=%cs %s", revision),
                groups=dict(sorted(totals.items())), extensions=dict(sorted(by_extension.items())),
                excluded_gitlinks=submodules,
                largest_core_files=sorted(largest, key=lambda x: x["nonblank"], reverse=True)[:15])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fex", type=Path, default=ROOT / "references/FEX")
    parser.add_argument("--citron-dynarmic", type=Path, default=ROOT / "references/dynarmic-citron")
    parser.add_argument("--azahar-dynarmic", type=Path, default=ROOT / "references/dynarmic-azahar")
    parser.add_argument("--fex-revision", default="HEAD")
    parser.add_argument("--citron-revision", default="HEAD")
    parser.add_argument("--azahar-revision", default="HEAD")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    report = dict(methodology={
        "source": "git archive of exact revisions; working-tree modifications and nested gitlinks excluded",
        "suffixes": sorted(SOURCE_SUFFIXES),
        "lines": "physical lines, including comments; nonblank excludes whitespace-only lines",
        "exclusions": "build outputs, nested gitlinks, scripts, CMake, JSON/DSL inputs and documentation",
        "generated": "tracked generated C/C++/assembly text is included; generated build outputs are not",
        "groups": "groups can overlap; do not sum all groups",
    }, repositories={
        "fex": measure(args.fex, args.fex_revision, "fex"),
        "fex_previous_reference": measure(args.fex, OLD_FEX, "fex"),
        "dynarmic_citron": measure(args.citron_dynarmic, args.citron_revision, "dynarmic"),
        "dynarmic_azahar": measure(args.azahar_dynarmic, args.azahar_revision, "dynarmic"),
    })
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
    for name, repo in report["repositories"].items():
        print(name, repo["revision"])
        for group, count in repo["groups"].items():
            print(f"  {group}: {count['files']} files, {count['lines']} lines, {count['nonblank']} nonblank")


if __name__ == "__main__":
    main()

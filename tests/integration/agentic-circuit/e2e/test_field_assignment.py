"""FW-0005 native verification and independently expected gfsim behavior."""

from pathlib import Path
import ast
import os
import re
import shlex
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[4]
FIXTURE = Path(__file__).with_name("fixtures") / "field_assignment"


def run(command, **kwargs):
    result = subprocess.run(command, text=True, capture_output=True, **kwargs)
    assert result.returncode == 0, result.stdout + result.stderr
    return result.stdout


@pytest.mark.parametrize("explicit", [False, True], ids=["field-store", "with-fields"])
def test_field_assignment_gfsim(tmp_path, explicit):
    from agentic_circuit._queue_frontend import lower_queue_source, RULE_LOWERING_PIPELINE

    opt = os.environ.get("ACIR_OPT")
    cxxgen = os.environ.get("ACIR_QUEUE_CXXGEN")
    plan = os.environ.get("ACIR_QUEUE_PLAN")
    if not all((opt, cxxgen, plan, shutil.which("c++"))):
        pytest.skip("integrated ACIR/gfsim environment is unavailable")
    text = (FIXTURE / "architecture.py").read_text()
    if explicit:
        class Expand(ast.NodeTransformer):
            def visit_Assign(self, node):
                target = node.targets[0]
                if isinstance(target, ast.Attribute):
                    base = ast.unparse(target.value)
                    return ast.copy_location(ast.parse(
                        f"{base} = {base}.with_fields({target.attr}={ast.unparse(node.value)})"
                    ).body[0], node)
                return node
        text = ast.unparse(ast.fix_missing_locations(Expand().visit(ast.parse(text))))
    raw = lower_queue_source(text, "fields", host_results=True)
    source = tmp_path / "raw.mlir"
    frozen = tmp_path / "frozen.mlir"
    source.write_text(raw)
    run([opt, f"--pass-pipeline={RULE_LOWERING_PIPELINE}", "--verify-each", str(source), "-o", str(frozen)])
    run([plan, str(frozen)])
    unsafe = tmp_path / "unsafe-index.mlir"
    unsafe.write_text(lower_queue_source(
        text.replace("command.index", "command.value"), "fields", host_results=True
    ))
    rejected_index = subprocess.run(
        [opt, f"--pass-pipeline={RULE_LOWERING_PIPELINE}", str(unsafe)],
        text=True, capture_output=True,
    )
    assert rejected_index.returncode != 0
    assert "index" in rejected_index.stderr
    for pattern, replacement in (
        (r'(ac.var.with [^\n]*field )"x"', r'\1"missing"'),
        (r'(ac.var.with [^\n]*field )"valid"', r'\1"x"'),
    ):
        malformed, count = re.subn(pattern, replacement, raw, count=1)
        assert count == 1
        invalid = tmp_path / "invalid.mlir"
        invalid.write_text(malformed)
        rejected = subprocess.run([opt, str(invalid)], text=True, capture_output=True)
        assert rejected.returncode != 0
        assert "ac.var.with" in rejected.stderr
    (tmp_path / "model.cpp").write_text(run([cxxgen, str(frozen)]))
    shutil.copyfile(FIXTURE / "driver.cpp", tmp_path / "driver.cpp")
    build = Path(opt).resolve().parent.parent
    cache = {}
    for line in (build / "CMakeCache.txt").read_text().splitlines():
        if "=" in line and not line.startswith(("//", "#")):
            key, value = line.split("=", 1)
            cache[key.split(":", 1)[0]] = value
    llvm = shlex.split(run(["llvm-config", "--ldflags", "--libs", "support"]))
    executable = tmp_path / "driver"
    run(
        [
            "c++",
            "-std=c++20",
            "-I",
            str(ROOT / "simulator/gfsim/include"),
            str(tmp_path / "driver.cpp"),
            str(build / "compiler/acir/gfsim/libgfsim.a"),
            str(build / "compiler/acir/lib/Bindings/libACIRBindings.a"),
            *llvm,
            "-lrt",
            "-ldl",
            "-lm",
            cache["ZLIB_LIBRARY_RELEASE"],
            cache["zstd_LIBRARY"],
            "-o",
            str(executable),
        ]
    )
    assert "field assignment expected results and parity passed" in run([str(executable)])

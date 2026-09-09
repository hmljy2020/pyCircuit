"""Generic end-to-end and verifier regression for FW-0002."""

from pathlib import Path
import os
import re
import shlex
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[4]
FIXTURE = Path(__file__).with_name("fixtures") / "blocking_branch"


def run(command, **kwargs):
    result = subprocess.run(command, text=True, capture_output=True, **kwargs)
    assert result.returncode == 0, result.stdout + result.stderr
    return result.stdout


def test_blocking_branch_gfsim_and_verifiers(tmp_path):
    from agentic_circuit._queue_frontend import (
        lower_queue_source,
        RULE_LOWERING_PIPELINE,
    )

    opt = os.environ.get("ACIR_OPT")
    cxxgen = os.environ.get("ACIR_QUEUE_CXXGEN")
    plan = os.environ.get("ACIR_QUEUE_PLAN")
    if not all((opt, cxxgen, plan, shutil.which("c++"))):
        pytest.skip("integrated ACIR/gfsim environment is unavailable")
    raw = lower_queue_source(
        (FIXTURE / "architecture.py").read_text(), "blocking_branch"
    )
    source = tmp_path / "raw.mlir"
    frozen = tmp_path / "frozen.mlir"
    source.write_text(raw)
    run(
        [
            opt,
            f"--pass-pipeline={RULE_LOWERING_PIPELINE}",
            str(source),
            "-o",
            str(frozen),
        ]
    )
    run([plan, str(frozen)])
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
    assert "parity passed" in run([str(executable)])

    # Removing candidate qualification must fail even when bypassing Python.
    candidate = re.search(r"ac.rule.condition (%\w+)", raw)[1]
    qualified = re.search(
        rf"(%\w+) = ac.var.and {candidate}, (%\w+) : !ac.var<i1>", raw
    )
    assert qualified
    malformed = raw.replace(f"when {qualified[1]} :", f"when {qualified[2]} :")
    source.write_text(malformed)
    result = subprocess.run(
        [opt, "--pass-pipeline=builtin.module(ac-lower-rules)", str(source)],
        text=True,
        capture_output=True,
    )
    assert result.returncode != 0
    assert "presence must imply" in result.stderr

    # Firing verification must independently reject a forged frozen proposal.
    text = frozen.read_text()
    candidate = re.search(r"ac.firing.condition (%\w+)", text)[1]
    qualified = re.search(
        rf"(%\w+) = ac.var.and {candidate}, (%\w+) : !ac.var<i1>", text
    )
    assert qualified
    forged = tmp_path / "forged.mlir"
    forged.write_text(text.replace(f"when {qualified[1]} :", f"when {qualified[2]} :"))
    result = subprocess.run([opt, str(forged)], text=True, capture_output=True)
    assert result.returncode != 0
    assert "presence must imply" in result.stderr

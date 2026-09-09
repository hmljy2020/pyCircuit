"""CMT expected results, scheduling/recording parity, and offline replay."""

from __future__ import annotations

import os
import re
import shlex
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[5]
SCENARIOS = (
    "normal",
    "capacity",
    "backpressure",
    "invalid",
    "diagnostics",
    "recovery",
    "isolation",
    "reset",
)


def run(command, cwd):
    result = subprocess.run(command, cwd=cwd, text=True, capture_output=True)
    assert result.returncode == 0, result.stdout + result.stderr
    return result


def generate_cmt(out):
    import agentic_circuit as ac
    from agentic_circuit._jit import _lower_acir_to_cpp, _lower_queue_acir
    from agentic_circuit._queue_frontend import lower_queue_source

    from designs.davincioo.spe.ooo.cmt import dual_cmt_system

    system = dual_cmt_system
    system_name = "dual_cmt_system"
    out.mkdir(parents=True, exist_ok=True)
    specialization = ac.jit(system, workspace=ROOT)
    raw = lower_queue_source(specialization._source(), system_name, host_results=True)
    (out / "raw.mlir").write_text(raw)
    (out / "frozen.mlir").write_text(_lower_queue_acir(raw))
    (out / "model.cpp").write_text(_lower_acir_to_cpp(raw))
    shutil.copyfile(Path(__file__).with_name("cmt_driver.cpp"), out / "driver.cpp")
    build = ROOT / ".pycircuit_out/local-clang22/build"
    cache = {}
    for line in (build / "CMakeCache.txt").read_text().splitlines():
        if "=" in line and not line.startswith(("//", "#")):
            key, value = line.split("=", 1)
            cache[key.split(":", 1)[0]] = value
    llvm_flags = run(["llvm-config", "--ldflags", "--libs", "support"], out).stdout
    command = [
        "c++",
        "-std=c++20",
        "-I",
        str(ROOT / "simulator/gfsim/include"),
        str(out / "driver.cpp"),
        str(build / "compiler/acir/gfsim/libgfsim.a"),
        str(build / "compiler/acir/lib/Bindings/libACIRBindings.a"),
        *shlex.split(llvm_flags),
        "-lrt",
        "-ldl",
        "-lm",
        cache["ZLIB_LIBRARY_RELEASE"],
        cache["zstd_LIBRARY"],
        "-o",
        str(out / "cmt_driver"),
    ]
    (out / "compile-command.txt").write_text(shlex.join(command) + "\n")
    run(command, out)
    return out


@pytest.fixture(scope="module")
def generated_cmt():
    out = Path(
        os.environ.get(
            "PYC_DAVINCIOO_CMT_OUT", ROOT / ".pycircuit_out/davincioo-cmt/20260909-cmt"
        )
    ).resolve()
    return generate_cmt(out)


@pytest.mark.parametrize("scenario", SCENARIOS)
def test_cmt_expected_results_and_replay(generated_cmt, scenario):
    out = generated_cmt / scenario
    out.mkdir(exist_ok=True)
    executable = generated_cmt / "cmt_driver"
    normal = run([str(executable), scenario], out)
    (out / "normal.log").write_text(normal.stdout)
    projection = (out / "projection.bin").read_bytes()
    recorded = run(["env", "PYC_RECORD_REPLAY=1", str(executable), scenario], out)
    (out / "recorded.log").write_text(recorded.stdout)
    assert projection == (out / "projection.bin").read_bytes()
    assert normal.stdout == recorded.stdout
    viewer = ROOT / "third_party/circuit-flow-viewer/src"
    run(
        [
            "env",
            f"PYTHONPATH={viewer}",
            "python",
            "-m",
            "circuit_flow_viewer.cli",
            "render",
            "execution.pyctrace",
            "--output",
            "replay.html",
        ],
        out,
    )
    (generated_cmt / "index.html").write_text(
        '<!doctype html><meta charset="utf-8"><title>CMT 回放</title><h1>CMT 单模块回放</h1><ul>'
        + "".join(f'<li><a href="{s}/replay.html">{s}</a></li>' for s in SCENARIOS)
        + "</ul>"
    )


def test_cmt_static_history_loop_matches_expanded_acir():
    import agentic_circuit as ac
    from agentic_circuit._queue_frontend import lower_queue_source

    from designs.davincioo.spe.ooo.cmt import dual_cmt_system

    source = ac.jit(dual_cmt_system, workspace=ROOT)._source()
    pattern = r"(?m)^( +)for i in range\(16\):\n" r" +histories\[i\]\.valid = False\n"
    expanded_source, replacements = re.subn(
        pattern,
        lambda match: "".join(
            match[1] + f"histories[{i}] = histories[{i}].with_fields(valid=False)\n"
            for i in range(16)
        ),
        source,
    )
    assert replacements == 1
    loop_acir = lower_queue_source(source, "dual_cmt_system", host_results=True)
    expanded_acir = lower_queue_source(
        expanded_source, "dual_cmt_system", host_results=True
    )

    def without_source_fingerprint(text):
        # Source edits intentionally change the JIT cache identity. Compare
        # every operation, type, binding and constant without that provenance.
        normalized, count = re.subn(
            r'jit_specialization = "sha256:[0-9a-f]{64}"',
            'jit_specialization = "source-fingerprint"',
            text,
        )
        assert count == 1
        return normalized

    assert without_source_fingerprint(loop_acir) == without_source_fingerprint(
        expanded_acir
    )

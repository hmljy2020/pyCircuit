"""Generate and execute standalone t-gfsim C++ from published source units."""

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    for name in (
        "repo",
        "cxx",
        "source-compiler",
        "linker",
        "final-linker",
        "final-emitter",
        "optimizer",
        "emitter",
        "scratch",
    ):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    args.scratch.mkdir(parents=True, exist_ok=True)
    scratch = Path(tempfile.mkdtemp(prefix="run-", dir=args.scratch)).resolve()
    fixtures = Path(__file__).resolve().parent
    (scratch / "units").mkdir()
    env = dict(
        os.environ,
        PYTHONPATH=str(args.repo / "python"),
        PYCIRCUIT_SOURCE_COMPILER=str(args.source_compiler),
        PYCIRCUIT_LINKER=str(args.final_linker),
        PYCIRCUIT_EMITTER=str(args.final_emitter),
    )
    commands = []

    def run(*args, reject=False):
        command = list(map(str, args))
        result = subprocess.run(
            command, cwd=scratch, env=env, text=True, capture_output=True, timeout=120
        )
        commands.append(
            {
                "command": command,
                "exit_status": result.returncode,
                "stdout": result.stdout,
                "stderr": result.stderr,
            }
        )
        (scratch / "commands.json").write_text(json.dumps(commands, indent=2) + "\n")
        if reject:
            assert result.returncode > 0, commands[-1]
        else:
            assert result.returncode == 0, commands[-1]
        return result

    def compile_unit(source, dependencies=()):
        unit = scratch / "units" / source.stem
        command = [
            sys.executable,
            "-m",
            "pycircuit.cli",
            "compile",
            "-c",
            source.name,
            "--source-root",
            source.parent,
            "--package-prefix",
            "linked_gfsim",
            "-o",
            unit,
        ]
        for dependency in dependencies:
            command.extend(("-I", dependency))
        run(*command)
        return unit

    def link_source(name, *units):
        command = [args.linker]
        for unit in units:
            files = json.loads((unit / "unit.json").read_text())["files"]
            command.extend(
                ("--body", unit / files["body"], "--header", unit / files["interface"])
            )
        linked = scratch / f"{name}.linked.ac"
        run(
            *command,
            "--top",
            f"linked_gfsim.{units[-1].name}.{name}",
            "--output",
            linked,
        )
        return linked

    def link(name, *units):
        return infer(name, link_source(name, *units))

    def infer(name, linked):
        inferred = scratch / f"{name}.inferred.ac"
        run(
            args.optimizer,
            linked,
            "--ac-simplify-linked-registrations",
            "--ac-elide-linked-true-guards",
            "--ac-infer-linked-method-guards",
            "--mlir-print-debuginfo",
            "-o",
            inferred,
        )
        run(args.linker, "--verify-only", inferred)
        return inferred

    def execute(inferred, case):
        build = scratch / inferred.name.split(".")[0]
        build.mkdir()
        header = build / "model.h"
        binary = build / "simulation"
        run(args.emitter, inferred, "--output", header)
        run(
            args.cxx,
            "-std=c++20",
            "-I" + str(args.repo / "include"),
            "-I" + str(build),
            f"-D{case}",
            fixtures / "simulate.cpp",
            "-o",
            binary,
        )
        run(binary)

    def reject_emission(inferred, *messages):
        header = scratch / "protected.h"
        sentinel = b"keep the previous output\n"
        header.write_bytes(sentinel)
        rejected = run(args.emitter, inferred, "--output", header, reject=True)
        for message in messages:
            assert message in rejected.stderr, rejected.stderr
        assert header.read_bytes() == sentinel

    multi = compile_unit(args.repo / "examples" / "multi_outputs" / "multi_outputs.py")
    design = compile_unit(fixtures / "designs.py")
    queue = compile_unit(fixtures.parent / "linked_registration_queue.py")
    bench = compile_unit(fixtures / "bench.py", [queue])
    scenarios = {
        "MultiOutputs": (multi, "MULTI_OUTPUTS"),
        "GroupedOutputs": (multi, "GROUPED_OUTPUTS"),
        "Fanout": (design, "FANOUT"),
        "Stage": (design, "STAGE"),
        "SwapState": (design, "SWAP_STATE"),
        "CachedWork": (design, "CACHED_WORK"),
        "ThroughMethods": (design, "METHODS"),
        "TimedQueue": (design, "TIMED_QUEUE"),
        "TableState": (design, "TABLE_STATE"),
        "TableQueue": (design, "TABLE_QUEUE"),
        "ConflictingPops": (design, "CONFLICTING_POPS"),
        "QueueAdd": (queue, "QUEUE_ADD"),
    }
    inferred = [(link(name, unit), case) for name, (unit, case) in scenarios.items()]
    checked_queue = link("CheckedQueueAdd", queue, bench)
    sampled_source = scratch / "sampledbench.py"
    sampled_source.write_text(
        (fixtures / "bench.py").read_text().replace(
            "@ac.system", "@ac.system(samples=((0, 0), (1, 0)))"
        ).replace("def CheckedQueueAdd", "def SampledQueueAdd")
    )
    sampled_bench = compile_unit(sampled_source, [queue])
    sampled_system = link("SampledQueueAdd", queue, sampled_bench)
    raw_bound = link("RawBound", design)
    raw_read = link("RawRead", design)
    interleaved = link("Interleaved", design)
    unknown = link("UnknownValue", design)

    # Wrap the compiled stateful Module in a native Collection.
    linked = link_source("FeedbackCell", design).read_text()
    aliases, family = (fixtures / "collection.mlir").read_text().split("// Body\n")
    prefix, system, rest = linked.partition('  "ac.system"()')
    assert system
    system = system + rest.replace(
        "callee = @linked_gfsim.designs.FeedbackCell",
        "callee = @linked_gfsim.designs.CollectionFamily",
        1,
    )

    def collection(name, body):
        package = scratch / f"{name}.linked.ac"
        package.write_text(aliases + prefix + body + system)
        return infer(name, package)

    collection_model = collection("CollectionFamily", family)
    clock_data = collection(
        "ClockAsData",
        family.replace("(%family, %index)", "(%family, %clock)").replace(
            "-> !family_row\n",
            '-> !family_row loc("clock-as-data.py":7:3)\n',
        ),
    )
    collection_cycle = collection(
        "CollectionCycle",
        family.replace("%feedback, %first", "%feedback, %feedback"),
    )

    # Feed the child output into the addition that supplies its input.
    symbol = 'sym_name = "linked_gfsim.designs.Interleaved"'
    prefix, found, body = interleaved.read_text().partition(symbol)
    assert found
    child = re.search(
        r"(?m)^    (%\d+) = ac\.instance\((%\d+)\).* : \(.*\) -> (.*?) loc\(", body
    )
    assert child
    produced, prepared, produced_type = child.groups()
    body, count = re.subn(
        rf'(?m)^(    {prepared} = "ac\.bits\.binary"\()%arg0(, .* : \()!ac\.bits<<.*?>>',
        lambda match: f"{match[1]}{produced}{match[2]}{produced_type}",
        body,
        count=1,
    )
    assert count == 1
    cycle = scratch / "InterleavedCycle.inferred.ac"
    cycle.write_text(prefix + symbol + body)
    run(args.linker, "--verify-only", cycle)

    for ir, case in inferred:
        execute(ir, case)
    execute(interleaved, "INTERLEAVED")
    execute(collection_model, "COLLECTION_FAMILY")
    execute(checked_queue, "CHECKED_QUEUE_ADD")
    reject_emission(
        sampled_system,
        "physical System samples are unsupported in t-gfsim",
        "sampledbench.py",
    )
    reject_emission(
        clock_data, "t-gfsim cannot use the root clock as data", "clock-as-data.py"
    )
    reject_emission(raw_bound, "_wired", "in_valid", "designs.py")
    reject_emission(raw_read, "inspected", "valid", "designs.py")
    reject_emission(cycle, "cyclic same-cycle value dependence")
    reject_emission(collection_cycle, "cyclic same-cycle value dependence")
    reject_emission(unknown, "X/Z", "designs.py")


if __name__ == "__main__":
    main()

"""Check standalone source linking against independently published units."""

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
from collections import Counter
from pathlib import Path


def call_inventory(artifact):
    text = artifact.read_text()
    locations = dict(re.findall(r"^(#loc\d*) = (loc\(.*\))$", text, re.MULTILINE))
    calls = Counter()
    for line in text.splitlines():
        if '"ac.method.call"(' not in line:
            continue
        attributes = re.search(
            r"callee = (.*?), instance_path = (\[.*?\]), occurrence = (.*?)}>",
            line,
        )
        source = re.search(r"ac\.location = (\{[^}]*\})", line)
        location = re.search(r" loc\((.*)\)$", line)
        assert attributes and source and location, line
        calls[
            (
                *attributes.groups(),
                source[1],
                locations.get(location[1], location[0].strip()),
            )
        ] += 1
    return calls


def method_inventory(artifact):
    return Counter(
        re.search(r'definition = (@[^}]+)}}, sym_name = "([^"]+)"', line).groups()
        for line in artifact.read_text().splitlines()
        if '"ac.method"(' in line
    )


def main():
    parser = argparse.ArgumentParser()
    for name in (
        "repo",
        "source-compiler",
        "source-linker",
        "linker",
        "emitter",
        "scratch",
    ):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    args.scratch.mkdir(parents=True, exist_ok=True)
    scratch = Path(tempfile.mkdtemp(prefix="run-", dir=args.scratch)).resolve()
    inputs = scratch / "units"
    portable = scratch / "portable"
    inputs.mkdir()
    portable.mkdir()
    commands = []
    env = dict(
        os.environ,
        PYTHONPATH=str(args.repo / "python"),
        PYCIRCUIT_SOURCE_COMPILER=str(args.source_compiler),
        PYCIRCUIT_LINKER=str(args.linker),
        PYCIRCUIT_EMITTER=str(args.emitter),
    )

    def run(*command, expected=0, cwd=scratch):
        command = list(map(str, command))
        result = subprocess.run(
            command, cwd=cwd, env=env, capture_output=True, text=True, timeout=120
        )
        commands.append(
            {
                "command": command,
                "cwd": str(cwd),
                "exit_status": result.returncode,
                "expected_exit": expected,
                "stderr": result.stderr,
                "stdout": result.stdout,
            }
        )
        (scratch / "commands.json").write_text(json.dumps(commands, indent=2) + "\n")
        assert result.returncode == expected, commands[-1]
        return result

    def cli(*command):
        return run(sys.executable, "-m", "pycircuit.cli", *command)

    def compile_unit(source, imports=()):
        unit = inputs / source.stem
        command = [
            "compile",
            "-c",
            source.name,
            "--source-root",
            source.parent,
            "--package-prefix",
            "serial_methods",
            "-o",
            unit,
        ]
        for dependency in imports:
            command.extend(("-I", dependency))
        cli(*command)
        return unit

    def payload(unit, kind):
        return unit / json.loads((unit / "unit.json").read_text())["files"][kind]

    def link(units, top, output, expected=0):
        command = [args.source_linker]
        for unit in units:
            command.extend(
                (
                    "--body",
                    payload(unit, "body"),
                    "--header",
                    payload(unit, "interface"),
                )
            )
        run(*command, "--top", top, "--output", output, expected=expected)

    serial = Path(__file__).resolve().parent / "serial-method"
    provider = compile_unit(serial / "cross_provider.py")
    relay = compile_unit(serial / "cross_relay.py", [provider])
    consumer = compile_unit(serial / "cross_consumer.py", [provider, relay])
    queue = compile_unit(
        args.repo / "tests/system/fixtures/source_system/routed_queue_add.py"
    )
    top = "serial_methods.cross_consumer.Top"
    queue_top = "serial_methods.routed_queue_add.QueueAdd"
    units = [provider, relay, consumer, queue]
    original = sum((call_inventory(payload(unit, "body")) for unit in units), Counter())
    bindings = {(callee, path) for callee, path, *_ in original}
    assert (
        "@serial_methods.cross_relay.Relay::@advance",
        '["left"]',
    ) in bindings
    assert (
        "@serial_methods.cross_provider.Counter::@add",
        '["counter"]',
    ) in bindings
    for path in ('["left"]', '["right"]', '["result"]'):
        for method in ("push", "pop"):
            assert (
                f"@pycircuit.__builtins__.fifo_downstream_pop::@{method}",
                path,
            ) in bindings

    linked = scratch / "linked.ac"
    reversed_output = scratch / "reversed.ac"
    queue_output = scratch / "queue.ac"
    link(units, top, linked)
    link(reversed(units), top, reversed_output)
    assert linked.read_bytes() == reversed_output.read_bytes()
    assert 'ac.stage = "linked-source"' in linked.read_text()
    assert call_inventory(linked) == original
    assert method_inventory(linked) == sum(
        (method_inventory(payload(unit, "body")) for unit in units), Counter()
    )

    # QueueAdd is an unused definition in the cross-source closure. Select it
    # separately to check its FIFO calls under an actual root binding as well.
    link([queue], queue_top, queue_output)
    assert call_inventory(queue_output) == call_inventory(payload(queue, "body"))
    run(args.source_linker, "--verify-only", queue_output)

    missing = scratch / "missing.ac"
    link([relay, consumer, queue], top, missing, expected=1)
    assert not missing.exists()
    protected = scratch / "protected.ac"
    protected.write_bytes(b"keep this file\n")
    link(units, top, protected, expected=1)
    assert protected.read_bytes() == b"keep this file\n"

    final = scratch / "final.ac"
    cli("link", provider, relay, consumer, "--top", top, "-o", final)
    inputs.rename(scratch / "retained-units")
    standalone = linked.rename(portable / linked.name)
    run(args.source_linker, "--verify-only", standalone.name, cwd=portable)
    # The idle instance exists, but has a different Module type from Relay.
    invalid = portable / "wrong-receiver.ac"
    receiver = "callee = @serial_methods.cross_relay.Relay::@advance, instance_path = "
    text = standalone.read_text()
    assert text.count(receiver + '["left"]') == 1
    invalid.write_text(text.replace(receiver + '["left"]', receiver + '["idle"]'))
    run(args.source_linker, "--verify-only", invalid.name, cwd=portable, expected=1)
    for target in ("cpp", "verilog"):
        accepted = run(args.emitter, final, "--target", target)
        assert json.loads(accepted.stdout)
        rejected = run(args.emitter, standalone, "--target", target, expected=1)
        assert "requires final hardware" in rejected.stderr


if __name__ == "__main__":
    main()

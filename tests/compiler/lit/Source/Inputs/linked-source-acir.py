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

OCCURRENCE = r"\boccurrence = (\{expansion = \[\], site = \{.*?}})"


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


def method_regions(text):
    return {
        name: (guard, body)
        for name, guard, body in re.findall(
            r'^    "ac.method"\([^\n]*sym_name = "([^"]+)"[^\n]*\n'
            r"(.*?)^    }, \{\n(.*?)^    }\)",
            text,
            re.MULTILINE | re.DOTALL,
        )
    }


def method_guards(text):
    return {name: guard for name, (guard, _) in method_regions(text).items()}


def without_location_aliases(region):
    return re.sub(r" loc\(#loc\d*\)", "", region)


def guard_conjunction(guard):
    """Read the SSA operands that contribute to the yielded guard."""
    definitions = {}
    for line in guard.splitlines():
        result = re.match(r"\s*(%\w+) = ", line)
        if result:
            definitions[result[1]] = line.strip()

    def terms(value):
        line = definitions[value]
        if '"ac.bits.binary"' in line and 'opcode = "and"' in line:
            lhs, rhs = re.search(r'"ac.bits.binary"\((%\w+), (%\w+)\)', line).groups()
            return terms(lhs) + terms(rhs)
        return [line]

    yielded = re.search(r'"ac.yield"\((%\w+)\)', guard)[1]
    return definitions, terms(yielded)


def guard_queries(terms, body):
    calls = {}
    for line in body.splitlines():
        if '"ac.method.call"' in line:
            binding = re.search(r'::@(\w+), instance_path = (\[.*?\])', line).groups()
            calls[re.search(OCCURRENCE, line)[1]] = binding
    queries = {}
    for line in terms:
        query = re.search(r'"ac.method.guard_query"\(([^)]*)\)', line)
        if query:
            binding = calls[re.search(OCCURRENCE, line)[1]]
            assert binding not in queries
            queries[binding] = query[1].split(", ") if query[1] else []
    return queries


def main():
    parser = argparse.ArgumentParser()
    for name in (
        "repo",
        "optimizer",
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

    def compile_unit(source, imports=(), package="serial_methods"):
        package_dir = inputs / package
        package_dir.mkdir(exist_ok=True)
        unit = package_dir / source.stem
        command = [
            "compile",
            "-c",
            source.name,
            "--source-root",
            source.parent,
            "--package-prefix",
            package,
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

    # Keep the ordinary example independent of optional local gallery entries.
    registration_queue = compile_unit(
        Path(__file__).resolve().parent / "linked_registration_queue.py"
    )
    registration_input = scratch / "registrations.ac"
    link(
        [registration_queue],
        "serial_methods.linked_registration_queue.QueueAdd",
        registration_input,
    )

    def simplify(source, name):
        output = scratch / (name + ".ac")
        options = ("--ac-simplify-linked-registrations", "--mlir-print-debuginfo")
        run(args.optimizer, source, *options, "-o", output)
        run(args.source_linker, "--verify-only", output)
        again = scratch / (name + ".again.ac")
        run(args.optimizer, output, *options, "-o", again)
        assert output.read_bytes() == again.read_bytes()
        assert 'ac.stage = "linked-source"' in output.read_text()
        assert "ac.registration_entry" not in output.read_text()
        return output

    simplified = simplify(registration_input, "registrations-simplified")
    original_text = registration_input.read_text()
    simplified_text = simplified.read_text()
    assert original_text.count("ac.registration_entry") == 4
    for name in ("send_left", "send_right", "add", "receive"):
        assert simplified_text.count('ac.registration_name = "' + name + '"') == 1
    assert '"ac.transaction"' not in simplified_text
    assert call_inventory(registration_input) == call_inventory(simplified)
    # The public receive result must flow directly to the Module output; its
    # exact declared type (including provenance) must survive promotion.
    receive = next(
        line
        for line in simplified_text.splitlines()
        if 'ac.registration_name = "receive"' in line
    )
    result = re.search(r"(%[\w]+)(?::(\d+))? =", receive)
    result = result[1] + ("#0" if result[2] else "")
    assert re.search(r'"ac.yield"\(' + re.escape(result) + r"\)", simplified_text)
    wrapper_end = next(
        line
        for line in original_text.splitlines()
        if "ac.registration_entry" in line and "-> ()" not in line
    )
    public_type = wrapper_end.rsplit(" -> ", 1)[1].split(" loc(", 1)[0]
    call_types = receive.rsplit(" -> ", 1)[1].split(" loc(", 1)[0]
    assert call_types == public_type or call_types.startswith("(" + public_type + ",")

    elide_pass = "--ac-elide-linked-true-guards"

    def elide(source, name):
        output = scratch / (name + ".ac")
        run(args.optimizer, source, elide_pass, "--mlir-print-debuginfo", "-o", output)
        run(args.source_linker, "--verify-only", output)
        assert call_inventory(source) == call_inventory(output)
        assert method_inventory(source) == method_inventory(output)
        return output

    run(args.optimizer, registration_input, elide_pass, expected=1)
    elided = elide(simplified, "queue-add-elided")
    elided_text = elided.read_text()
    assert elided_text.count("ac.implicit_path") == 10
    guards = method_guards(elided_text)
    assert len(guards) == 4
    assert all('"ac.yield"()' in guard for guard in guards.values())

    def constants(text):
        return [line for line in text.splitlines() if '"ac.bits.constant"' in line]

    assert len(constants(elided_text)) == 23
    assert constants(elided_text) == constants(simplified_text)
    assert elide(elided, "queue-add-elided-again").read_bytes() == elided.read_bytes()
    run(args.optimizer, elided, "--ac-analyze-source-method-summaries", expected=1)

    infer_pass = "--ac-infer-linked-method-guards"

    def infer(source, name):
        output = scratch / (name + ".ac")
        again = scratch / (name + ".again.ac")
        options = (infer_pass, "--mlir-print-debuginfo")
        run(args.optimizer, source, *options, "-o", output)
        run(args.source_linker, "--verify-only", output)
        run(args.optimizer, output, *options, "-o", again)
        assert output.read_bytes() == again.read_bytes()
        assert call_inventory(source) == call_inventory(output)
        assert method_inventory(source) == method_inventory(output)
        return output

    for source in (simplified, elided):
        inferred = infer(source, source.stem + "-inferred")
        guard, body = method_regions(inferred.read_text())["add"]
        original_body = method_regions(source.read_text())["add"][1]
        assert without_location_aliases(body) == without_location_aliases(original_body)
        assert 'opcode = "add"' in body
        _, terms = guard_conjunction(guard)
        assert guard_queries(terms, body) == {
            ("pop", '["left_input"]'): [],
            ("pop", '["right_input"]'): [],
            ("push", '["output"]'): [],
        }
        assert all(
            '"ac.method.guard_query"' in term
            or ('"ac.bits.constant"' in term and "value = #ac.math_int<1>" in term)
            for term in terms
        )

    guard_source = Path(__file__).resolve().parent / "linked_method_guards.py"
    guarded = compile_unit(guard_source)
    guards_input = scratch / "nested-guards.ac"
    link(
        [guarded],
        "serial_methods.linked_method_guards.GuardSystem",
        guards_input,
    )
    nested = simplify(guards_input, "nested-guards-simplified")
    original_regions = method_regions(nested.read_text())
    inferred_text = infer(nested, "nested-guards-inferred").read_text()
    inferred_regions = method_regions(inferred_text)
    assert without_location_aliases("".join(inferred_regions["accept"])) == (
        without_location_aliases("".join(original_regions["accept"]))
    )
    assert 'state = "ceiling"' in inferred_regions["accept"][0]

    drive_guard, drive_body = inferred_regions["drive"]
    definitions, terms = guard_conjunction(drive_guard)
    assert len(terms) == 2
    assert any('predicate = "ult"' in term for term in terms)
    queries = guard_queries(terms, drive_body)
    assert sorted(queries) == [("route", '["relay"]')]
    (argument,) = queries[("route", '["relay"]')]
    assert 'state = "value"' in definitions[argument]

    route_guard, route_body = inferred_regions["route"]
    assert without_location_aliases(route_body) == without_location_aliases(
        original_regions["route"][1]
    )
    definitions, terms = guard_conjunction(route_guard)
    assert len(terms) == 3
    assert any('predicate = "ne"' in term for term in terms)
    queries = guard_queries(terms, route_body)
    assert sorted(queries) == [("accept", '["left"]'), ("accept", '["right"]')]
    for receiver, increment in (("left", 1), ("right", 2)):
        (argument,) = queries[("accept", '["' + receiver + '"]')]
        addition = definitions[argument]
        assert 'opcode = "add"' in addition
        entry_arg, constant = re.search(
            r'"ac.bits.binary"\((%\w+), (%\w+)\)', addition
        ).groups()
        assert route_guard.lstrip().startswith(f"^bb0({entry_arg}:")
        assert f"value = #ac.math_int<{increment}>" in definitions[constant]

    # Both targets have the same signature. Swapping the two occurrences keeps
    # their argument types intact while routing each guard to the wrong call.
    left, right = re.findall(r'"ac.method.guard_query".*?' + OCCURRENCE, route_guard)
    swapped = {left: right, right: left}
    wrong_guard = re.sub(
        OCCURRENCE,
        lambda match: "occurrence = " + swapped.get(match[1], match[1]),
        route_guard,
    )
    wrong_binding = scratch / "wrong-guard-binding.ac"
    wrong_binding.write_text(inferred_text.replace(route_guard, wrong_guard, 1))
    run(args.source_linker, "--verify-only", wrong_binding, expected=1)

    prior = scratch / "prior-result.ac"
    link([guarded], "serial_methods.linked_method_guards.PriorResultSystem", prior)
    prior = simplify(prior, "prior-result-simplified")
    rejected = scratch / "prior-result-inferred.ac"
    error = run(args.optimizer, prior, infer_pass, "-o", rejected, expected=1).stderr
    assert "guard" in error and "ac.method.call" in error
    call_line = next(
        line
        for line, source in enumerate(guard_source.read_text().splitlines(), 1)
        if "left.accept(right.read(), 0)" in source
    )
    assert f"{guard_source.name}:{call_line}:" in error
    assert not rejected.exists()

    dead_guard_constant = constants(guards["add"])[0]
    pruned = scratch / "queue-add-pruned.ac"
    pruned.write_text(elided_text.replace(dead_guard_constant + "\n", "", 1))
    run(args.source_linker, "--verify-only", pruned)

    unmarked = scratch / "queue-add-unmarked.ac"
    unmarked.write_text(elided_text.replace("ac.linked_implicit_guards, ", "", 1))
    run(args.source_linker, "--verify-only", unmarked, expected=1)

    # Give the nested FIFO push a false path without letting Python capture
    # prune the call. The other paths and all four Method guards remain true.
    lines = simplified_text.splitlines(keepends=True)
    push = next(
        i
        for i, line in enumerate(lines)
        if '"ac.method.call"' in line and '::@push, instance_path = ["output"]' in line
    )
    path = re.search(r'"ac.method.call"\((%\w+),', lines[push])[1]
    constant = next(
        i
        for i in range(push - 1, -1, -1)
        if lines[i].lstrip().startswith(path + ' = "ac.bits.constant"')
    )
    lines[constant] = lines[constant].replace(
        "value = #ac.math_int<1>", "value = #ac.math_int<0>", 1
    )
    false_path = scratch / "queue-add-false-path.ac"
    false_path.write_text("".join(lines))
    false_path_elided = elide(false_path, "queue-add-false-path-elided").read_text()
    assert lines[push] in false_path_elided
    assert false_path_elided.count("ac.implicit_path") == 9

    # An otherwise identical transaction without the registration marker is a
    # general transaction and remains part of the linked-source language.
    generic = scratch / "generic-transaction.ac"
    missing_call = (
        "\n".join(
            line
            for line in original_text.splitlines()
            if not ('"ac.method.call"' in line and "::@send_left," in line)
        )
        + "\n"
    )
    generic.write_text(missing_call.replace(", ac.registration_entry", "", 1))
    generic_output = simplify(generic, "generic-simplified")
    assert generic_output.read_text().count('"ac.transaction"') == 1
    assert 'name = "send_left"' in generic_output.read_text()

    malformed = scratch / "malformed-registration.ac"
    malformed.write_text(missing_call)
    rejected_output = scratch / "malformed-simplified.ac"
    run(
        args.optimizer,
        malformed,
        "--ac-simplify-linked-registrations",
        "-o",
        rejected_output,
        expected=1,
    )
    assert not rejected_output.exists()

    # Check the existing explicit guard and nested sibling Method calls using
    # their checked-in sources and ordinary independent unit publication.
    for example, top_name in (
        ("queue_rule_pipeline", "QueueRulePipelineSystem"),
        ("sibling_atomic", "SiblingAtomic"),
    ):
        package = "example_" + example
        directory = args.repo / "examples" / example
        definition = compile_unit(directory / (example + ".py"), package=package)
        bench = compile_unit(directory / "bench.py", [definition], package=package)
        before = scratch / (example + ".ac")
        link([definition, bench], package + ".bench." + top_name, before)
        after = simplify(before, example + "-simplified")
        assert call_inventory(before) == call_inventory(after)
        assert method_inventory(before) == method_inventory(after)
        after_elision = elide(after, example + "-elided").read_text()
        if example == "queue_rule_pipeline":
            text = after.read_text()
            assert 'ac.registration_name = "consume"' in text
            guard = method_guards(text)["consume"]
            assert '"ac.bits.compare"' in guard
            assert method_guards(after_elision)["consume"] == guard
        else:
            nested = call_inventory(after)
            assert any("::@increment" in key[0] for key in nested)
            assert any("::@record" in key[0] for key in nested)
            record = [
                next(
                    line
                    for line in text.splitlines()
                    if '"ac.method.call"' in line and "::@record," in line
                )
                for text in (after.read_text(), after_elision)
            ]
            operands = [
                re.search(r'"ac.method.call"\(([^)]*)\)', line)[1] for line in record
            ]
            _, record_argument = operands[0].split(", ")
            assert "ac.implicit_path" in record[1]
            assert operands[1] == record_argument

    invalid_registration = scratch / "duplicate-registration.ac"
    registrations = [
        line for line in simplified_text.splitlines() if "ac.registration_name" in line
    ]
    first_occurrence = registrations[0].split("occurrence = ", 1)[1].split("}>", 1)[0]
    second_occurrence = registrations[1].split("occurrence = ", 1)[1].split("}>", 1)[0]
    invalid_registration.write_text(
        simplified_text.replace(
            "occurrence = " + second_occurrence,
            "occurrence = " + first_occurrence,
            1,
        )
    )
    run(args.source_linker, "--verify-only", invalid_registration, expected=1)

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

#!/usr/bin/env python3
"""Check a freshly emitted RV32I t-gfsim Model using the existing EEI and oracle."""

import argparse
from collections import defaultdict
import json
from pathlib import Path
import time

from run_generated import execute, invoke
from verify import compare


def accessors(port):
    """Translate the published field order to t-gfsim record members."""
    members = defaultdict(list)
    for field in port["fields"]:
        path = field["path"]
        for index, name in enumerate(path):
            parent = tuple(path[:index])
            if name not in members[parent]:
                members[parent].append(name)

    for field in port["fields"]:
        path = field["path"]
        access = "".join(
            f".field_{members[tuple(path[:index])].index(name)}"
            for index, name in enumerate(path)
        )
        yield access, field["low"], field["width"]


def bridge_ports(ports):
    bus = next(port for port in ports["inputs"] if port["name"] == "bus")
    width = bus["width"]
    lines = [
        f"constexpr unsigned busWidth = {width};",
        "void driveBus(Model &model, std::string_view bus) {",
    ]
    for access, low, size in accessors(bus):
        lines.append(
            f"  model.in_bus{access} = unpack<{size}>(bus.substr({width-low-size}, {size}));"
        )
    lines.append("}")
    for name, port in zip(("pins", "state"), ports["outputs"], strict=True):
        lines.extend(
            [
                f"std::string {name}(const Model &model) {{",
                f"  std::string text({port['width']}, '0');",
            ]
        )
        for access, low, _ in accessors(port):
            lines.append(f"  pack(text, {low}, model.out_{port['name']}{access});")
        lines.extend(("  return text;", "}"))
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", type=Path, required=True, help="emitted model.h")
    parser.add_argument("--programs", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--include", type=Path, required=True)
    parser.add_argument("--cxx", required=True)
    args = parser.parse_args()
    if args.model.name != "model.h":
        parser.error("--model must name the generated model.h")
    verification = Path(__file__).resolve().parent
    ports_path = verification.parent / "generated/cpp/ports.json"
    ports = json.loads(ports_path.read_text())
    args.output.mkdir(parents=True)
    (args.output / "ports.h").write_text(bridge_ports(ports))
    executable = (args.output / "bridge").resolve()
    invoke(
        [
            args.cxx,
            "-std=c++20",
            "-O2",
            "-I" + str(args.include),
            "-I" + str(args.model.resolve().parent),
            "-I" + str(args.output),
            str(verification / "artifact_driver/transactional.cpp"),
            "-o",
            str(executable),
        ],
        args.output / "build.log",
    )
    for case, latency in (
        ("smoke", [0]),
        ("held_wb_store_data", [2]),
        ("c-array", [0]),
    ):
        program = args.programs / case
        config = json.loads((program / "simulation.json").read_text())
        config["image"] = str((program / config["image"]).resolve())
        config["data_latency"] = latency
        result = args.output / case
        started = time.monotonic()
        execute([str(executable)], ports, config, result)
        elapsed = time.monotonic() - started
        report = compare(program, result, ports_path)
        if case == "smoke":
            assert report["load_use_cycles"] == 1
        elif case == "held_wb_store_data":
            assert report["memory_wait_cycles"] >= 2
        execution = json.loads((result / "execution.json").read_text())
        report.update(
            elapsed_seconds=elapsed,
            cycles_per_second=execution["model_cycles"] / elapsed,
        )
        (result / "verification.json").write_text(json.dumps(report, indent=2) + "\n")
        print(case, report, flush=True)


if __name__ == "__main__":
    main()

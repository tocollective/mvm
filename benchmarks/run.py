#!/usr/bin/env python3
# Copyright 2026 tocollective contributors
# SPDX-License-Identifier: Apache-2.0
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import argparse
import json
import statistics
import subprocess
import sys
from pathlib import Path

SOURCES = (
    "arithmetic.m",
    "calls.m",
    "arrays.m",
    "strings.m",
    "gc.m",
    "codegen.m",
    "threads.m",
)


class BenchmarkFailure(RuntimeError):
    pass


def execute(command, timeout):
    try:
        result = subprocess.run(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=timeout,
            check=False,
        )
    except subprocess.TimeoutExpired as error:
        raise BenchmarkFailure(f"timeout after {timeout}s: {command}") from error
    if result.returncode != 0:
        raise BenchmarkFailure(
            f"command failed with status {result.returncode}: {command}\n"
            f"{result.stdout}{result.stderr}"
        )
    return result.stdout


def parse_output(output):
    records = {}
    for line in output.splitlines():
        if not line.startswith("BENCHMARK "):
            continue
        fields = {}
        for item in line.split()[1:]:
            key, separator, value = item.partition("=")
            if not separator or key in fields:
                raise BenchmarkFailure(f"invalid benchmark record: {line}")
            fields[key] = value
        if set(fields) != {"name", "iterations", "result", "ns"}:
            raise BenchmarkFailure(f"incomplete benchmark record: {line}")
        name = fields["name"]
        if not name or name in records:
            raise BenchmarkFailure(f"duplicate or empty benchmark name: {line}")
        try:
            records[name] = {
                "iterations": int(fields["iterations"]),
                "result": int(fields["result"]),
                "ns": int(fields["ns"]),
            }
        except ValueError as error:
            raise BenchmarkFailure(f"non-numeric benchmark record: {line}") from error
        if records[name]["iterations"] <= 0 or records[name]["ns"] < 0:
            raise BenchmarkFailure(f"invalid benchmark values: {line}")
    if not records:
        raise BenchmarkFailure("program produced no benchmark records")
    return records


def benchmark_program(source, args, root, work_dir):
    image = work_dir / f"{source.stem}.mvm"
    execute(
        [
            str(args.mc),
            str(source),
            "-I",
            str(root.parent / "mvm-c" / "examples"),
            "-I",
            str(root.parent / "mvm-lib"),
            "-o",
            str(image),
        ],
        args.timeout,
    )

    command = [str(args.mvm), str(image)]
    for _ in range(args.warmup):
        parse_output(execute(command, args.timeout))

    collected = {}
    for _ in range(args.runs):
        records = parse_output(execute(command, args.timeout))
        for name, record in records.items():
            if name not in collected:
                collected[name] = {
                    "iterations": record["iterations"],
                    "result": record["result"],
                    "samples_ns": [],
                }
            current = collected[name]
            if (current["iterations"] != record["iterations"] or
                    current["result"] != record["result"]):
                raise BenchmarkFailure(f"unstable result for {name}")
            current["samples_ns"].append(record["ns"])
    return collected


def positive(value):
    number = int(value)
    if number <= 0:
        raise argparse.ArgumentTypeError("value must be positive")
    return number


def nonnegative(value):
    number = int(value)
    if number < 0:
        raise argparse.ArgumentTypeError("value must be non-negative")
    return number


def main():
    root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description="Run the mvm benchmark suite")
    parser.add_argument("--mc", type=Path, default=root.parent / "mvm-c" / "bin" / "mvm-c")
    parser.add_argument("--mvm", type=Path, default=root / "bin" / "mvm")
    parser.add_argument("--work-dir", type=Path, default=root / "build" / "benchmarks")
    parser.add_argument("--runs", type=positive, default=5)
    parser.add_argument("--warmup", type=nonnegative, default=1)
    parser.add_argument("--timeout", type=positive, default=180)
    parser.add_argument("--filter", help="run only source names containing this text")
    parser.add_argument("--json", dest="json_path", type=Path, help="write raw samples as JSON")
    args = parser.parse_args()
    args.mc = args.mc.resolve()
    args.mvm = args.mvm.resolve()
    args.work_dir = args.work_dir.resolve()
    args.work_dir.mkdir(parents=True, exist_ok=True)

    results = {}
    try:
        for filename in SOURCES:
            if args.filter and args.filter not in filename:
                continue
            source = root / "benchmarks" / filename
            records = benchmark_program(source, args, root, args.work_dir)
            for name, record in records.items():
                if name in results:
                    raise BenchmarkFailure(f"duplicate benchmark across programs: {name}")
                results[name] = record
    except BenchmarkFailure as error:
        parser.exit(1, f"benchmark: error: {error}\n")

    if not results:
        parser.exit(1, "benchmark: error: no benchmarks selected\n")

    output = []
    print(f"{'benchmark':<24} {'median ms':>12} {'ns/op':>12} {'result':>16}")
    for name, record in results.items():
        median_ns = int(statistics.median(record["samples_ns"]))
        median_ms = median_ns / 1_000_000
        per_operation = median_ns / record["iterations"]
        print(f"{name:<24} {median_ms:>12.3f} {per_operation:>12.1f} "
              f"{record['result']:>16}")
        output.append({
            "name": name,
            "iterations": record["iterations"],
            "result": record["result"],
            "samples_ns": record["samples_ns"],
            "median_ns": median_ns,
        })

    if args.json_path:
        args.json_path.parent.mkdir(parents=True, exist_ok=True)
        args.json_path.write_text(json.dumps(output, indent=2) + "\n")


if __name__ == "__main__":
    main()

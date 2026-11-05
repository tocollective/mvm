<!--
Copyright 2026 tocollective contributors
SPDX-License-Identifier: Apache-2.0

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    https://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->

# mvm benchmarks

A permanent benchmark suite for the interpreter. Workloads are written in M,
check their own results, and time only the selected section with `time.mono()`.

## Workloads

- `arithmetic.m` — integer and floating-point loops;
- `calls.m` — direct and indirect function calls;
- `arrays.m` — writing and reading a fixed-size array;
- `strings.m` — concatenation, length, and code point access;
- `gc.m` — creating unreachable arrays and automatic collections;
- `codegen.m` — a `while` loop with branching, for-each over a fixed-size array, and tail
  recursion;
- `threads.m` — sequential thread creation with `thread<F, R>` and waiting
  for results with `worker.join(worker)`, with result validation.

## Running

From the repository root:

```sh
python3 benchmarks/run.py
```

Or through CMake after configuring the build:

```sh
cmake --build build --target benchmark
```

By default, the runner compiles each program into `build/benchmarks`, performs one
warmup and five measured runs. It prints the median, nanoseconds per operation, and checksum.

Useful options:

```sh
python3 benchmarks/run.py --runs 10 --warmup 2
python3 benchmarks/run.py --filter arithmetic
python3 benchmarks/run.py --json build/benchmarks/latest.json
```

`--mc`, `--mvm`, `--work-dir`, and `--timeout` allow comparing separate builds.
Use identical Release settings when comparing changes and save the JSON output.
Each workload must produce the same result in every run; otherwise the runner
exits with an error.

Format of a single record from an M program:

```text
BENCHMARK name=arithmetic_integer iterations=10000000 result=49999995000000 ns=...
```

The runner requires no third-party Python packages. CMake creates the `benchmark` target if
Python 3 is found during configuration; regular builds and tests do not run the suite.

## License

Copyright 2026 tocollective contributors.

Licensed under the Apache License, Version 2.0. See [LICENSE](../LICENSE) for the full license text.

Unless required by applicable law or agreed to in writing, software distributed
under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
CONDITIONS OF ANY KIND, either express or implied. See the License for the
specific language governing permissions and limitations under the License.

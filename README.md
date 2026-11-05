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

# MVM

MVM is a stack based virtual machine for the M programming language.

Build and run the tests (requires CMake, pkg-config, libffi, Flex and Bison):

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Print the build version and exit:

```sh
bin/mvm --version
```

Run the self-checking benchmark suite (median of five runs plus one warmup):

```sh
python3 benchmarks/run.py
```

See [benchmarks/README.md](benchmarks/README.md) for workloads, filtering and JSON output.

Run with an initial memory reserve and a maximum VM allocation budget:

```sh
bin/mvm --min-memory 1MB --max-memory 64MB program.mvm
```

VM options must precede the program path; arguments after it are passed to
the M program.

Compile with source locations and run with source-mapped stack traces:

```sh
../mvm-c/bin/mvm-c --debug program.m -o program.mvm
bin/mvm --debug program.mvm
```

Compile one module to MIR:

```sh
../mvm-c/bin/mvm-c -I ../mvm-c/examples ../mvm-c/examples/hello.m -o hello.mir
```

## License

Copyright 2026 tocollective contributors.

Licensed under the Apache License, Version 2.0. See [LICENSE](LICENSE) for the full license text.

Unless required by applicable law or agreed to in writing, software distributed
under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
CONDITIONS OF ANY KIND, either express or implied. See the License for the
specific language governing permissions and limitations under the License.

Bundled third-party license texts are included in
[THIRD_PARTY_LICENSES.txt](THIRD_PARTY_LICENSES.txt). The build copies both license
files alongside the executable in `bin/`.

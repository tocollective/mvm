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

"""Exercise CLI memory limits through real compiled programs."""
import argparse
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--mc', required=True)
parser.add_argument('--mvm', required=True)
parser.add_argument('--work-dir', required=True)
args = parser.parse_args()
work = Path(args.work_dir)
work.mkdir(parents=True, exist_ok=True)
lib = Path(__file__).resolve().parents[2] / 'mvm-lib'


def compile_program(name, source):
    path = work / f'{name}.m'
    image = path.with_suffix('.mvm')
    path.write_text(source)
    result = subprocess.run([args.mc, '-I', str(lib), str(path), '-o', str(image)],
                            capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, result.stderr
    return str(image)


def run(options, image=None, program_args=()):
    command = [args.mvm, *options]
    if image:
        command += [image, *program_args]
    return subprocess.run(command, capture_output=True, text=True, timeout=30)


hello = compile_program('hello', '''import { io } from "base/io.m"
let main(): Word {
    io.println("ran")
    return 0
}
''')
for suffix in ('', 'B', 'KB', 'kB', 'KiB', 'MB', 'm', 'MiB', 'GB', 'g', 'GiB'):
    value = '8388608' if suffix in ('', 'B') else ('8192' if suffix.lower() in
             ('kb', 'kib') else ('8' if suffix.lower() in
              ('mb', 'm', 'mib') else '1'))
    result = run(['--min-memory=0', '--max-memory', value + suffix], hello)
    assert result.returncode == 0 and result.stdout == 'ran\n', (suffix, result)
for invalid in ('', '-1', '+1', ' 1MB', '1 MB', '1.5MB', '1XB', '1MBx',
                '1КБ', '1МБ', '1ГБ', '1кб', '1мб', '1гб',
                '18446744073709551616', '18446744073709551615GB'):
    for flag in ('--min-memory', '--max-memory'):
        result = run([flag, invalid], hello)
        assert result.returncode != 0 and 'invalid value' in result.stderr, result
for options in (['--max-memory', '0'], ['--min-memory', '2MB', '--max-memory', '1MB']):
    result = run(options, hello)
    assert result.returncode != 0 and 'invalid memory limits' in result.stderr
for flag in ('--min-memory', '--max-memory'):
    result = run([flag])
    assert result.returncode != 0 and 'missing value' in result.stderr
for flag in ('--ram', '-r'):
    result = run([flag, '1048576'], hello)
    assert result.returncode != 0 and 'unknown option' in result.stderr
result = run(['--frames', '1MB'], hello)
assert result.returncode != 0 and 'invalid value' in result.stderr
result = run(['--min-memory', '0', '--max-memory', '1'], hello)
assert result.returncode != 0 and 'out of memory' in result.stderr and not result.stdout

# A live array must fail before allocating its enormous buffer, then report OOM.
oom = compile_program('oom', '''import { io } from "base/io.m"
let main(): Word {
    let values: Long[1024 * 1024 * 1024] = []
    io.println("unreachable")
    return length(values) as Word
}
''')
result = run(['--min-memory', '0', '--max-memory', '2MB'], oom)
assert result.returncode != 0 and 'out of memory' in result.stderr and not result.stdout

# GC must reclaim dead allocations while a similarly sized live set exceeds cap.
reuse = compile_program('reuse', '''import { io } from "base/io.m"
let allocate(): UWord {
    let data: Long[4096] = []
    return length(data)
}
let main(): Word {
    for i: UWord in 0..100 { allocate() }
    io.println("reuse ok")
    return 0
}
''')
result = run(['--min-memory', '0', '--max-memory', '1MB'], reuse)
assert result.returncode == 0 and result.stdout == 'reuse ok\n', result

# The former fixed 1 MiB instruction/rodata buffer no longer limits an image.
strings = ['x' * 8192 + str(i) for i in range(140)]
text = ''.join(item + '\n' for item in strings)
large = compile_program('large', 'import { io } from "base/io.m"\n'
    'let main(): Word {\n' +
    '\n'.join('io.println("' + item + '")' for item in strings) +
    '\n return 0\n }\n')
result = run(['--min-memory', '0', '--max-memory', '32MB'], large)
assert result.returncode == 0 and result.stdout == text, result.stderr
result = run(['--min-memory', '0', '--max-memory', '256KB'], large)
assert result.returncode != 0 and 'out of memory' in result.stderr

# Argument allocation failures must not be overwritten by a subsequent mvm_run.
entry = compile_program('args', '''import { io } from "base/io.m"
let main(args: String[]): Word {
    io.println("entry ran")
    return 0
}
''')
result = run(['--min-memory', '0', '--max-memory', '256KB'], entry, ['x'] * 20000)
assert result.returncode != 0 and 'out of memory' in result.stderr and not result.stdout
print('memory CLI: OK')

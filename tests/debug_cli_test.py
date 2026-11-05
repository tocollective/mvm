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

"""Exercise debug metadata, diagnostics and BREAKPOINT through real programs."""
import argparse
from pathlib import Path
import struct
import subprocess
import zipfile

parser = argparse.ArgumentParser()
parser.add_argument('--mc', required=True)
parser.add_argument('--mvm', required=True)
parser.add_argument('--work-dir', required=True)
args = parser.parse_args()
work = Path(args.work_dir)
work.mkdir(parents=True, exist_ok=True)
root = Path(__file__).resolve().parents[2]
lib = root / 'mvm-lib'
examples = root / 'mvm-c' / 'examples'


def compile_program(name, source, debug=True, extension='.mvm'):
    path = work / f'{name}.m'
    output = path.with_suffix(extension)
    path.write_text(source)
    command = [args.mc]
    if debug:
        command.append('--debug')
    command += ['-I', str(lib), '-I', str(examples), str(path), '-o', str(output)]
    result = subprocess.run(command, capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, result.stderr
    return output


def run(image, debug=True):
    command = [args.mvm]
    if debug:
        command.append('-d')
    command.append(str(image))
    return subprocess.run(command, capture_output=True, text=True, timeout=30)


def sections(data):
    assert data[:8] == b'MVMO\r\n\x1a\n'
    table = struct.unpack_from('<Q', data, 32)[0]
    count = struct.unpack_from('<I', data, 40)[0]
    result = {}
    for i in range(count):
        kind, flags, offset, size, entry_size, entry_count = struct.unpack_from(
            '<IIQQII', data, table + i * 32)
        result[kind] = (offset, size, entry_size, entry_count)
    return result


def rewrite_image(source, target, mutate, isa=None):
    with zipfile.ZipFile(source) as old, zipfile.ZipFile(target, 'w') as new:
        for info in old.infolist():
            data = old.read(info.filename)
            if info.filename.endswith('.mo'):
                data = bytearray(data)
                mutate(data)
                if isa is not None:
                    struct.pack_into('<H', data, 14, isa)
                data = bytes(data)
            elif info.filename == 'manifest.toml' and isa is not None:
                data = data.replace(b'MVM-ISA-Version = "2.4"',
                                    f'MVM-ISA-Version = "2.{isa}"'.encode())
            new.writestr(info, data)


for executable in (args.mc, args.mvm):
    result = subprocess.run([executable, '--help'], capture_output=True, text=True)
    assert result.returncode == 0 and '-d, --debug' in result.stdout, result

source = '''let main(): Word {
    let answer: Word = 42
    return answer
}
'''
mir = compile_program('debug-lines', source, extension='.mir')
mir_text = mir.read_text()
assert '.loc ' in mir_text
assert '.var 0 "answer": Word' in mir_text
plain = compile_program('plain-lines', source, debug=False, extension='.mo')
plain_sections = sections(plain.read_bytes())
assert 12 not in plain_sections and 13 not in plain_sections
debug = compile_program('debug-lines', source, extension='.mo')
debug_data = debug.read_bytes()
debug_sections = sections(debug_data)
line_section = debug_sections[12]
assert line_section[2] == 24 and line_section[3] > 0
first_line = struct.unpack_from('<IIIIII', debug_data, line_section[0])
assert first_line[0] == 0 and first_line[4] > 0
variable_section = debug_sections[13]
assert variable_section[2] == 20 and variable_section[3] == 1
variable = struct.unpack_from('<IIIII', debug_data, variable_section[0])
strings = debug_sections[2]
name = debug_data[strings[0] + variable[2]:strings[0] + variable[2] + variable[3]]
assert variable[:2] == (0, 0) and name == b'answer'

error = compile_program('debug-error', '''let fail(values: Word[]): Word {
    return values[1]
}

let main(): Word {
    let values: Word[0] = []
    return fail(values)
}
''')
result = run(error)
assert result.returncode != 0
assert 'at fail (' in result.stderr and 'debug-error.m:2:12' in result.stderr
assert 'at main (' in result.stderr and 'debug-error.m:7:12' in result.stderr
assert result.stderr.index('at fail') < result.stderr.index('at main')
result = run(error, debug=False)
assert result.returncode != 0 and '[DEBUG]' not in result.stderr

breakpoint_source = '''let stop(): Void {
    breakpoint()
}

let main(): Word {
    stop()
    return 0
}
'''
breakpoint = compile_program('debug-breakpoint', breakpoint_source)
result = run(breakpoint)
assert result.returncode != 0 and '[BREAKPOINT]: opcode 94' in result.stderr
assert 'debug-breakpoint.m:2:5' in result.stderr
assert 'at stop' in result.stderr and 'at main' in result.stderr
result = subprocess.run([args.mvm, '--debug', str(breakpoint)],
                        capture_output=True, text=True, timeout=30)
assert result.returncode != 0 and '[BREAKPOINT]' in result.stderr

without_lines = compile_program('plain-breakpoint', breakpoint_source, debug=False)
result = run(without_lines)
assert result.returncode != 0 and '[BREAKPOINT]' in result.stderr
assert 'at stop' in result.stderr and 'at main' in result.stderr
assert 'plain-breakpoint.m:' not in result.stderr

worker = compile_program('debug-worker', '''import { spawn, join } from "std/thread.m"

let stop(): Void {
    breakpoint()
}

let main(): Word {
    let worker: Thread = spawn::<(): Void>(stop)
    join::<Void>(worker)
    return 0
}
''')
result = run(worker)
assert result.returncode != 0 and '[BREAKPOINT]' in result.stderr
assert 'at stop' in result.stderr and 'debug-worker.m:4:5' in result.stderr


def invalid_offset(data):
    table = sections(data)[12]
    code_offset = struct.unpack_from('<I', data, table[0] + 4)[0]
    struct.pack_into('<I', data, table[0] + 4, code_offset + 1)


malformed = work / 'debug-invalid-offset.mvm'
rewrite_image(breakpoint, malformed, invalid_offset)
result = run(malformed)
assert result.returncode != 0
assert 'debug line does not target an instruction' in result.stderr


def no_change(data):
    pass


old_isa = work / 'debug-breakpoint-isa21.mvm'
rewrite_image(breakpoint, old_isa, no_change, isa=1)
result = run(old_isa)
assert result.returncode != 0 and (
    'invalid or truncated instruction' in result.stderr or
    'invalid primitive type or ISA version' in result.stderr)
print('debug CLI: OK')

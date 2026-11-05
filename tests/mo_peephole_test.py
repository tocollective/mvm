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
import struct
import subprocess
import zipfile
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('--mc', required=True)
parser.add_argument('--mvm', required=True)
parser.add_argument('--work-dir', required=True)
args = parser.parse_args()
work = Path(args.work_dir)
work.mkdir(parents=True, exist_ok=True)
source = work / 'peephole.m'
image = work / 'peephole.mvm'
source.write_text('''let main(): Word {
    let value: Bool = false
    if value {}
    return 0
}
''')

result = subprocess.run([args.mc, str(source), '-o', str(image)],
                        capture_output=True, text=True, timeout=30)
assert result.returncode == 0, result.stderr
result = subprocess.run([args.mvm, str(image)], capture_output=True, text=True,
                        timeout=30)
assert result.returncode == 0, result.stderr

with zipfile.ZipFile(image) as archive:
    module_name = next(name for name in archive.namelist()
                       if name.startswith('modules/') and name.endswith('.mo'))
    data = archive.read(module_name)

assert data[:8] == b'MVMO\r\n\x1a\n'
table = struct.unpack_from('<Q', data, 32)[0]
section_count = struct.unpack_from('<I', data, 40)[0]
sections = {}
for i in range(section_count):
    kind, flags, offset, size, entry_size, entry_count = struct.unpack_from(
        '<IIQQII', data, table + i * 32)
    sections[kind] = (offset, size, entry_size, entry_count)

function_offset, function_size, entry_size, function_count = sections[5]
assert entry_size == 36 and function_count == 1
function = data[function_offset:function_offset + function_size]
code_offset = struct.unpack_from('<I', function, 12)[0]
code_size = struct.unpack_from('<I', function, 16)[0]
code = sections[9]
instructions = data[code[0] + code_offset:code[0] + code_offset + code_size]
assert instructions == bytes((23, 23, 9, 0, 0, 8, 0, 0, 26, 18, 0, 0, 0, 0, 0)), instructions.hex()
print('MO peephole: OK')

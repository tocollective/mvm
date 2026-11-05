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
parser.add_argument('--source', required=True)
parser.add_argument('--work-dir', required=True)
args = parser.parse_args()
work = Path(args.work_dir)
work.mkdir(parents=True, exist_ok=True)
image = work / 'local-jump.mvm'
result = subprocess.run([args.mc, args.source, '-o', str(image)],
                        capture_output=True, text=True, timeout=30)
assert result.returncode == 0, result.stderr
result = subprocess.run([args.mvm, str(image)], capture_output=True, text=True,
                        timeout=30)
assert result.returncode == 0, result.stderr


def instruction_size(op, data, offset):
    if op in (1, 2):
        return 6
    if op in (3, 5, 6, 7, 10, 11, 25):
        return 5
    if op == 4:
        return 2
    if op in (8, 9, 65, 66, 67, 92, 93):
        return 3
    if 12 <= op <= 19:
        return 1 + (1 << ((op - 12) % 4))
    if op == 20:
        return 5
    if op == 21 or op == 86:
        return 9
    if op in (50, 52, 53, 58, 59, 61):
        return 2
    if op == 57:
        return 4
    if op == 78:
        return 5 + struct.unpack_from('<I', data, offset + 1)[0] * 4
    if op == 95:
        return 11
    if op in (68, 69, 70, 81, 82, 83, 84, 85, 88, 89, 90, 91):
        return 5
    return 1


def module_from(image_path):
    with zipfile.ZipFile(image_path) as archive:
        name = next(name for name in archive.namelist()
                    if name.startswith('modules/') and name.endswith('.mo'))
        return archive.read(name)


def sections(data):
    table = struct.unpack_from('<Q', data, 32)[0]
    count = struct.unpack_from('<I', data, 40)[0]
    result = {}
    for i in range(count):
        kind, flags, offset, size, entry_size, entry_count = struct.unpack_from(
            '<IIQQII', data, table + i * 32)
        result[kind] = (offset, size, entry_size, entry_count)
    return result


data = module_from(image)
section = sections(data)
function = section[5][0]
code_offset, code_size = struct.unpack_from('<II', data, function + 12)
code = section[9][0] + code_offset
offset = 0
local_jumps = 0
first_local_jump = None
while offset < code_size:
    op = data[code + offset]
    if op == 95:
        if first_local_jump is None:
            first_local_jump = offset
        local_jumps += 1
    size = instruction_size(op, data, code + offset)
    assert size > 0 and offset + size <= code_size
    offset += size
assert offset == code_size and local_jumps >= 6, local_jumps


def rewrite(target, isa=None, invalid_comparison=False):
    with zipfile.ZipFile(image) as old, zipfile.ZipFile(target, 'w') as new:
        for info in old.infolist():
            content = old.read(info.filename)
            if info.filename.endswith('.mo'):
                content = bytearray(content)
                if isa is not None:
                    struct.pack_into('<H', content, 14, isa)
                if invalid_comparison:
                    code_section = sections(content)[9][0]
                    assert first_local_jump is not None
                    content[code_section + code_offset + first_local_jump + 5] = 0
                content = bytes(content)
            elif info.filename == 'manifest.toml' and isa is not None:
                content = content.replace(b'MVM-ISA-Version = "2.4"',
                                          b'MVM-ISA-Version = "2.3"')
            new.writestr(info, content)


old_image = work / 'local-jump-isa23.mvm'
rewrite(old_image, isa=3)
result = subprocess.run([args.mvm, old_image], capture_output=True, text=True,
                        timeout=30)
assert result.returncode != 0 and 'invalid or truncated instruction' in result.stderr

invalid_image = work / 'local-jump-invalid.mvm'
rewrite(invalid_image, invalid_comparison=True)
result = subprocess.run([args.mvm, invalid_image], capture_output=True, text=True,
                        timeout=30)
assert result.returncode != 0
print('local jump: OK')

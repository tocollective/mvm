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

"""Exercise the loader with valid ZIPs containing deliberately invalid MO data."""
import argparse
import pathlib
import struct
import subprocess
import zipfile


def u32(data, offset):
    return struct.unpack_from('<I', data, offset)[0]


def put32(data, offset, value):
    struct.pack_into('<I', data, offset, value)


def sections(data):
    table = struct.unpack_from('<Q', data, 32)[0]
    result = {}
    for i in range(u32(data, 40)):
        offset = table + i * 32
        tag = u32(data, offset)
        start, size = struct.unpack_from('<QQ', data, offset + 8)
        result[tag] = (start, size, u32(data, offset + 28))
    return result


def records(data, section, header=12):
    start, size, count = section
    cursor = start
    result = []
    for _ in range(count):
        length = u32(data, cursor)
        result.append((cursor + header, length))
        cursor += header + length
    assert cursor == start + size
    return result


def run(command):
    return subprocess.run(command, capture_output=True, text=True, timeout=15)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--mc', required=True)
    parser.add_argument('--mvm', required=True)
    parser.add_argument('--work-dir', type=pathlib.Path, required=True)
    args = parser.parse_args()
    args.work_dir.mkdir(parents=True, exist_ok=True)
    source = args.work_dir / 'seed.m'
    source.write_text('''type A = Word | String
        type B = Bool | String
        type Box { value: A }
        let global: A = 1
        let other: B = true
        let main(): Word {
            let value: A = 2
            switch variantof(value) {
                case typeof(Word): break
                default: return 1
            }
            return 0
        }
    ''', encoding='utf-8')
    seed = args.work_dir / 'seed.mvm'
    compiled = run([args.mc, str(source), '-o', str(seed)])
    assert compiled.returncode == 0, compiled.stderr
    assert run([args.mvm, str(seed)]).returncode == 0
    with zipfile.ZipFile(seed) as archive:
        entries = {info.filename: archive.read(info) for info in archive.infolist()}
    object_name = next(name for name in entries if name.endswith('.mo'))
    original = entries[object_name]
    sec = sections(original)
    types = records(original, sec[3])
    union_offset = next(offset for offset, _ in types if original[offset] == 0x26)
    word = next(i for i, (offset, _) in enumerate(types) if original[offset] == 0x08)
    boolean = next(i for i, (offset, _) in enumerate(types) if original[offset] == 0x01)
    aggregate = next(i for i, (offset, _) in enumerate(types) if original[offset] == 0x23)
    union_id = next(i for i, (offset, _) in enumerate(types) if offset == union_offset)
    constant = next(offset for offset, _ in records(original, sec[8], header=4) if original[offset] == 0x28)
    functions, _, _ = sec[5]
    code, code_size, _ = sec[9]
    new_offset = original.index(bytes([86]), code, code + code_size) - code
    relocs, _, reloc_count = sec[11]
    variant_reloc = next(relocs + i * 16 for i in range(reloc_count)
                         if u32(original, relocs + i * 16) == new_offset + 5)

    mutations = {
        'excessive_variants': lambda data: put32(data, union_offset + 1, 0xffffffff),
        'invalid_variant_id': lambda data: put32(data, union_offset + 5, 0xffffffff),
        'duplicate_variant': lambda data: put32(data, union_offset + 9, u32(data, union_offset + 5)),
        'void_variant': lambda data: put32(data, union_offset + 5, 0),
        'nested_union': lambda data: put32(data, union_offset + 5, union_id),
        'recursive_value': lambda data: put32(data, union_offset + 5, aggregate),
        'wrong_payload_type': lambda data: put32(data, constant + 5, boolean),
        'unknown_opcode': lambda data: data.__setitem__(code, 255),
        'truncated_function': lambda data: put32(data, functions + 16, 2),
        'invalid_new_variant': lambda data: put32(data, variant_reloc + 8, boolean),
        'unsupported_version': lambda data: data.__setitem__(8, 9),
    }
    assert word != boolean
    for name, mutate in mutations.items():
        changed = bytearray(original)
        mutate(changed)
        path = args.work_dir / f'{name}.mvm'
        with zipfile.ZipFile(path, 'w') as archive:
            for entry, contents in entries.items():
                archive.writestr(entry, changed if entry == object_name else contents)
        result = run([args.mvm, str(path)])
        assert result.returncode > 0, f'{name}: accepted or crashed: {result.returncode}: {result.stderr}'
        assert 'AddressSanitizer' not in result.stderr and 'runtime error:' not in result.stderr, result.stderr
        assert 'mvm image:' in result.stderr or 'failed to create mvm' in result.stderr, result.stderr
    print(f'{len(mutations)} malformed union images rejected')


if __name__ == '__main__':
    main()

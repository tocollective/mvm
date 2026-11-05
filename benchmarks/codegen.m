/*
 * Copyright 2026 tocollective contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

import { time } from "base/time.m"
import { report } from "support.m"

let countdown(n: UWord, total: ULong): ULong {
    if n == 0 return total
    return countdown(n - 1, total + n as ULong)
}

let main(): Word {
    let mut iterations: UWord = 0
    let mut branches: ULong = 0
    let mut start: ULong = time.mono()
    while iterations < 5000_000 {
        iterations += 1
        if (iterations & 3) == 0 branches += 1
    }
    let mut elapsed: ULong = time.mono() - start
    if iterations != 5000000 || branches != 1250000 return 1
    report("codegen_while", 5000000, branches, elapsed)

    let values: ULong[1024] = []
    for i: UWord in 0..1024 values[i] = i as ULong
    let mut total: ULong = 0
    start = time.mono()
    for round: UWord in 0..2000 {
        for value: ULong in values total += value
    }
    elapsed = time.mono() - start
    if total != 1047552000 return 2
    report("codegen_for_each", 2048000, total, elapsed)

    start = time.mono()
    total = countdown(1000_000, 0)
    elapsed = time.mono() - start
    if total != 500000500000 return 3
    report("codegen_tail_call", 1000000, total, elapsed)
    return 0
}

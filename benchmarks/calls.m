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

type Operation = (value: Long): Long

let increment(value: Long): Long {
    return value + 1
}

let main(): Word {
    let mut directResult: Long = 0
    let mut start: ULong = time.mono()
    for i: ULong in 0..2000_000 {
        directResult = increment(directResult)
    }
    let mut elapsed: ULong = time.mono() - start
    if directResult != 2000000 return 1
    report("calls_direct", 2000000, directResult as ULong, elapsed)

    let operations: Operation[] = [increment]
    let mut indirectResult: Long = 0
    start = time.mono()
    for i: ULong in 0..500_000 {
        indirectResult = operations[0](indirectResult)
    }
    elapsed = time.mono() - start
    if indirectResult != 500000 return 2
    report("calls_indirect", 500000, indirectResult as ULong, elapsed)
    return 0
}

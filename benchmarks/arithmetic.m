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

let main(): Word {
    let mut integerResult: ULong = 0
    let mut start: ULong = time.mono()
    for i: Long in 0..1000_0000 {
        integerResult += i as ULong
    }
    let mut elapsed: ULong = time.mono() - start
    if integerResult != 49999995000000 return 1
    report("arithmetic_integer", 10000000, integerResult, elapsed)

    let mut floatResult: Double = 0.0
    start = time.mono()
    for i: Long in 0..1000_000 {
        floatResult += (i as Double) * 0.5
    }
    elapsed = time.mono() - start
    let checksum: ULong = floatResult as ULong
    if checksum != 249999750000 return 2
    report("arithmetic_float", 1000000, checksum, elapsed)
    return 0
}

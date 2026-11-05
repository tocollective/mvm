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
    let mut values: ULong[1024] = []
    let mut checksum: ULong = 0
    let start: ULong = time.mono()
    for round: ULong in 0..2000 {
        for i: UWord in 0..1024 {
            values[i] = i as ULong + round
        }
    }
    for i: UWord in 0..1024 {
        checksum += values[i]
    }
    let elapsed: ULong = time.mono() - start
    if checksum != 2570752 return 1
    report("arrays_indexing", 2048000, checksum, elapsed)
    return 0
}

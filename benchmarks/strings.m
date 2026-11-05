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
    let mut checksum: ULong = 0
    let start: ULong = time.mono()
    for i: ULong in 0..100_000 {
        let text: String = "mvm" + "✓"
        checksum += length(text) as ULong
        checksum += text[0] as ULong
    }
    let elapsed: ULong = time.mono() - start
    if checksum != 11300000 return 1
    report("strings_concat_access", 100000, checksum, elapsed)
    return 0
}

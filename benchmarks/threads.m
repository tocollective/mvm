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
import { BaseThread, thread } from "base/thread.m"
import { report } from "support.m"

let twice(value: ULong): ULong {
    return value * 2
}

let main(): Word {
    let mut result: ULong = 0
    let start: ULong = time.mono()
    for i: ULong in 0..2000 {
        let worker: BaseThread<ULong> = thread::<(value: ULong): ULong, ULong>(twice, i)
        result = worker.join(worker)
        if result != i * 2 return 1
    }
    let elapsed: ULong = time.mono() - start
    if result != 3998 return 2
    report("threads_spawn_join", 2000, result, elapsed)
    return 0
}

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

#include "internal.h"

#include <stdio.h>

void mvm_set_debug(mvm_t *mvm, bool enabled) {
  if (mvm)
    mvm->debug = enabled;
}

static mvm_function_type_t *debug_function_at(mvm_t *mvm, size_t pc) {
  size_t low = 0;
  size_t high = mvm->function_type_count;
  while (low < high) {
    const size_t middle = low + (high - low) / 2;
    mvm_function_type_t *function = &mvm->function_types[middle];
    if (pc < function->address) {
      high = middle;
    } else if (pc - function->address >= function->code_size) {
      low = middle + 1;
    } else {
      return function;
    }
  }
  return NULL;
}

static const mvm_debug_line_t *debug_line_at(mvm_function_type_t *function,
                                             size_t pc) {
  if (!function)
    return NULL;
  size_t low = 0;
  size_t high = function->debug_line_count;
  while (low < high) {
    const size_t middle = low + (high - low) / 2;
    if (function->debug_lines[middle].pc <= pc)
      low = middle + 1;
    else
      high = middle;
  }
  return low ? &function->debug_lines[low - 1] : NULL;
}

static void debug_print_frame(mvm_t *mvm, mvm_function_type_t *function,
                              size_t pc) {
  if (!function)
    function = debug_function_at(mvm, pc);
  const mvm_debug_line_t *line = debug_line_at(function, pc);
  const char *name = function && function->name ? function->name : NULL;
  if (name && line)
    fprintf(stderr, "  at %s (%s:%u", name, line->file, line->line);
  else if (name)
    fprintf(stderr, "  at %s (pc 0x%zX", name, pc);
  else if (line)
    fprintf(stderr, "  at %s:%u", line->file, line->line);
  else
    fprintf(stderr, "  at pc 0x%zX", pc);
  if (line && line->column)
    fprintf(stderr, ":%u", line->column);
  if (name || line)
    fputs(")", stderr);
  fputc('\n', stderr);
}

void mvm_debug_report(mvm_t *mvm, mvm_execution_t *execution,
                      mvm_result_t status, mvm_opcodes_t op,
                      size_t instruction_pc) {
  if (!mvm || !mvm->debug)
    return;
  mvm_runtime_lock(mvm);
  if (status == MVM_RESULT_BREAKPOINT)
    fprintf(stderr, "[BREAKPOINT]: opcode %i at %zX\n", op, instruction_pc);
  else
    fprintf(stderr, "[DEBUG]: stack trace for error %i at %zX\n", status,
            instruction_pc);
  debug_print_frame(mvm, mvm_execution_function_for(mvm, execution),
                    instruction_pc);
  mvm_frame_t *frames = mvm_execution_frames_for(mvm, execution);
  const size_t count = *mvm_execution_frame_count_for(mvm, execution);
  for (size_t i = count; i; --i) {
    const mvm_frame_t *frame = &frames[i - 1];
    debug_print_frame(mvm, frame->caller_function, frame->call_pc);
  }
  mvm_runtime_unlock(mvm);
}

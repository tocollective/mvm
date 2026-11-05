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

#include "main.h"

config_t config = {
    .minimum_memory = MVM_MEMORY_DEFAULT_MIN,
    .maximum_memory = MVM_MEMORY_DEFAULT_MAX,
    .frame_limit = MVM_FRAME_DEFAULT_LIMIT,
    .version = MVM_VERSION,
};

static void help(FILE *output, const char *program) {
  fprintf(
      output,
      "mvm %s\n\n"
      "usage: %s [options] <program.mvm> [arguments...]\n\n"
      "Run an mvm program container.\n"
      "VM options must precede the program path; arguments after it are\n"
      "passed to the program.\n\n"
      "Options:\n"
      "  -h, --help              Show this help and exit.\n"
      "  -v, --version           Show the version and exit.\n"
      "  --min-memory <size>     Initial VM memory reserve (default: 1MB).\n"
      "  --max-memory <size>     Maximum VM memory pool (default: 256MB).\n"
      "  -f, --frames <count>    Maximum call depth (default: %zu).\n"
      "  -d, --debug             Show source locations and call stacks.\n"
      "",
      config.version, program, (size_t)MVM_FRAME_DEFAULT_LIMIT);
}

static bool suffix_equal(const char *text, const char *suffix) {
  while (*text && *suffix) {
    if (tolower((unsigned char)*text++) != (unsigned char)*suffix++)
      return false;
  }
  return !*text && !*suffix;
}

// Parse decimal integers without signs, whitespace or overflow. Unit parsing
// is deliberately separate from --frames, which only accepts a plain count.
static bool parse_size(const char *text, bool units, size_t *out) {
  if (!text || text[0] < '0' || text[0] > '9')
    return false;
  size_t value = 0;
  while (*text >= '0' && *text <= '9') {
    const size_t digit = (size_t)(*text++ - '0');
    if (value > (SIZE_MAX - digit) / 10)
      return false;
    value = value * 10 + digit;
  }
  unsigned power = 0;
  if (*text) {
    if (!units)
      return false;
    if (suffix_equal(text, "b"))
      power = 0;
    else if (suffix_equal(text, "k") || suffix_equal(text, "kb") ||
             suffix_equal(text, "kib"))
      power = 1;
    else if (suffix_equal(text, "m") || suffix_equal(text, "mb") ||
             suffix_equal(text, "mib"))
      power = 2;
    else if (suffix_equal(text, "g") || suffix_equal(text, "gb") ||
             suffix_equal(text, "gib"))
      power = 3;
    else
      return false;
  }
  for (unsigned i = 0; i < power; ++i) {
    if (value > SIZE_MAX / 1024)
      return false;
    value *= 1024;
  }
  *out = value;
  return true;
}

static bool parse_size_option(int argc, char *argv[], int *i,
                              const char *inline_value, bool units,
                              size_t *out) {
  const char *option = argv[*i];
  if (!inline_value && *i + 1 >= argc) {
    fprintf(stderr, "missing value for %s\n", option);
    return false;
  }
  const char *value = inline_value ? inline_value : argv[++*i];
  if (!parse_size(value, units, out)) {
    fprintf(stderr, "invalid value for %s: %s\n", option, value);
    return false;
  }
  return true;
}

int main(int argc, char *argv[]) {
  char *binary_path = NULL;
  int program_index = 0;
  for (int i = 1; i < argc; ++i) {
    const char *arg = argv[i];
    if (!strcmp(arg, "-h") || !strcmp(arg, "--help")) {
      help(stdout, argv[0]);
      return EXIT_SUCCESS;
    } else if (!strcmp(arg, "-v") || !strcmp(arg, "--version")) {
      printf("mvm %s\n", config.version);
      return EXIT_SUCCESS;
    } else if (!strcmp(arg, "--min-memory") ||
               !strncmp(arg, "--min-memory=", 13)) {
      if (!parse_size_option(argc, argv, &i, arg[12] == '=' ? arg + 13 : NULL,
                             true, &config.minimum_memory))
        return EXIT_FAILURE;
    } else if (!strcmp(arg, "--max-memory") ||
               !strncmp(arg, "--max-memory=", 13)) {
      if (!parse_size_option(argc, argv, &i, arg[12] == '=' ? arg + 13 : NULL,
                             true, &config.maximum_memory))
        return EXIT_FAILURE;
    } else if (!strcmp(arg, "-f") || !strcmp(arg, "--frames")) {
      if (!parse_size_option(argc, argv, &i, NULL, false, &config.frame_limit))
        return EXIT_FAILURE;
    } else if (!strcmp(arg, "-d") || !strcmp(arg, "--debug")) {
      config.debug = true;
    } else if (!strcmp(arg, "--")) {
      if (++i < argc) {
        binary_path = argv[i];
        program_index = i;
      }
      break;
    } else if (arg[0] == '-') {
      fprintf(stderr, "unknown option: %s\n", arg);
      return EXIT_FAILURE;
    } else {
      binary_path = argv[i];
      program_index = i;
      break;
    }
  }
  if (!config.maximum_memory || config.minimum_memory > config.maximum_memory) {
    fprintf(stderr, "invalid memory limits: require 0 <= min-memory <= "
                    "max-memory and max-memory > 0\n");
    return EXIT_FAILURE;
  }
  if (!binary_path) {
    help(stderr, argv[0]);
    return EXIT_FAILURE;
  }

  const size_t allocation_failures = mvm_memory_failures();
  mvm_t *mvm = mvm_create(binary_path, config.minimum_memory,
                          config.maximum_memory, config.frame_limit);
  if (!mvm) {
    if (mvm_memory_failures() != allocation_failures)
      fprintf(stderr,
              "mvm: out of memory while creating instance "
              "(maximum: %zu bytes)\n",
              config.maximum_memory);
    else
      fprintf(stderr, "failed to create mvm\n");
    return EXIT_FAILURE;
  }
  mvm_set_debug(mvm, config.debug);
  mvm_result_t result = MVM_RESULT_OK;
  const size_t argument_failures = mvm_memory_failures();
  if (mvm->entry_argc == 1) {
    const size_t argument_count = (size_t)(argc - program_index);
    mvm_value_t arguments;
    result = mvm_array_new(mvm, MVM_KIND_STRING, argument_count, &arguments);
    if (result == MVM_RESULT_OK && !mvm_stack_push(mvm->stack, arguments))
      result = MVM_RESULT_VM_ERROR;
    for (size_t i = 0; i < argument_count && result == MVM_RESULT_OK; ++i)
      result =
          mvm_array_set(mvm, arguments, i,
                        (mvm_value_t){.kind = MVM_KIND_STRING,
                                      .as.s = argv[(size_t)program_index + i]});
  }
  if (result == MVM_RESULT_VM_ERROR &&
      mvm_memory_failures() != argument_failures)
    result = MVM_RESULT_OUT_OF_MEMORY;
  if (result == MVM_RESULT_OK)
    result = mvm_run(mvm);
  if (result == MVM_RESULT_OUT_OF_MEMORY)
    fprintf(stderr, "mvm: out of memory at pc 0x%zX (maximum: %zu bytes)\n",
            mvm->pc, config.maximum_memory);
  else if (result == MVM_RESULT_BREAKPOINT)
    fprintf(stderr, "mvm stopped at breakpoint\n");
  else if (result != MVM_RESULT_OK)
    fprintf(stderr, "mvm failed with error %i at pc 0x%zX\n", result, mvm->pc);

  int exit_status = EXIT_FAILURE;
  if (result == MVM_RESULT_OK && mvm->stack->size &&
      mvm->stack->data[mvm->stack->size - 1].kind == MVM_KIND_SWORD)
    exit_status = mvm->stack->data[mvm->stack->size - 1].as.sw;
  else if (result == MVM_RESULT_OK)
    fprintf(stderr, "mvm entry returned an invalid result\n");
  mvm_destroy(mvm);
  return exit_status;
}

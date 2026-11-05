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

#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "internal.h"

#include "tomlc17.h"
#include "zip.h"

#include <errno.h>
#include <float.h>
#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif

#if defined(__APPLE__) && defined(__MACH__)
#include <TargetConditionals.h>
#endif

#define IMAGE_MAX_ENTRIES 4096
#define IMAGE_MAX_MANIFEST (64 * 1024)
#define IMAGE_MAX_OBJECT (256ULL * 1024 * 1024)
#define IMAGE_MAX_OBJECTS (1024ULL * 1024 * 1024)

#define MO_MODULE 1
#define MO_STRINGS 2
#define MO_TYPES 3
#define MO_GLOBALS 4
#define MO_FUNCTIONS 5
#define MO_IMPORTS 6
#define MO_EXPORTS 7
#define MO_CONSTANTS 8
#define MO_CODE 9
#define MO_RODATA 10
#define MO_RELOC 11
#define MO_DEBUG_LINES 12
#define MO_DEBUG_VARIABLES 13

#define MO_IMPORT_FUNCTION 1
#define MO_IMPORT_GLOBAL 2
#define MO_IMPORT_TYPE 3
#define MO_IMPORT_NATIVE 4

typedef struct ImageLoader ImageLoader;

typedef struct ImageSection {
  const unsigned char *data;
  size_t size;
  uint32_t count;
} ImageSection;

typedef struct ImageType {
  const unsigned char *body;
  size_t size;
  char *name;
  uint32_t id;
  uint64_t hash;
  uint8_t hash_state;
  struct ImageModule *owner;
  uint32_t local_index;
  struct ImageType *canonical_next;
} ImageType;

typedef struct ImageGlobal {
  char *name;
  uint32_t type;
  uint32_t constant;
  uint32_t flags;
  uint32_t id;
  uint8_t state;
} ImageGlobal;

typedef struct ImageFunction {
  char *name;
  uint32_t type;
  uint32_t code_offset;
  uint32_t code_size;
  uint16_t local_count;
  uint32_t max_stack;
  uint32_t address;
} ImageFunction;

typedef struct ImageImport {
  char *module;
  char *name;
  uint8_t kind;
  uint32_t type;
  struct ImageModule *target_module;
  uint32_t target_index;
  uint16_t native_id;
} ImageImport;

typedef struct ImageExport {
  char *name;
  uint8_t kind;
  uint32_t index;
  uint32_t flags;
} ImageExport;

typedef struct ImageConstant {
  const unsigned char *body;
  size_t size;
} ImageConstant;

typedef struct ImageDebugLine {
  uint32_t function;
  uint32_t code_offset;
  char *file;
  uint32_t line;
  uint32_t column;
} ImageDebugLine;

typedef struct ImageDebugVariable {
  uint32_t function;
  uint32_t slot;
  char *name;
  uint32_t type;
} ImageDebugVariable;

typedef struct ImageModule {
  char *path;
  char *name;
  char *source;
  uint32_t flags;
  uint16_t mo_major;
  uint16_t isa_major;
  uint16_t isa_minor;
  const unsigned char *data;
  size_t data_size;
  ImageSection sections[14];
  ImageType *types;
  size_t type_count;
  ImageGlobal *globals;
  size_t global_count;
  ImageFunction *functions;
  size_t function_count;
  ImageImport *imports;
  size_t import_count;
  ImageExport *exports;
  size_t export_count;
  ImageConstant *constants;
  size_t constant_count;
  ImageDebugLine *debug_lines;
  size_t debug_line_count;
  ImageDebugVariable *debug_variables;
  size_t debug_variable_count;
  size_t type_base;
  size_t global_base;
  size_t constant_base;
  uint32_t code_base;
  uint32_t rodata_base;
} ImageModule;

typedef struct ImageManifest {
  char *entry_module;
  char *main_export;
} ImageManifest;

static mvm_kind_t image_primitive_kind(uint8_t tag);

struct ImageLoader {
  mvm_t *mvm;
  void **owned;
  size_t owned_count;
  size_t owned_capacity;
  ImageModule **modules;
  size_t module_count;
  ImageManifest manifest;
  mvm_type_desc_t *type_descs;
  size_t type_count;
  mvm_value_t *constant_values;
  uint8_t *constant_states;
  size_t constant_count;
  bool failed;
};

static uint16_t image_u16(const unsigned char *data) {
  return (uint16_t)data[0] | (uint16_t)data[1] << 8;
}

static uint32_t image_u32(const unsigned char *data) {
  return (uint32_t)data[0] | (uint32_t)data[1] << 8 | (uint32_t)data[2] << 16 |
         (uint32_t)data[3] << 24;
}

static uint64_t image_u64(const unsigned char *data) {
  uint64_t value = 0;
  for (size_t i = 0; i < 8; ++i)
    value |= (uint64_t)data[i] << (i * 8);
  return value;
}

static bool image_fail(ImageLoader *loader, const char *category,
                       const char *path, const char *format, ...) {
  if (!loader->failed) {
    fprintf(stderr, "[LOAD ERROR: %s] %s: ", category, path ? path : "image");
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputc('\n', stderr);
  }
  loader->failed = true;
  return false;
}

static bool image_oom(ImageLoader *loader) {
  return image_fail(loader, "resource", NULL, "out of memory");
}

static bool image_own(ImageLoader *loader, void *pointer) {
  if (!pointer)
    return true;
  if (loader->owned_count == loader->owned_capacity) {
    const size_t capacity =
        loader->owned_capacity ? loader->owned_capacity * 2 : 32;
    if (capacity < loader->owned_capacity ||
        capacity > SIZE_MAX / sizeof(*loader->owned)) {
      mvm_memory_free(pointer);
      return image_oom(loader);
    }
    void **owned = mvm_memory_realloc(loader->mvm->memory, loader->owned,
                                      capacity * sizeof(*owned));
    if (!owned) {
      mvm_memory_free(pointer);
      return image_oom(loader);
    }
    loader->owned = owned;
    loader->owned_capacity = capacity;
  }
  loader->owned[loader->owned_count++] = pointer;
  return true;
}

static void *image_alloc(ImageLoader *loader, size_t size) {
  void *pointer = mvm_memory_calloc(loader->mvm->memory, 1, size ? size : 1);
  if (!pointer || !image_own(loader, pointer))
    return NULL;
  return pointer;
}

static void *image_array(ImageLoader *loader, size_t count, size_t width) {
  if (width && count > SIZE_MAX / width) {
    image_oom(loader);
    return NULL;
  }
  return image_alloc(loader, count * width);
}

static char *image_string(ImageLoader *loader, const void *data, size_t size) {
  if (size == SIZE_MAX) {
    image_oom(loader);
    return NULL;
  }
  char *string = image_alloc(loader, size + 1);
  if (string && size)
    memcpy(string, data, size);
  return string;
}

static bool image_utf8(const unsigned char *text, size_t length,
                       bool allow_nul) {
  size_t i = 0;
  while (i < length) {
    const unsigned char c = text[i++];
    if (!c) {
      if (!allow_nul)
        return false;
      continue;
    }
    if (c < 0x80)
      continue;
    size_t width;
    uint32_t value;
    uint32_t minimum;
    if (c >= 0xC2 && c <= 0xDF) {
      width = 2;
      value = c & 0x1F;
      minimum = 0x80;
    } else if (c >= 0xE0 && c <= 0xEF) {
      width = 3;
      value = c & 0x0F;
      minimum = 0x800;
    } else if (c >= 0xF0 && c <= 0xF4) {
      width = 4;
      value = c & 7;
      minimum = 0x10000;
    } else {
      return false;
    }
    if (width - 1 > length - i)
      return false;
    for (size_t j = 1; j < width; ++j) {
      const unsigned char next = text[i++];
      if ((next & 0xC0) != 0x80)
        return false;
      value = (value << 6) | (next & 0x3F);
    }
    if (value < minimum || value > 0x10FFFF ||
        (value >= 0xD800 && value <= 0xDFFF))
      return false;
  }
  return true;
}

static bool image_safe_path(const char *path) {
  const size_t length = strlen(path);
  if (!length || path[0] == '/' || strchr(path, '\\'))
    return false;
  if (length >= 2 && path[1] == ':')
    return false;
  size_t start = 0;
  for (size_t i = 0; i <= length; ++i) {
    if (i != length && path[i] != '/')
      continue;
    const size_t component = i - start;
    if (!component) {
      if (i == length && start == length && length > 1)
        return true;
      return false;
    }
    if ((component == 1 && path[start] == '.') ||
        (component == 2 && path[start] == '.' && path[start + 1] == '.'))
      return false;
    start = i + 1;
  }
  return true;
}

static bool image_suffix(const char *path, const char *suffix) {
  const size_t path_length = strlen(path);
  const size_t suffix_length = strlen(suffix);
  return path_length >= suffix_length &&
         memcmp(path + path_length - suffix_length, suffix, suffix_length) == 0;
}

static bool image_read_entry(ImageLoader *loader, struct zip_t *zip,
                             size_t index, const char *path, size_t limit,
                             unsigned char **data, size_t *size) {
  if (zip_entry_openbyindex(zip, index) < 0)
    return image_fail(loader, "container", path, "cannot open ZIP entry");
  const unsigned long long declared = zip_entry_size(zip);
  if (declared > limit || declared > SIZE_MAX) {
    zip_entry_close(zip);
    return image_fail(loader, "resource", path, "entry exceeds its size limit");
  }
  void *contents =
      mvm_memory_alloc(loader->mvm->memory, declared ? (size_t)declared : 1);
  if (!contents) {
    zip_entry_close(zip);
    return image_oom(loader);
  }
  const ssize_t result = zip_entry_noallocread(zip, contents, (size_t)declared);
  const int close_result = zip_entry_close(zip);
  if (result < 0 || close_result < 0 || (size_t)result != (size_t)declared) {
    mvm_memory_free(contents);
    return image_fail(loader, "container", path,
                      "size, CRC, encryption or compression method is invalid");
  }
  if (!image_own(loader, contents))
    return false;
  *data = contents;
  *size = (size_t)declared;
  return true;
}

static char *image_strref(ImageLoader *loader, ImageModule *module,
                          uint32_t offset, uint32_t length) {
  const ImageSection *strings = &module->sections[MO_STRINGS];
  if (!length && !offset)
    return image_string(loader, "", 0);
  if ((uint64_t)offset + length > strings->size) {
    image_fail(loader, "format", module->path,
               "string reference is out of bounds");
    return NULL;
  }
  const unsigned char *data = strings->data + offset;
  if (!image_utf8(data, length, false)) {
    image_fail(loader, "format", module->path, "string is not valid UTF-8");
    return NULL;
  }
  return image_string(loader, data, length);
}

static bool image_unique_name(ImageLoader *loader, ImageModule *module,
                              char **names, size_t count, char *name,
                              const char *what) {
  for (size_t i = 0; i < count; ++i) {
    if (strcmp(names[i], name) == 0)
      return image_fail(loader, "format", module->path, "duplicate %s '%s'",
                        what, name);
  }
  return true;
}

static bool image_parse_header(ImageLoader *loader, ImageModule *module) {
  const unsigned char *data = module->data;
  const size_t size = module->data_size;
  if (size < 48 || memcmp(data, "MVMO\r\n\x1a\n", 8) != 0)
    return image_fail(loader, "format", module->path, "invalid MO header");
  if (!((image_u16(data + 8) == 1 && image_u16(data + 12) == 1 &&
         image_u16(data + 10) <= 1 && image_u16(data + 14) <= 1) ||
        (image_u16(data + 8) == 2 && image_u16(data + 12) == 2 &&
         image_u16(data + 10) == 0 && image_u16(data + 14) <= 5) ||
        (image_u16(data + 8) == 3 && image_u16(data + 12) == 2 &&
         image_u16(data + 10) == 0 && image_u16(data + 14) <= 5)))
    return image_fail(loader, "version", module->path,
                      "unsupported MO or ISA version");
  module->mo_major = image_u16(data + 8);
  module->isa_major = image_u16(data + 12);
  module->isa_minor = image_u16(data + 14);
  const uint32_t header_size = image_u32(data + 16);
  const uint64_t file_size = image_u64(data + 24);
  const uint64_t table_offset = image_u64(data + 32);
  const uint32_t section_count = image_u32(data + 40);
  if (header_size < 48 || image_u32(data + 20) || image_u32(data + 44) ||
      file_size != size || table_offset < header_size || table_offset > size ||
      section_count > (size - (size_t)table_offset) / 32)
    return image_fail(loader, "format", module->path,
                      "invalid MO section table");
  bool present[14] = {0};
  for (size_t i = 0; i < section_count; ++i) {
    const unsigned char *entry = data + table_offset + i * 32;
    const uint32_t type = image_u32(entry);
    const uint32_t flags = image_u32(entry + 4);
    const uint64_t offset = image_u64(entry + 8);
    const uint64_t section_size = image_u64(entry + 16);
    const uint32_t entry_size = image_u32(entry + 24);
    const uint32_t entry_count = image_u32(entry + 28);
    if (!type)
      return image_fail(loader, "format", module->path, "invalid section type");
    if (type > 13) {
      if (flags & 1)
        return image_fail(loader, "format", module->path,
                          "unknown critical section %" PRIu32, type);
      continue;
    }
    if (present[type] || flags)
      return image_fail(loader, "format", module->path,
                        "duplicate section or unsupported flags");
    present[type] = true;
    if (offset > size || section_size > size - (size_t)offset ||
        (section_size &&
         (offset < (uint64_t)header_size ||
          offset < table_offset + (uint64_t)section_count * 32)))
      return image_fail(loader, "format", module->path,
                        "section lies outside the MO payload");
    static const uint8_t fixed_sizes[14] = {0,  24, 0, 0, 24, 36, 28,
                                            32, 0,  0, 0, 16, 24, 20};
    if (fixed_sizes[type]) {
      if (entry_size != fixed_sizes[type] ||
          section_size != (uint64_t)entry_size * entry_count)
        return image_fail(loader, "format", module->path,
                          "invalid fixed-size section");
    } else if ((type == MO_TYPES || type == MO_CONSTANTS)
                   ? entry_size != 0
                   : entry_size || entry_count) {
      return image_fail(loader, "format", module->path,
                        "invalid section entry metadata");
    }
    if (section_size) {
      for (size_t j = 0; j < i; ++j) {
        const unsigned char *other = data + table_offset + j * 32;
        const uint64_t other_offset = image_u64(other + 8);
        const uint64_t other_size = image_u64(other + 16);
        if (other_size && offset < other_offset + other_size &&
            other_offset < offset + section_size)
          return image_fail(loader, "format", module->path,
                            "overlapping sections");
      }
    }
    module->sections[type] = (ImageSection){.data = data + (size_t)offset,
                                            .size = (size_t)section_size,
                                            .count = entry_count};
  }
  if (!present[MO_MODULE] || !present[MO_STRINGS] ||
      module->sections[MO_MODULE].count != 1 ||
      (module->sections[MO_FUNCTIONS].count && !present[MO_CODE]) ||
      (module->sections[MO_CODE].size && !present[MO_FUNCTIONS]))
    return image_fail(loader, "format", module->path,
                      "required section is absent");
  return true;
}

static bool image_parse_types(ImageLoader *loader, ImageModule *module) {
  const ImageSection *section = &module->sections[MO_TYPES];
  module->types = image_array(loader, section->count, sizeof(*module->types));
  module->type_count = section->count;
  if (section->count && !module->types)
    return false;
  size_t offset = 0;
  for (size_t i = 0; i < module->type_count; ++i) {
    if (section->size - offset < 12)
      return image_fail(loader, "format", module->path,
                        "truncated type record");
    const uint32_t body_size = image_u32(section->data + offset);
    const uint32_t name_offset = image_u32(section->data + offset + 4);
    const uint32_t name_length = image_u32(section->data + offset + 8);
    offset += 12;
    if (body_size > section->size - offset)
      return image_fail(loader, "format", module->path,
                        "invalid type body size");
    module->types[i].owner = module;
    module->types[i].local_index = (uint32_t)i;
    module->types[i].body = section->data + offset;
    module->types[i].size = body_size;
    module->types[i].name =
        image_strref(loader, module, name_offset, name_length);
    if (!module->types[i].name)
      return false;
    if ((name_length && !name_offset) || !body_size)
      return image_fail(loader, "format", module->path, "invalid type name");
    offset += body_size;
  }
  if (offset != section->size)
    return image_fail(loader, "format", module->path,
                      "type section has trailing bytes");
  return true;
}

static bool image_parse_globals(ImageLoader *loader, ImageModule *module) {
  const ImageSection *section = &module->sections[MO_GLOBALS];
  module->globals =
      image_array(loader, section->count, sizeof(*module->globals));
  module->global_count = section->count;
  if (section->count && !module->globals)
    return false;
  char **names = image_array(loader, section->count, sizeof(*names));
  if (section->count && !names)
    return false;
  for (size_t i = 0; i < section->count; ++i) {
    const unsigned char *entry = section->data + i * 24;
    ImageGlobal *global = &module->globals[i];
    global->name =
        image_strref(loader, module, image_u32(entry), image_u32(entry + 4));
    global->type = image_u32(entry + 8);
    global->constant = image_u32(entry + 12);
    global->flags = image_u32(entry + 16);
    if (!global->name ||
        !image_unique_name(loader, module, names, i, global->name, "global"))
      return false;
    names[i] = global->name;
    if (global->type >= module->type_count ||
        global->constant >=
            section->count + module->sections[MO_CONSTANTS].count ||
        global->constant >= module->sections[MO_CONSTANTS].count ||
        (global->flags & ~1U) || image_u32(entry + 20) != 0)
      return image_fail(loader, "format", module->path,
                        "invalid global record");
  }
  return true;
}

static bool image_parse_functions(ImageLoader *loader, ImageModule *module) {
  const ImageSection *section = &module->sections[MO_FUNCTIONS];
  module->functions =
      image_array(loader, section->count, sizeof(*module->functions));
  module->function_count = section->count;
  if (section->count && !module->functions)
    return false;
  char **names = image_array(loader, section->count, sizeof(*names));
  if (section->count && !names)
    return false;
  for (size_t i = 0; i < section->count; ++i) {
    const unsigned char *entry = section->data + i * 36;
    ImageFunction *function = &module->functions[i];
    function->name =
        image_strref(loader, module, image_u32(entry), image_u32(entry + 4));
    function->type = image_u32(entry + 8);
    function->code_offset = image_u32(entry + 12);
    function->code_size = image_u32(entry + 16);
    function->local_count = image_u16(entry + 20);
    function->max_stack = image_u32(entry + 24);
    if (!function->name || !image_unique_name(loader, module, names, i,
                                              function->name, "function"))
      return false;
    names[i] = function->name;
    if (function->type >= module->type_count || image_u16(entry + 22) ||
        image_u32(entry + 28) || image_u32(entry + 32) ||
        !function->code_size ||
        (uint64_t)function->code_offset + function->code_size >
            module->sections[MO_CODE].size)
      return image_fail(loader, "format", module->path,
                        "invalid function record");
    for (size_t j = 0; j < i; ++j) {
      const ImageFunction *other = &module->functions[j];
      if (function->code_offset < other->code_offset + other->code_size &&
          other->code_offset < function->code_offset + function->code_size)
        return image_fail(loader, "format", module->path,
                          "overlapping functions");
    }
  }
  return true;
}

static bool image_parse_imports(ImageLoader *loader, ImageModule *module) {
  const ImageSection *section = &module->sections[MO_IMPORTS];
  module->imports =
      image_array(loader, section->count, sizeof(*module->imports));
  module->import_count = section->count;
  if (section->count && !module->imports)
    return false;
  for (size_t i = 0; i < section->count; ++i) {
    const unsigned char *entry = section->data + i * 28;
    ImageImport *import = &module->imports[i];
    import->module =
        image_strref(loader, module, image_u32(entry), image_u32(entry + 4));
    import->name = image_strref(loader, module, image_u32(entry + 8),
                                image_u32(entry + 12));
    import->kind = entry[16];
    import->type = image_u32(entry + 20);
    if (!import->module || !import->name)
      return false;
    if (entry[17] || entry[18] || entry[19] || image_u32(entry + 24) ||
        import->kind < 1 || import->kind > 4 ||
        (import->kind == MO_IMPORT_NATIVE) != !import->module[0] ||
        (import->kind == MO_IMPORT_TYPE ? import->type != UINT32_MAX
                                        : import->type >= module->type_count))
      return image_fail(loader, "format", module->path,
                        "invalid import record");
    for (size_t j = 0; j < i; ++j) {
      const ImageImport *other = &module->imports[j];
      if (import->kind != MO_IMPORT_NATIVE && import->kind == other->kind &&
          strcmp(import->module, other->module) == 0 &&
          strcmp(import->name, other->name) == 0)
        return image_fail(loader, "format", module->path, "duplicate import");
    }
  }
  return true;
}

static bool image_parse_exports(ImageLoader *loader, ImageModule *module) {
  const ImageSection *section = &module->sections[MO_EXPORTS];
  module->exports =
      image_array(loader, section->count, sizeof(*module->exports));
  module->export_count = section->count;
  if (section->count && !module->exports)
    return false;
  for (size_t i = 0; i < section->count; ++i) {
    const unsigned char *entry = section->data + i * 32;
    ImageExport *export = &module->exports[i];
    export->name =
        image_strref(loader, module, image_u32(entry), image_u32(entry + 4));
    export->kind = entry[8];
    export->index = image_u32(entry + 12);
    export->flags = image_u32(entry + 16);
    if (!export->name)
      return false;
    if (entry[9] || entry[10] || entry[11] || image_u32(entry + 28) ||
        export->kind < 1 || export->kind > 3 || (export->flags & ~1U) ||
        (export->kind == 1 && export->index >= module->function_count) ||
        (export->kind == 2 && export->index >= module->global_count) ||
        (export->kind == 3 && export->index >= module->type_count))
      return image_fail(loader, "format", module->path,
                        "invalid export record");
    for (size_t j = 0; j < i; ++j)
      if (strcmp(export->name, module->exports[j].name) == 0)
        return image_fail(loader, "format", module->path,
                          "duplicate export '%s'", export->name);
  }
  return true;
}

static bool image_parse_constants(ImageLoader *loader, ImageModule *module) {
  const ImageSection *section = &module->sections[MO_CONSTANTS];
  module->constants =
      image_array(loader, section->count, sizeof(*module->constants));
  module->constant_count = section->count;
  if (section->count && !module->constants)
    return false;
  size_t offset = 0;
  for (size_t i = 0; i < section->count; ++i) {
    if (section->size - offset < 4)
      return image_fail(loader, "format", module->path,
                        "truncated constant record");
    const uint32_t body_size = image_u32(section->data + offset);
    offset += 4;
    if (!body_size || body_size > section->size - offset)
      return image_fail(loader, "format", module->path,
                        "invalid constant body size");
    module->constants[i].body = section->data + offset;
    module->constants[i].size = body_size;
    offset += body_size;
  }
  if (offset != section->size)
    return image_fail(loader, "format", module->path,
                      "constant section has trailing bytes");
  return true;
}

static bool image_parse_debug_lines(ImageLoader *loader, ImageModule *module) {
  const ImageSection *section = &module->sections[MO_DEBUG_LINES];
  module->debug_lines =
      image_array(loader, section->count, sizeof(*module->debug_lines));
  module->debug_line_count = section->count;
  if (section->count && !module->debug_lines)
    return false;
  for (size_t i = 0; i < section->count; ++i) {
    const unsigned char *entry = section->data + i * 24;
    ImageDebugLine *line = &module->debug_lines[i];
    line->function = image_u32(entry);
    line->code_offset = image_u32(entry + 4);
    line->file =
        image_strref(loader, module, image_u32(entry + 8), image_u32(entry + 12));
    line->line = image_u32(entry + 16);
    line->column = image_u32(entry + 20);
    if (!line->file)
      return false;
    if (line->function >= module->function_count || !line->line)
      return image_fail(loader, "format", module->path,
                        "invalid debug line record");
    const ImageFunction *function = &module->functions[line->function];
    if (line->code_offset < function->code_offset ||
        line->code_offset - function->code_offset >= function->code_size)
      return image_fail(loader, "format", module->path,
                        "debug line is outside its function");
    if (i) {
      const ImageDebugLine *previous = &module->debug_lines[i - 1];
      if (line->function < previous->function ||
          (line->function == previous->function &&
           line->code_offset <= previous->code_offset))
        return image_fail(loader, "format", module->path,
                          "debug lines are unsorted or duplicated");
    }
  }
  return true;
}

static bool image_parse_debug_variables(ImageLoader *loader,
                                        ImageModule *module) {
  const ImageSection *section = &module->sections[MO_DEBUG_VARIABLES];
  module->debug_variables =
      image_array(loader, section->count, sizeof(*module->debug_variables));
  module->debug_variable_count = section->count;
  if (section->count && !module->debug_variables)
    return false;
  for (size_t i = 0; i < section->count; ++i) {
    const unsigned char *entry = section->data + i * 20;
    ImageDebugVariable *variable = &module->debug_variables[i];
    variable->function = image_u32(entry);
    variable->slot = image_u32(entry + 4);
    variable->name = image_strref(loader, module, image_u32(entry + 8),
                                  image_u32(entry + 12));
    variable->type = image_u32(entry + 16);
    if (!variable->name || !variable->name[0] ||
        variable->function >= module->function_count ||
        variable->type >= module->type_count)
      return image_fail(loader, "format", module->path,
                        "invalid debug variable record");
    const ImageFunction *function = &module->functions[variable->function];
    if (function->type >= module->type_count)
      return image_fail(loader, "format", module->path,
                        "invalid debug variable function type");
    const ImageType *signature = &module->types[function->type];
    if (signature->size < 13 || signature->body[0] != 0x22)
      return image_fail(loader, "format", module->path,
                        "debug variable function is not a function");
    const uint32_t fixed = image_u16(signature->body + 5);
    const bool variadic = (image_u32(signature->body + 1) & 1) != 0;
    const uint64_t slots = (uint64_t)fixed + (variadic ? 1u : 0u) +
                           function->local_count;
    if (variable->slot >= slots || (variadic && variable->slot == fixed))
      return image_fail(loader, "format", module->path,
                        "debug variable slot is out of bounds");
    if (i) {
      const ImageDebugVariable *previous = &module->debug_variables[i - 1];
      if (variable->function < previous->function ||
          (variable->function == previous->function &&
           variable->slot <= previous->slot))
        return image_fail(loader, "format", module->path,
                          "debug variables are unsorted or duplicated");
    }
  }
  return true;
}

static bool image_parse_mo(ImageLoader *loader, ImageModule *module) {
  if (!image_parse_header(loader, module))
    return false;
  const unsigned char *record = module->sections[MO_MODULE].data;
  module->name =
      image_strref(loader, module, image_u32(record), image_u32(record + 4));
  module->source = image_strref(loader, module, image_u32(record + 8),
                                image_u32(record + 12));
  module->flags = image_u32(record + 16);
  if (!module->name || !module->source)
    return false;
  if (!module->name[0] || (module->flags & ~1U) || image_u32(record + 20))
    return image_fail(loader, "format", module->path, "invalid MODULE record");
  return image_parse_types(loader, module) &&
         image_parse_globals(loader, module) &&
         image_parse_functions(loader, module) &&
         image_parse_debug_lines(loader, module) &&
         image_parse_debug_variables(loader, module) &&
         image_parse_imports(loader, module) &&
         image_parse_exports(loader, module) &&
         image_parse_constants(loader, module);
}

static bool image_version(const char *text, unsigned *major, unsigned *minor) {
  if (!text[0])
    return false;
  char *end;
  unsigned long a = strtoul(text, &end, 10);
  if (*end++ != '.' || !*end)
    return false;
  unsigned long b = strtoul(end, &end, 10);
  if (*end || a > UINT32_MAX || b > UINT32_MAX)
    return false;
  *major = (unsigned)a;
  *minor = (unsigned)b;
  return true;
}

static bool image_parse_manifest(ImageLoader *loader, const char *path,
                                 const unsigned char *data, size_t size) {
  if (size >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF)
    return image_fail(loader, "container", path, "manifest has a UTF-8 BOM");
  if (!image_utf8(data, size, false))
    return image_fail(loader, "container", path, "manifest is not valid UTF-8");
  toml_result_t parsed = toml_parse_named((const char *)data, (int)size, path);
  if (!parsed.ok)
    return image_fail(loader, "container", path, "invalid TOML: %s",
                      parsed.errmsg);
  static const char *known[] = {"MVM-Format-Version", "MVM-ISA-Version",
                                "Entry-Module", "Main-Export", "Created-By"};
  for (int32_t i = 0; i < parsed.toptab.u.tab.size; ++i) {
    const char *key = parsed.toptab.u.tab.key[i];
    bool allowed = strncmp(key, "X-", 2) == 0;
    for (size_t j = 0; j < sizeof(known) / sizeof(known[0]); ++j)
      allowed = allowed || strcmp(key, known[j]) == 0;
    if (!allowed || parsed.toptab.u.tab.value[i].type != TOML_STRING) {
      toml_free(parsed);
      return image_fail(loader, "container", path,
                        "unknown or non-string key '%s'", key);
    }
  }
  toml_datum_t format = toml_get(parsed.toptab, "MVM-Format-Version");
  toml_datum_t isa = toml_get(parsed.toptab, "MVM-ISA-Version");
  toml_datum_t entry = toml_get(parsed.toptab, "Entry-Module");
  toml_datum_t main = toml_get(parsed.toptab, "Main-Export");
  unsigned format_major, format_minor, isa_major, isa_minor;
  const bool valid = format.type == TOML_STRING && isa.type == TOML_STRING &&
                     entry.type == TOML_STRING && main.type == TOML_STRING &&
                     image_version(format.u.s, &format_major, &format_minor) &&
                     image_version(isa.u.s, &isa_major, &isa_minor) &&
                     format_major == 1 && format_minor == 0 &&
                     ((isa_major == 1 && isa_minor <= 1) ||
                      (isa_major == 2 && isa_minor <= 5)) &&
                     entry.u.s[0] && main.u.s[0];
  if (valid) {
    loader->manifest.entry_module =
        image_string(loader, entry.u.s, entry.u.str.len);
    loader->manifest.main_export =
        image_string(loader, main.u.s, main.u.str.len);
  }
  toml_free(parsed);
  if (!valid || !loader->manifest.entry_module || !loader->manifest.main_export)
    return image_fail(loader, "container", path,
                      "invalid manifest fields or version");
  return true;
}

static int image_module_compare(const void *left, const void *right) {
  const ImageModule *const *a = left;
  const ImageModule *const *b = right;
  return strcmp((*a)->name, (*b)->name);
}

static bool image_load_zip(ImageLoader *loader, const char *path) {
  struct zip_t *zip = zip_open(path, 0, 'r');
  if (!zip)
    return image_fail(loader, "container", path, "cannot open ZIP archive");
  const ssize_t total = zip_entries_total(zip);
  if (total < 0 || total > IMAGE_MAX_ENTRIES) {
    zip_close(zip);
    return image_fail(loader, "resource", path,
                      "invalid or excessive ZIP entry count");
  }
  char **names = image_array(loader, (size_t)total, sizeof(*names));
  if (total && !names) {
    zip_close(zip);
    return false;
  }
  unsigned char *manifest = NULL;
  size_t manifest_size = 0;
  uint64_t total_size = 0;
  for (size_t i = 0; i < (size_t)total; ++i) {
    if (zip_entry_openbyindex(zip, i) < 0) {
      zip_close(zip);
      return image_fail(loader, "container", path,
                        "cannot read central directory");
    }
    const char *entry_name = zip_entry_name(zip);
    const bool directory = zip_entry_isdir(zip) == 1;
    const bool symlink = zip_entry_issymlink(zip) == 1;
    if (!entry_name || symlink ||
        !image_utf8((const unsigned char *)entry_name, strlen(entry_name),
                    false) ||
        !image_safe_path(entry_name)) {
      zip_entry_close(zip);
      zip_close(zip);
      return image_fail(loader, "container", path,
                        "unsafe or invalid ZIP entry path");
    }
    for (size_t j = 0; j < i; ++j) {
      if (strcmp(names[j], entry_name) == 0) {
        zip_entry_close(zip);
        zip_close(zip);
        return image_fail(loader, "container", path, "duplicate ZIP entry '%s'",
                          entry_name);
      }
    }
    names[i] = image_string(loader, entry_name, strlen(entry_name));
    if (!names[i]) {
      zip_entry_close(zip);
      zip_close(zip);
      return false;
    }
    const bool is_manifest = strcmp(entry_name, "manifest.toml") == 0;
    const bool is_mo = image_suffix(entry_name, ".mo") && !directory;
    zip_entry_close(zip);
    if (!is_manifest && !is_mo)
      continue;
    if (is_manifest && manifest) {
      zip_close(zip);
      return image_fail(loader, "container", path, "duplicate manifest");
    }
    unsigned char *contents;
    size_t size;
    if (!image_read_entry(loader, zip, i, entry_name,
                          is_manifest ? IMAGE_MAX_MANIFEST : IMAGE_MAX_OBJECT,
                          &contents, &size)) {
      zip_close(zip);
      return false;
    }
    if (is_manifest) {
      manifest = contents;
      manifest_size = size;
    } else {
      total_size += size;
      if (total_size > IMAGE_MAX_OBJECTS) {
        zip_close(zip);
        return image_fail(loader, "resource", path, "MO size limit exceeded");
      }
      ImageModule *module = image_alloc(loader, sizeof(*module));
      if (!module) {
        zip_close(zip);
        return false;
      }
      module->path = names[i];
      module->data = contents;
      module->data_size = size;
      ImageModule **modules = mvm_memory_realloc(
          loader->mvm->memory, loader->modules,
          (loader->module_count + 1) * sizeof(*loader->modules));
      if (!modules) {
        zip_close(zip);
        return image_oom(loader);
      }
      loader->modules = modules;
      loader->modules[loader->module_count++] = module;
    }
  }
  zip_close(zip);
  if (!manifest || !loader->module_count)
    return image_fail(loader, "container", path,
                      "manifest or MO modules are absent");
  if (!image_parse_manifest(loader, "manifest.toml", manifest, manifest_size))
    return false;
  size_t roots = 0;
  ImageModule *root = NULL;
  for (size_t i = 0; i < loader->module_count; ++i) {
    ImageModule *module = loader->modules[i];
    if (!image_parse_mo(loader, module))
      return false;
    for (size_t j = 0; j < i; ++j)
      if (strcmp(module->name, loader->modules[j]->name) == 0)
        return image_fail(loader, "link", module->path, "duplicate module '%s'",
                          module->name);
    if (module->flags & 1) {
      ++roots;
      root = module;
    }
  }
  if (roots != 1 || strcmp(root->name, loader->manifest.entry_module) != 0)
    return image_fail(loader, "entry", root ? root->path : path,
                      "archive must have one matching root module");
  for (size_t i = 0; i < loader->module_count; ++i) {
    ImageModule *module = loader->modules[i];
    for (size_t j = 0; j < module->export_count; ++j)
      if ((module->exports[j].flags & 1) && module != root)
        return image_fail(loader, "entry", module->path,
                          "HOST export outside the root module");
  }
  if (root != loader->modules[0]) {
    for (size_t i = 0; i < loader->module_count; ++i) {
      if (loader->modules[i] == root) {
        loader->modules[i] = loader->modules[0];
        loader->modules[0] = root;
        break;
      }
    }
  }
  qsort(loader->modules + 1, loader->module_count - 1, sizeof(*loader->modules),
        image_module_compare);
  return true;
}

static ImageModule *image_find_module(ImageLoader *loader, const char *name) {
  for (size_t i = 0; i < loader->module_count; ++i)
    if (strcmp(loader->modules[i]->name, name) == 0)
      return loader->modules[i];
  return NULL;
}

static ImageExport *image_find_export(ImageModule *module, const char *name,
                                      uint8_t kind) {
  for (size_t i = 0; i < module->export_count; ++i)
    if (module->exports[i].kind == kind &&
        strcmp(module->exports[i].name, name) == 0)
      return &module->exports[i];
  return NULL;
}

static uint8_t image_tag(ImageType *type) {
  return type->size ? type->body[0] : 0xFF;
}

static bool image_resolve_import(ImageLoader *loader, ImageModule *module,
                                 ImageImport *import) {
  if (import->kind == MO_IMPORT_NATIVE)
    return true;
  ImageModule *target = image_find_module(loader, import->module);
  if (!target)
    return image_fail(loader, "link", module->path, "module '%s' is absent",
                      import->module);
  ImageExport *export = image_find_export(target, import->name, import->kind);
  if (!export)
    return image_fail(loader, "link", module->path,
                      "export '%s' is absent in '%s'", import->name,
                      import->module);
  import->target_module = target;
  import->target_index = export->index;
  return true;
}

static bool image_resolved_type(ImageLoader *loader, ImageModule *module,
                                uint32_t index, ImageModule **resolved_module,
                                uint32_t *resolved_index) {
  size_t remaining = loader->type_count + 1;
  while (index < module->type_count &&
         image_tag(&module->types[index]) == 0x25) {
    ImageType *type = &module->types[index];
    if (type->size != 5)
      return image_fail(loader, "format", module->path,
                        "invalid imported type");
    const uint32_t import_index = image_u32(type->body + 1);
    if (import_index >= module->import_count ||
        module->imports[import_index].kind != MO_IMPORT_TYPE ||
        !module->imports[import_index].target_module)
      return image_fail(loader, "link", module->path, "invalid type import");
    ImageImport *import = &module->imports[import_index];
    module = import->target_module;
    index = import->target_index;
    if (!remaining--)
      return image_fail(loader, "link", module->path,
                        "cyclic imported type alias");
  }
  if (index >= module->type_count)
    return image_fail(loader, "format", module->path,
                      "type index is out of bounds");
  *resolved_module = module;
  *resolved_index = index;
  return true;
}

static bool image_type_equal(ImageLoader *loader, ImageModule *a, uint32_t ai,
                             ImageModule *b, uint32_t bi, size_t depth) {
  if (depth > 256 || depth > loader->type_count + 1)
    return false;
  if (!image_resolved_type(loader, a, ai, &a, &ai) ||
      !image_resolved_type(loader, b, bi, &b, &bi))
    return false;
  ImageType *at = &a->types[ai];
  ImageType *bt = &b->types[bi];
  const uint8_t tag = image_tag(at);
  if (tag != image_tag(bt))
    return false;
  if (tag <= 0x11) {
    const bool a_modern = a->mo_major >= 3;
    const bool b_modern = b->mo_major >= 3;
    if (at->size != (a_modern ? 5U : 1U) ||
        bt->size != (b_modern ? 5U : 1U))
      return false;
    const uint32_t legacy = mvm_nullable_kind(image_primitive_kind(tag)) ? 2U : 0U;
    const uint32_t af = a_modern ? image_u32(at->body + 1) : legacy;
    const uint32_t bf = b_modern ? image_u32(bt->body + 1) : legacy;
    return af == bf;
  }
  if (tag == 0x23 || tag == 0x24)
    return a == b && ai == bi;
  if (tag == 0x20 || tag == 0x21) {
    const uint32_t af = image_u32(at->body + 5) | (a->mo_major < 3 ? 2U : 0U);
    const uint32_t bf = image_u32(bt->body + 5) | (b->mo_major < 3 ? 2U : 0U);
    return at->size == 9 && bt->size == 9 && af == bf &&
           image_type_equal(loader, a, image_u32(at->body + 1), b,
                            image_u32(bt->body + 1), depth + 1);
  }
  if (tag == 0x26) {
    if (at->size < 5 || bt->size < 5)
      return false;
    const uint32_t count = image_u32(at->body + 1);
    if (count < 2 || count > 4096 || image_u32(bt->body + 1) != count ||
        at->size != (size_t)5 + count * 4 || bt->size != at->size)
      return false;
    for (size_t i = 0; i < count; ++i) {
      bool found = false;
      for (size_t j = 0; j < count && !found; ++j)
        found = image_type_equal(loader, a, image_u32(at->body + 5 + i * 4), b,
                                 image_u32(bt->body + 5 + j * 4), depth + 1);
      if (!found)
        return false;
    }
    return true;
  }
  if (tag == 0x22) {
    const uint32_t af = image_u32(at->body + 1) | (a->mo_major < 3 ? 2U : 0U);
    const uint32_t bf = image_u32(bt->body + 1) | (b->mo_major < 3 ? 2U : 0U);
    if (at->size < 13 || bt->size < 13 || af != bf ||
        image_u16(at->body + 5) != image_u16(bt->body + 5) ||
        image_u16(at->body + 7) || image_u16(bt->body + 7))
      return false;
    const uint16_t argc = image_u16(at->body + 5);
    if (at->size != (size_t)13 + argc * 4 || bt->size != at->size)
      return false;
    for (size_t i = 0; i < argc; ++i)
      if (!image_type_equal(loader, a, image_u32(at->body + 9 + i * 4), b,
                            image_u32(bt->body + 9 + i * 4), depth + 1))
        return false;
    return image_type_equal(loader, a, image_u32(at->body + 9 + argc * 4), b,
                            image_u32(bt->body + 9 + argc * 4), depth + 1);
  }
  return false;
}

static uint32_t image_type_id(ImageLoader *loader, ImageModule *module,
                              uint32_t index) {
  ImageType *type = &module->types[index];
  if (image_tag(type) != 0x25)
    return type->id;
  ImageModule *resolved;
  uint32_t resolved_index;
  if (!image_resolved_type(loader, module, index, &resolved, &resolved_index))
    return UINT32_MAX;
  return resolved->types[resolved_index].id;
}

static bool image_contains_union(ImageLoader *loader, uint32_t id,
                                 uint32_t *path, size_t depth) {
  if (id >= loader->type_count || depth >= 256)
    return true;
  const mvm_type_desc_t *type = &loader->type_descs[id];
  if (type->tag == MVM_TYPE_UNION)
    return true;
  for (size_t i = 0; i < depth; ++i)
    if (path[i] == id)
      return false;
  path[depth++] = id;
  if (type->tag == MVM_TYPE_REFERENCE || type->tag == MVM_TYPE_ARRAY)
    return image_contains_union(loader, type->inner_type_id, path, depth);
  if (type->tag == MVM_TYPE_AGGREGATE)
    for (size_t i = 0; i < type->field_count; ++i)
      if (image_contains_union(loader, type->fields[i].type_id, path, depth))
        return true;
  if (type->tag == MVM_TYPE_FUNCTION) {
    if (image_contains_union(loader, type->result_type_id, path, depth))
      return true;
    for (size_t i = 0; i < type->argc; ++i)
      if (image_contains_union(loader, type->param_type_ids[i], path, depth))
        return true;
  }
  return false;
}

static bool image_native_signature(ImageLoader *loader, ImageModule *module,
                                   ImageImport *import) {
  ImageModule *type_module;
  uint32_t type_index;
  if (!image_resolved_type(loader, module, import->type, &type_module,
                           &type_index))
    return false;
  ImageType *type = &type_module->types[type_index];
  uint32_t path[256];
  if (image_contains_union(loader, type->id, path, 0))
    return image_fail(loader, "native", module->path,
                      "union types cannot cross the native ABI");
  if (image_tag(type) != 0x22 || type->size < 13)
    return image_fail(loader, "format", module->path,
                      "native import is not a function");
  const uint16_t argc = image_u16(type->body + 5);
  if (argc > UINT8_MAX || type->size != (size_t)13 + argc * 4)
    return image_fail(loader, "format", module->path,
                      "invalid native signature");

  const bool available =
      mvm_find_native(loader->mvm, import->name, &import->native_id);
  const bool variadic = !!(image_u32(type->body + 1) & 1);
  uint8_t native_argc = 0;
  mvm_kind_t native_result = MVM_KIND_VOID;
  const mvm_kind_t *native_args = NULL;
  bool native_variadic = variadic;
  bool native_dynamic = false;
  if (available &&
      (!mvm_native_describe(loader->mvm, import->native_id, &native_argc,
                            &native_result, &native_args, &native_variadic,
                            &native_dynamic) ||
       (!native_dynamic &&
        (native_argc != argc || native_variadic != variadic))))
    return image_fail(loader, "native", module->path,
                      "native signature mismatch for '%s'", import->name);

  mvm_kind_t arg_kinds[UINT8_MAX];
  uint32_t arg_type_ids[UINT8_MAX];
  for (size_t i = 0; i < argc; ++i) {
    const uint32_t id =
        image_type_id(loader, type_module, image_u32(type->body + 9 + i * 4));
    if (id >= loader->type_count)
      return image_fail(loader, "native", module->path,
                        "native argument mismatch for '%s'", import->name);
    arg_type_ids[i] = id;
    arg_kinds[i] = loader->type_descs[id].kind;
    if (available && !native_dynamic && arg_kinds[i] != native_args[i])
      return image_fail(loader, "native", module->path,
                        "native argument mismatch for '%s'", import->name);
  }
  const uint32_t result =
      image_type_id(loader, type_module, image_u32(type->body + 9 + argc * 4));
  if (result >= loader->type_count)
    return image_fail(loader, "native", module->path,
                      "native result mismatch for '%s'", import->name);
  if (available) {
    if (!native_dynamic && loader->type_descs[result].kind != native_result)
      return image_fail(loader, "native", module->path,
                        "native result mismatch for '%s'", import->name);
  } else {
    native_result = loader->type_descs[result].kind;
    if (mvm_native_defer(loader->mvm, import->name, native_result, (uint8_t)argc,
                         arg_kinds, variadic,
                         &import->native_id) != MVM_RESULT_OK)
      return image_fail(loader, "native", module->path,
                        "cannot defer native function '%s'", import->name);
  }
  if (mvm_native_bind_signature(loader->mvm, import->native_id, result,
                                arg_type_ids) != MVM_RESULT_OK)
    return image_fail(loader, "native", module->path,
                      "incompatible full native signature for '%s'", import->name);
  return true;
}

static bool image_link_imports(ImageLoader *loader) {
  for (size_t i = 0; i < loader->module_count; ++i) {
    ImageModule *module = loader->modules[i];
    for (size_t j = 0; j < module->import_count; ++j)
      if (!image_resolve_import(loader, module, &module->imports[j]))
        return false;
  }
  return true;
}

static bool image_validate_imports(ImageLoader *loader) {
  for (size_t i = 0; i < loader->module_count; ++i) {
    ImageModule *module = loader->modules[i];
    for (size_t j = 0; j < module->import_count; ++j) {
      ImageImport *import = &module->imports[j];
      if (import->kind == MO_IMPORT_NATIVE)
        continue;
      ImageModule *target = import->target_module;
      if (import->kind == MO_IMPORT_FUNCTION) {
        if (!image_type_equal(loader, module, import->type, target,
                              target->functions[import->target_index].type, 0))
          return image_fail(loader, "link", module->path,
                            "function signature mismatch for '%s'",
                            import->name);
      } else if (import->kind == MO_IMPORT_GLOBAL) {
        if (!image_type_equal(loader, module, import->type, target,
                              target->globals[import->target_index].type, 0))
          return image_fail(loader, "link", module->path,
                            "global type mismatch for '%s'", import->name);
      }
    }
  }
  return true;
}

static uint64_t image_hash_mix(uint64_t hash, uint64_t value) {
  return (hash ^ value) * UINT64_C(1099511628211);
}

static bool image_type_hash(ImageLoader *loader, ImageModule *module,
                            uint32_t index, size_t depth, uint64_t *result) {
  if (depth > 256 ||
      !image_resolved_type(loader, module, index, &module, &index))
    return false;
  ImageType *type = &module->types[index];
  if (type->hash_state == 2) {
    *result = type->hash;
    return true;
  }
  if (type->hash_state == 1)
    return false;
  type->hash_state = 1;
  const uint8_t tag = image_tag(type);
  uint64_t hash = image_hash_mix(UINT64_C(1469598103934665603), tag), inner;
  if (tag <= 0x11) {
    const bool modern = module->mo_major >= 3;
    if (type->size != (modern ? 5U : 1U))
      return false;
    hash = image_hash_mix(
        hash, modern ? image_u32(type->body + 1)
                     : (mvm_nullable_kind(image_primitive_kind(tag)) ? 2U : 0U));
  } else if (tag == 0x23 || tag == 0x24) {
    hash = image_hash_mix(hash, (uint64_t)(uintptr_t)module);
    hash = image_hash_mix(hash, index);
  } else if (tag == 0x20 || tag == 0x21) {
    if (type->size != 9 ||
        !image_type_hash(loader, module, image_u32(type->body + 1), depth + 1,
                         &inner))
      return false;
    const uint32_t flags = image_u32(type->body + 5) |
                           (module->mo_major < 3 ? 2U : 0U);
    hash = image_hash_mix(image_hash_mix(hash, flags), inner);
  } else if (tag == 0x22) {
    if (type->size < 13)
      return false;
    const uint16_t count = image_u16(type->body + 5);
    if (type->size != (size_t)13 + count * 4)
      return false;
    const uint32_t flags = image_u32(type->body + 1) |
                           (module->mo_major < 3 ? 2U : 0U);
    hash = image_hash_mix(image_hash_mix(hash, flags), count);
    for (size_t i = 0; i <= count; ++i) {
      if (!image_type_hash(loader, module, image_u32(type->body + 9 + i * 4),
                           depth + 1, &inner))
        return false;
      hash = image_hash_mix(hash, inner);
    }
  } else if (tag == 0x26) {
    if (type->size < 5)
      return false;
    const uint32_t count = image_u32(type->body + 1);
    if (count < 2 || count > 4096 || type->size != (size_t)5 + count * 4)
      return false;
    uint64_t members = 0;
    for (size_t i = 0; i < count; ++i) {
      if (!image_type_hash(loader, module, image_u32(type->body + 5 + i * 4),
                           depth + 1, &inner))
        return false;
      members += inner;
    }
    hash = image_hash_mix(image_hash_mix(hash, count), members);
  } else
    return false;
  type->hash = hash;
  type->hash_state = 2;
  *result = hash;
  return true;
}

static bool image_assign_indices(ImageLoader *loader) {
  for (size_t i = 0; i < loader->module_count; ++i) {
    ImageModule *module = loader->modules[i];
    module->type_base = loader->type_count;
    for (size_t j = 0; j < module->type_count; ++j) {
      ImageType *type = &module->types[j];
      if (image_tag(type) != 0x25) {
        if (loader->type_count >= UINT32_MAX)
          return image_fail(loader, "resource", module->path, "too many types");
        type->id = (uint32_t)loader->type_count++;
      }
    }
    module->global_base = loader->mvm->global_count;
    loader->mvm->global_count += module->global_count;
    module->constant_base = loader->constant_count;
    loader->constant_count += module->constant_count;
  }
  size_t bucket_count = 1;
  while (bucket_count < loader->type_count) {
    if (bucket_count > SIZE_MAX / 2)
      return image_fail(loader, "resource", NULL,
                        "type table exceeds size limit");
    bucket_count *= 2;
  }
  ImageType **buckets = image_array(loader, bucket_count, sizeof(*buckets));
  if (!buckets)
    return false;
  size_t canonical_count = 0;
  for (size_t i = 0; i < loader->module_count; ++i) {
    ImageModule *module = loader->modules[i];
    for (size_t j = 0; j < module->type_count; ++j) {
      ImageType *type = &module->types[j];
      if (image_tag(type) == 0x25)
        continue;
      uint64_t hash;
      if (!image_type_hash(loader, module, (uint32_t)j, 0, &hash))
        return image_fail(loader, "format", module->path,
                          "invalid or recursive structural type");
      const size_t bucket = (size_t)hash & (bucket_count - 1);
      bool found = false;
      for (ImageType *old = buckets[bucket]; old; old = old->canonical_next) {
        if (old->hash == hash &&
            image_type_equal(loader, module, (uint32_t)j, old->owner,
                             old->local_index, 0)) {
          type->id = old->id;
          found = true;
          break;
        }
      }
      if (!found) {
        type->id = (uint32_t)canonical_count++;
        type->canonical_next = buckets[bucket];
        buckets[bucket] = type;
      }
    }
  }
  loader->type_count = canonical_count;
  loader->type_descs =
      image_array(loader, loader->type_count, sizeof(*loader->type_descs));
  loader->constant_values = image_array(loader, loader->constant_count,
                                        sizeof(*loader->constant_values));
  loader->constant_states = image_array(loader, loader->constant_count, 1);
  if ((loader->type_count && !loader->type_descs) ||
      (loader->constant_count &&
       (!loader->constant_values || !loader->constant_states)))
    return false;
  for (size_t i = 0; i < loader->module_count; ++i) {
    ImageModule *module = loader->modules[i];
    for (size_t j = 0; j < module->type_count; ++j) {
      if (image_tag(&module->types[j]) == 0x25) {
        module->types[j].id = image_type_id(loader, module, (uint32_t)j);
        if (module->types[j].id == UINT32_MAX)
          return false;
      }
    }
    for (size_t j = 0; j < module->global_count; ++j)
      module->globals[j].id = (uint32_t)(module->global_base + j);
  }
  return true;
}

static mvm_kind_t image_primitive_kind(uint8_t tag) {
  if (tag == 0)
    return MVM_KIND_VOID;
  if (tag <= 0x0C)
    return (mvm_kind_t)(tag + 1);
  if (tag == 0x0D)
    return MVM_KIND_TYPE;
  if (tag == 0x0E)
    return MVM_KIND_THREAD;
  if (tag == 0x0F)
    return MVM_KIND_MUTEX;
  if (tag == 0x10)
    return MVM_KIND_CONDITION;
  if (tag == 0x11)
    return MVM_KIND_ATOMIC;
  return MVM_KIND_VOID;
}

static size_t image_kind_size(mvm_kind_t kind) {
  const size_t width = mvm_ram_kind_width(kind);
  if (width)
    return width;
  switch (kind) {
  case MVM_KIND_STRING:
  case MVM_KIND_ARRAY:
  case MVM_KIND_TYPE:
  case MVM_KIND_REFERENCE:
  case MVM_KIND_FUNC:
  case MVM_KIND_THREAD:
  case MVM_KIND_UNION:
  case MVM_KIND_MUTEX:
  case MVM_KIND_CONDITION:
  case MVM_KIND_ATOMIC:
    return sizeof(uint64_t);
  default:
    return 0;
  }
}

static bool image_build_type_headers(ImageLoader *loader) {
  for (size_t i = 0; i < loader->module_count; ++i) {
    ImageModule *module = loader->modules[i];
    for (size_t j = 0; j < module->type_count; ++j) {
      ImageType *type = &module->types[j];
      const uint8_t tag = image_tag(type);
      if (tag == 0x25)
        continue;
      mvm_type_desc_t *desc = &loader->type_descs[type->id];
      desc->inner_type_id = MVM_TYPE_ID_NONE;
      desc->result_type_id = MVM_TYPE_ID_NONE;
      desc->alignment = 1;
      if (tag <= 0x11) {
        const bool modern = module->mo_major >= 3;
        if (type->size != (modern ? 5U : 1U) ||
            (modern && (image_u32(type->body + 1) & ~2U)) ||
            (tag > 0x0E &&
             (module->isa_major < 2 ||
              (module->isa_major == 2 && module->isa_minor < 3))))
          return image_fail(loader, "format", module->path,
                            "invalid primitive type or ISA version");
        desc->tag = MVM_TYPE_PRIMITIVE;
        desc->kind = image_primitive_kind(tag);
        desc->size = image_kind_size(desc->kind);
        desc->alignment = desc->size ? desc->size : 1;
        desc->flags = modern ? image_u32(type->body + 1)
                             : (mvm_nullable_kind(desc->kind)
                                    ? MVM_TYPE_FLAG_NULLABLE
                                    : 0);
      } else if (tag == 0x20 || tag == 0x21) {
        if (type->size != 9 || (image_u32(type->body + 5) & ~3U))
          return image_fail(loader, "format", module->path,
                            "invalid unary type");
        const uint32_t inner = image_u32(type->body + 1);
        if (inner >= module->type_count)
          return image_fail(loader, "format", module->path,
                            "invalid inner type");
        desc->tag = tag == 0x20 ? MVM_TYPE_ARRAY : MVM_TYPE_REFERENCE;
        desc->kind = tag == 0x20 ? MVM_KIND_ARRAY : MVM_KIND_REFERENCE;
        desc->size = sizeof(uint64_t);
        desc->alignment = sizeof(uint64_t);
        desc->inner_type_id = image_type_id(loader, module, inner);
        desc->flags = image_u32(type->body + 5) |
                      (module->mo_major < 3 ? MVM_TYPE_FLAG_NULLABLE : 0);
      } else if (tag == 0x22) {
        if (type->size < 13 || (image_u32(type->body + 1) & ~3U) ||
            image_u16(type->body + 7))
          return image_fail(loader, "format", module->path,
                            "invalid function type");
        const uint16_t argc = image_u16(type->body + 5);
        if (type->size != (size_t)13 + argc * 4 || argc > UINT8_MAX ||
            ((image_u32(type->body + 1) & 1) && argc == UINT8_MAX))
          return image_fail(loader, "format", module->path,
                            "invalid function signature");
        desc->tag = MVM_TYPE_FUNCTION;
        desc->kind = MVM_KIND_FUNC;
        desc->size = sizeof(uint64_t);
        desc->alignment = sizeof(uint64_t);
        desc->flags =
            ((image_u32(type->body + 1) & 1) ? MVM_TYPE_FLAG_VARIADIC : 0) |
            ((image_u32(type->body + 1) & 2) ? MVM_TYPE_FLAG_NULLABLE : 0) |
            (module->mo_major < 3 ? MVM_TYPE_FLAG_NULLABLE : 0);
        desc->argc = argc;
        desc->param_type_ids = image_array(loader, argc, sizeof(uint32_t));
        if (argc && !desc->param_type_ids)
          return false;
        for (size_t k = 0; k < argc; ++k) {
          const uint32_t param = image_u32(type->body + 9 + k * 4);
          if (param >= module->type_count)
            return image_fail(loader, "format", module->path,
                              "invalid parameter type");
          ((uint32_t *)desc->param_type_ids)[k] =
              image_type_id(loader, module, param);
        }
        const uint32_t result = image_u32(type->body + 9 + argc * 4);
        if (result >= module->type_count)
          return image_fail(loader, "format", module->path,
                            "invalid result type");
        desc->result_type_id = image_type_id(loader, module, result);
      } else if (tag == 0x26) {
        if (module->isa_major < 2 || type->size < 5)
          return image_fail(loader, "format", module->path,
                            "invalid union type");
        const uint32_t count = image_u32(type->body + 1);
        if (count < 2 || count > 4096 || type->size != (size_t)5 + count * 4)
          return image_fail(loader, "format", module->path,
                            "invalid union variants");
        uint32_t *variants = image_array(loader, count, sizeof(uint32_t));
        if (!variants)
          return false;
        for (size_t k = 0; k < count; ++k) {
          const uint32_t local = image_u32(type->body + 5 + k * 4);
          if (local >= module->type_count)
            return image_fail(loader, "format", module->path,
                              "invalid union variant type");
          variants[k] = image_type_id(loader, module, local);
          for (size_t previous = 0; previous < k; ++previous)
            if (variants[previous] == variants[k])
              return image_fail(loader, "format", module->path,
                                "duplicate union variant");
        }
        desc->tag = MVM_TYPE_UNION;
        desc->kind = MVM_KIND_UNION;
        desc->size = desc->alignment = 8;
        desc->variant_count = count;
        desc->variant_type_ids = variants;
      } else if (tag == 0x23) {
        if (type->size < 21 || (image_u32(type->body + 13) & ~1U) ||
            !type->name[0] || image_u64(type->body + 1) > SIZE_MAX)
          return image_fail(loader, "format", module->path,
                            "invalid struct type");
        desc->tag = MVM_TYPE_AGGREGATE;
        desc->kind = MVM_KIND_VOID;
        desc->size = (size_t)image_u64(type->body + 1);
        desc->alignment = image_u32(type->body + 9);
        desc->field_count = image_u32(type->body + 17);
        if (!desc->field_count ||
            type->size != (size_t)21 + desc->field_count * 20)
          return image_fail(loader, "format", module->path,
                            "invalid struct fields");
      } else if (tag == 0x24) {
        if (type->size < 9 || !type->name[0])
          return image_fail(loader, "format", module->path,
                            "invalid enum type");
        const uint32_t underlying = image_u32(type->body + 1);
        const uint32_t members = image_u32(type->body + 5);
        if (underlying >= module->type_count ||
            type->size != (size_t)9 + members * 16)
          return image_fail(loader, "format", module->path,
                            "invalid enum members");
        ImageModule *base_module;
        uint32_t base_index;
        if (!image_resolved_type(loader, module, underlying, &base_module,
                                 &base_index))
          return false;
        const uint8_t base_tag = image_tag(&base_module->types[base_index]);
        if (base_tag < 2 || base_tag > 9)
          return image_fail(loader, "format", module->path,
                            "invalid enum underlying type");
        desc->tag = MVM_TYPE_ENUM;
        desc->kind = image_primitive_kind(base_tag);
        desc->size = image_kind_size(desc->kind);
        desc->alignment = desc->size;
        desc->inner_type_id = image_type_id(loader, module, underlying);
      } else {
        return image_fail(loader, "format", module->path,
                          "unknown type tag 0x%02X", tag);
      }
    }
  }
  return true;
}

static bool image_build_struct_fields(ImageLoader *loader) {
  for (size_t i = 0; i < loader->module_count; ++i) {
    ImageModule *module = loader->modules[i];
    for (size_t j = 0; j < module->type_count; ++j) {
      ImageType *type = &module->types[j];
      if (image_tag(type) != 0x23)
        continue;
      mvm_type_desc_t *desc = &loader->type_descs[type->id];
      desc->fields =
          image_array(loader, desc->field_count, sizeof(*desc->fields));
      if (!desc->fields)
        return false;
      for (size_t k = 0; k < desc->field_count; ++k) {
        const unsigned char *field = type->body + 21 + k * 20;
        char *name = image_strref(loader, module, image_u32(field),
                                  image_u32(field + 4));
        const uint64_t offset = image_u64(field + 8);
        const uint32_t field_type = image_u32(field + 16);
        if (!name || !name[0] || field_type >= module->type_count ||
            offset > SIZE_MAX)
          return image_fail(loader, "format", module->path,
                            "invalid struct field");
        const uint32_t id = image_type_id(loader, module, field_type);
        if (id >= loader->type_count || !loader->type_descs[id].size)
          return image_fail(loader, "format", module->path,
                            "invalid field type");
        mvm_type_field_t *target = &((mvm_type_field_t *)desc->fields)[k];
        target->offset = (size_t)offset;
        target->size = loader->type_descs[id].size;
        target->kind = loader->type_descs[id].kind;
        target->type_id = id;
        target->has_type_id = true;
      }
    }
  }
  return true;
}

static bool image_collect_references(ImageLoader *loader, uint32_t id,
                                     size_t base,
                                     mvm_type_reference_t *references,
                                     size_t capacity, size_t *count,
                                     size_t depth) {
  if (depth > 256 || depth > loader->type_count)
    return image_fail(loader, "format", NULL,
                      "recursive or excessive aggregate layout");
  const mvm_type_desc_t *desc = &loader->type_descs[id];
  if (desc->tag != MVM_TYPE_AGGREGATE) {
    mvm_kind_t kind = desc->kind;
    if (kind == MVM_KIND_STRING || kind == MVM_KIND_ARRAY ||
        kind == MVM_KIND_REFERENCE || kind == MVM_KIND_THREAD ||
        kind == MVM_KIND_UNION || kind == MVM_KIND_MUTEX ||
        kind == MVM_KIND_CONDITION || kind == MVM_KIND_ATOMIC) {
      if (*count >= capacity)
        return image_fail(loader, "format", NULL,
                          "too many aggregate references");
      references[(*count)++] =
          (mvm_type_reference_t){.offset = base, .kind = kind};
    }
    return true;
  }
  for (size_t i = 0; i < desc->field_count; ++i) {
    const mvm_type_field_t *field = &desc->fields[i];
    if (field->offset > SIZE_MAX - base)
      return image_fail(loader, "resource", NULL, "aggregate offset overflow");
    if (!image_collect_references(loader, field->type_id, base + field->offset,
                                  references, capacity, count, depth + 1))
      return false;
  }
  return true;
}

static bool image_build_references(ImageLoader *loader) {
  for (size_t i = 0; i < loader->type_count; ++i) {
    mvm_type_desc_t *desc = &loader->type_descs[i];
    if (desc->tag != MVM_TYPE_AGGREGATE)
      continue;
    const size_t capacity = desc->size / sizeof(uint64_t) + 1;
    mvm_type_reference_t *references =
        image_array(loader, capacity, sizeof(*references));
    if (!references)
      return false;
    size_t count = 0;
    if (!image_collect_references(loader, (uint32_t)i, 0, references, capacity,
                                  &count, 0))
      return false;
    if (count > capacity)
      return image_fail(loader, "format", NULL,
                        "too many aggregate references");
    desc->references = references;
    desc->reference_count = count;
  }
  return true;
}

static char *image_runtime_string(ImageLoader *loader, const char *text) {
  const size_t length = strlen(text);
  if (length == SIZE_MAX) {
    image_oom(loader);
    return NULL;
  }
  char *copy = mvm_memory_alloc(loader->mvm->memory, length + 1);
  if (!copy) {
    image_oom(loader);
    return NULL;
  }
  memcpy(copy, text, length + 1);
  return copy;
}

static bool image_layout(ImageLoader *loader) {
  size_t function_count = 0;
  for (size_t i = 0; i < loader->module_count; ++i) {
    if (loader->modules[i]->function_count > SIZE_MAX - function_count)
      return image_fail(loader, "resource", NULL, "too many functions");
    function_count += loader->modules[i]->function_count;
  }
  if (function_count > SIZE_MAX / sizeof(mvm_function_type_t))
    return image_fail(loader, "resource", NULL,
                      "function metadata exceeds size limit");
  loader->mvm->function_types = mvm_memory_calloc(
      loader->mvm->memory, function_count, sizeof(mvm_function_type_t));
  if (function_count && !loader->mvm->function_types)
    return image_fail(loader, "resource", NULL,
                      "cannot allocate function metadata");
  uint64_t offset = 0;
  for (size_t i = 0; i < loader->module_count; ++i) {
    ImageModule *module = loader->modules[i];
    offset += (uint64_t)module->sections[MO_CODE].size +
              module->sections[MO_RODATA].size;
    if (offset > SIZE_MAX || offset > (uint64_t)UINT32_MAX + 1)
      return image_fail(loader, "resource", module->path,
                        "linked image exceeds address space");
  }
  if (!mvm_ram_resize(loader->mvm->ram, (size_t)offset))
    return image_oom(loader);
  offset = 0;
  for (size_t i = 0; i < loader->module_count; ++i) {
    ImageModule *module = loader->modules[i];
    module->code_base = (uint32_t)offset;
    offset += module->sections[MO_CODE].size;
    module->rodata_base = (uint32_t)offset;
    offset += module->sections[MO_RODATA].size;
    if (module->sections[MO_CODE].size)
      memcpy(loader->mvm->ram->data + module->code_base,
             module->sections[MO_CODE].data, module->sections[MO_CODE].size);
    if (module->sections[MO_RODATA].size)
      memcpy(loader->mvm->ram->data + module->rodata_base,
             module->sections[MO_RODATA].data,
             module->sections[MO_RODATA].size);
    for (size_t j = 0; j < module->function_count; ++j) {
      ImageFunction *function = &module->functions[j];
      function->address = module->code_base + function->code_offset;
      mvm_function_type_t *target =
          &loader->mvm->function_types[loader->mvm->function_type_count];
      *target = (mvm_function_type_t){
          .address = function->address,
          .type_id = image_type_id(loader, module, function->type),
          .max_stack = function->max_stack,
          .code_size = function->code_size};
      target->name = image_runtime_string(loader, function->name);
      if (!target->name)
        return false;
      ++loader->mvm->function_type_count;
    }
  }
  return true;
}

static bool image_apply_relocations(ImageLoader *loader) {
  for (size_t i = 0; i < loader->module_count; ++i) {
    ImageModule *module = loader->modules[i];
    const ImageSection *relocs = &module->sections[MO_RELOC];
    for (size_t j = 0; j < relocs->count; ++j) {
      const unsigned char *reloc = relocs->data + j * 16;
      const uint32_t code_offset = image_u32(reloc);
      const uint8_t kind = reloc[4];
      const uint32_t target = image_u32(reloc + 8);
      const int32_t addend = (int32_t)image_u32(reloc + 12);
      const size_t width = kind == 9 ? 2 : 4;
      if (!kind || kind > 9 || reloc[5] || reloc[6] || reloc[7] ||
          code_offset > module->sections[MO_CODE].size ||
          width > module->sections[MO_CODE].size - code_offset)
        return image_fail(loader, "verify", module->path, "invalid relocation");
      uint64_t value = 0;
      if (kind == 1 || kind == 2) {
        if (target || addend < 0)
          return image_fail(loader, "verify", module->path,
                            "invalid address relocation");
        value = (kind == 1 ? module->code_base : module->rodata_base) +
                (uint32_t)addend;
      } else {
        if (addend)
          return image_fail(loader, "verify", module->path,
                            "invalid relocation addend");
        if (kind == 3 || kind == 5 || kind == 7) {
          const size_t count = kind == 3   ? module->function_count
                               : kind == 5 ? module->global_count
                                           : module->type_count;
          if (target >= count)
            return image_fail(loader, "verify", module->path,
                              "relocation target is out of bounds");
          value = kind == 3   ? module->functions[target].address
                  : kind == 5 ? module->globals[target].id
                              : image_type_id(loader, module, target);
        } else {
          if (target >= module->import_count)
            return image_fail(loader, "verify", module->path,
                              "import relocation target is out of bounds");
          ImageImport *import = &module->imports[target];
          const uint8_t expected = kind == 4   ? MO_IMPORT_FUNCTION
                                   : kind == 6 ? MO_IMPORT_GLOBAL
                                   : kind == 8 ? MO_IMPORT_TYPE
                                               : MO_IMPORT_NATIVE;
          if (import->kind != expected)
            return image_fail(loader, "verify", module->path,
                              "relocation and import kinds differ");
          if (kind == 4)
            value =
                import->target_module->functions[import->target_index].address;
          else if (kind == 6)
            value = import->target_module->globals[import->target_index].id;
          else if (kind == 8)
            value = image_type_id(loader, import->target_module,
                                  import->target_index);
          else
            value = import->native_id;
        }
      }
      if ((width == 2 && value > UINT16_MAX) || value > UINT32_MAX)
        return image_fail(loader, "verify", module->path,
                          "relocation overflow");
      const size_t address = module->code_base + code_offset;
      const bool written =
          width == 2
              ? mvm_ram_poke_uhalf(loader->mvm->ram, address, (uint16_t)value)
              : mvm_ram_poke_uword(loader->mvm->ram, address, (uint32_t)value);
      if (!written)
        return image_fail(loader, "verify", module->path,
                          "relocation is out of RAM");
    }
  }
  return true;
}

static bool image_entry(ImageLoader *loader) {
  ImageModule *root = loader->modules[0];
  ImageExport *export =
      image_find_export(root, loader->manifest.main_export, MO_IMPORT_FUNCTION);
  if (!export || !(export->flags & 1))
    return image_fail(loader, "entry", root->path,
                      "HOST entry export is absent");
  ImageFunction *function = &root->functions[export->index];
  ImageModule *type_module;
  uint32_t type_index;
  if (!image_resolved_type(loader, root, function->type, &type_module,
                           &type_index))
    return false;
  ImageType *type = &type_module->types[type_index];
  if (image_tag(type) != 0x22 || type->size < 13 ||
      image_u32(type->body + 1) != 0)
    return image_fail(loader, "entry", root->path, "invalid entry signature");
  const uint16_t argc = image_u16(type->body + 5);
  if (type->size != (size_t)13 + argc * 4 || (argc != 0 && argc != 1))
    return image_fail(loader, "entry", root->path,
                      "entry must take zero or one argument");
  const uint32_t result =
      image_type_id(loader, type_module, image_u32(type->body + 9 + argc * 4));
  if (result >= loader->type_count ||
      loader->type_descs[result].kind != MVM_KIND_SWORD)
    return image_fail(loader, "entry", root->path, "entry must return Word");
  if (argc == 1) {
    const uint32_t args_id =
        image_type_id(loader, type_module, image_u32(type->body + 9));
    if (args_id >= loader->type_count ||
        loader->type_descs[args_id].tag != MVM_TYPE_ARRAY)
      return image_fail(loader, "entry", root->path, "args must be String[]");
    const uint32_t string_id = loader->type_descs[args_id].inner_type_id;
    if (string_id >= loader->type_count ||
        loader->type_descs[string_id].kind != MVM_KIND_STRING)
      return image_fail(loader, "entry", root->path, "args must be String[]");
  }
  loader->mvm->entry_argc = (uint8_t)argc;
  loader->mvm->pc = function->address;
  return true;
}

typedef struct ImageCursor {
  const unsigned char *data;
  size_t size;
  size_t offset;
} ImageCursor;

static bool image_take(ImageCursor *cursor, void *data, size_t size) {
  if (cursor->offset > cursor->size || size > cursor->size - cursor->offset)
    return false;
  if (size)
    memcpy(data, cursor->data + cursor->offset, size);
  cursor->offset += size;
  return true;
}

static bool image_take_u8(ImageCursor *cursor, uint8_t *value) {
  return image_take(cursor, value, 1);
}

static bool image_take_u32(ImageCursor *cursor, uint32_t *value) {
  unsigned char data[4];
  if (!image_take(cursor, data, sizeof(data)))
    return false;
  *value = image_u32(data);
  return true;
}

static bool image_take_u64(ImageCursor *cursor, uint64_t *value) {
  unsigned char data[8];
  if (!image_take(cursor, data, sizeof(data)))
    return false;
  *value = image_u64(data);
  return true;
}

static bool image_eval_constant(ImageLoader *loader, ImageModule *module,
                                uint32_t index, uint32_t expected_type,
                                mvm_value_t *result);

static bool image_eval_global(ImageLoader *loader, ImageModule *module,
                              uint32_t index, mvm_value_t *result) {
  if (index >= module->global_count)
    return image_fail(loader, "format", module->path,
                      "global index is out of bounds");
  ImageGlobal *global = &module->globals[index];
  if (global->state == 2) {
    *result = loader->mvm->globals[global->id];
    return true;
  }
  if (global->state == 1)
    return image_fail(loader, "link", module->path,
                      "cyclic global initializer");
  global->state = 1;
  mvm_value_t value = {.kind = MVM_KIND_NULL};
  const uint32_t type = image_type_id(loader, module, global->type);
  if (!image_eval_constant(loader, module, global->constant, type, &value))
    return false;
  loader->mvm->globals[global->id] = value;
  global->state = 2;
  *result = value;
  return true;
}

static bool image_expected_tag(ImageLoader *loader, ImageModule *module,
                               uint32_t expected, uint8_t tag) {
  if (expected >= loader->type_count)
    return image_fail(loader, "format", module->path,
                      "constant type is invalid");
  const mvm_type_desc_t *desc = &loader->type_descs[expected];
  if (tag <= 0x0C) {
    mvm_kind_t kind =
        tag == 0x01 ? MVM_KIND_NULL : image_primitive_kind(tag - 1);
    if (desc->kind != kind &&
        !(desc->tag == MVM_TYPE_ENUM && desc->kind == kind))
      return image_fail(loader, "format", module->path,
                        "scalar constant type mismatch");
  }
  return true;
}

static bool image_eval_constant(ImageLoader *loader, ImageModule *module,
                                uint32_t index, uint32_t expected_type,
                                mvm_value_t *result) {
  if (index >= module->constant_count)
    return image_fail(loader, "format", module->path,
                      "constant index is out of bounds");
  const size_t global_index = module->constant_base + index;
  if (loader->constant_states[global_index] == 2) {
    *result = loader->constant_values[global_index];
    return true;
  }
  if (loader->constant_states[global_index] == 1)
    return image_fail(loader, "link", module->path,
                      "cyclic constant initializer");
  loader->constant_states[global_index] = 1;
  ImageCursor cursor = {.data = module->constants[index].body,
                        .size = module->constants[index].size};
  uint8_t tag;
  if (!image_take_u8(&cursor, &tag))
    return image_fail(loader, "format", module->path, "empty constant");
  *result = (mvm_value_t){.kind = MVM_KIND_NULL};
  bool ok = true;
  if (tag == 0x00 || tag == 0x01) {
    if (expected_type >= loader->type_count)
      ok = image_fail(loader, "format", module->path,
                      "constant type is invalid");
    else if (tag == 0x01 &&
             !(loader->type_descs[expected_type].flags & MVM_TYPE_FLAG_NULLABLE))
      ok = image_fail(loader, "format", module->path,
                      "null is not valid for a non-null type");
    else if (tag == 0x00) {
      if (!mvm_type_defaultable(loader->mvm, expected_type))
        return image_fail(loader, "format", module->path,
                          "type has no default value");
      const mvm_type_desc_t *desc = &loader->type_descs[expected_type];
      if (desc->tag == MVM_TYPE_AGGREGATE) {
        if (mvm_typed_alloc(loader->mvm, expected_type, result) !=
            MVM_RESULT_OK)
          ok = image_fail(loader, "resource", module->path,
                          "cannot allocate constant");
      } else if (desc->kind == MVM_KIND_ARRAY ||
                 desc->kind == MVM_KIND_REFERENCE ||
                 desc->kind == MVM_KIND_STRING ||
                 desc->kind == MVM_KIND_THREAD ||
                 desc->kind == MVM_KIND_MUTEX ||
                 desc->kind == MVM_KIND_CONDITION ||
                 desc->kind == MVM_KIND_ATOMIC) {
        result->kind = MVM_KIND_NULL;
      } else {
        result->kind = desc->kind;
        result->as.ul = 0;
      }
    }
  } else if (tag >= 0x02 && tag <= 0x0C) {
    if (!image_expected_tag(loader, module, expected_type, tag)) {
      ok = false;
    } else {
      result->kind = image_primitive_kind(tag - 1);
      if (tag == 0x02) {
        uint8_t value;
        ok = image_take_u8(&cursor, &value) && value <= 1;
        result->as.b = value;
      } else if (tag == 0x03 || tag == 0x07) {
        uint8_t value;
        ok = image_take_u8(&cursor, &value);
        if (tag == 0x03)
          result->as.ub = value;
        else
          result->as.sb = (int8_t)value;
      } else if (tag == 0x04 || tag == 0x08) {
        unsigned char data[2];
        ok = image_take(&cursor, data, 2);
        const uint16_t value = image_u16(data);
        if (tag == 0x04)
          result->as.uh = value;
        else
          result->as.sh = (int16_t)value;
      } else if (tag == 0x05 || tag == 0x09 || tag == 0x0B) {
        uint32_t value;
        ok = image_take_u32(&cursor, &value);
        if (tag == 0x05)
          result->as.uw = value;
        else if (tag == 0x09)
          result->as.sw = (int32_t)value;
        else
          memcpy(&result->as.f, &value, sizeof(value));
      } else {
        uint64_t value;
        ok = image_take_u64(&cursor, &value);
        if (tag == 0x06)
          result->as.ul = value;
        else if (tag == 0x0A)
          result->as.sl = (int64_t)value;
        else
          memcpy(&result->as.d, &value, sizeof(value));
      }
    }
  } else if (tag == 0x20) {
    uint32_t offset, length;
    ok = image_take_u32(&cursor, &offset) && image_take_u32(&cursor, &length);
    if (ok &&
        ((uint64_t)offset + length + 1 > module->sections[MO_RODATA].size ||
         module->rodata_base + offset + length >= loader->mvm->ram->size ||
         loader->mvm->ram->data[module->rodata_base + offset + length] != 0 ||
         !image_utf8(loader->mvm->ram->data + module->rodata_base + offset,
                     length, false)))
      ok =
          image_fail(loader, "format", module->path, "invalid string constant");
    result->kind = MVM_KIND_STRING;
    result->as.s =
        (char *)loader->mvm->ram->data + module->rodata_base + offset;
  } else if (tag == 0x21) {
    uint8_t space, reserved[3];
    uint32_t function_index;
    ok = image_take_u8(&cursor, &space) && image_take(&cursor, reserved, 3) &&
         image_take_u32(&cursor, &function_index);
    ImageModule *target_module = module;
    if (space == 1) {
      if (function_index >= module->import_count ||
          module->imports[function_index].kind != MO_IMPORT_FUNCTION)
        ok = false;
      else {
        target_module = module->imports[function_index].target_module;
        function_index = module->imports[function_index].target_index;
      }
    } else if (space != 0) {
      ok = false;
    }
    if (!ok || function_index >= target_module->function_count || reserved[0] ||
        reserved[1] || reserved[2])
      return image_fail(loader, "format", module->path,
                        "invalid function constant");
    result->kind = MVM_KIND_FUNC;
    result->as.func = target_module->functions[function_index].address;
  } else if (tag == 0x22) {
    uint32_t type;
    ok = image_take_u32(&cursor, &type);
    if (!ok || type >= module->type_count)
      return image_fail(loader, "format", module->path,
                        "invalid type constant");
    result->kind = MVM_KIND_TYPE;
    result->as.type = image_type_id(loader, module, type);
    result->type_id = result->as.type;
    result->has_full_type = true;
  } else if (tag == 0x23) {
    uint8_t space, reserved[3];
    uint32_t global_index_ref;
    ok = image_take_u8(&cursor, &space) && image_take(&cursor, reserved, 3) &&
         image_take_u32(&cursor, &global_index_ref);
    ImageModule *target_module = module;
    if (space == 1) {
      if (global_index_ref >= module->import_count ||
          module->imports[global_index_ref].kind != MO_IMPORT_GLOBAL)
        ok = false;
      else {
        target_module = module->imports[global_index_ref].target_module;
        global_index_ref = module->imports[global_index_ref].target_index;
      }
    } else if (space != 0) {
      ok = false;
    }
    if (!ok || global_index_ref >= target_module->global_count || reserved[0] ||
        reserved[1] || reserved[2] ||
        (target_module->globals[global_index_ref].flags & 1))
      return image_fail(loader, "format", module->path,
                        "invalid global constant");
    if (!image_eval_global(loader, target_module, global_index_ref, result))
      return false;
  } else if (tag == 0x24 || tag == 0x27) {
    uint32_t array_type, length, item_count = 0;
    ok = image_take_u32(&cursor, &array_type) &&
         image_take_u32(&cursor, &length);
    if (ok && tag == 0x27)
      ok = image_take_u32(&cursor, &item_count);
    if (tag == 0x24)
      item_count = length;
    if (!ok || array_type >= module->type_count || item_count > length)
      return image_fail(loader, "format", module->path,
                        "invalid array constant");
    const uint32_t id = image_type_id(loader, module, array_type);
    if (id >= loader->type_count ||
        loader->type_descs[id].tag != MVM_TYPE_ARRAY)
      return image_fail(loader, "format", module->path,
                        "array constant has non-array type");
    const uint32_t element_type = loader->type_descs[id].inner_type_id;
    if (item_count < length && !mvm_type_defaultable(loader->mvm, element_type))
      return image_fail(loader, "format", module->path,
                        "array element has no default value");
    if (mvm_array_new_typed(loader->mvm, id, 0, result) != MVM_RESULT_OK)
      return image_fail(loader, "resource", module->path,
                        "cannot allocate array constant");
    result->type_id = id;
    result->has_full_type = true;
    loader->constant_values[global_index] = *result;
    for (size_t i = 0; i < item_count; ++i) {
      uint32_t item;
      mvm_value_t value;
      if (!image_take_u32(&cursor, &item) || item >= index ||
          !image_eval_constant(loader, module, item, element_type, &value) ||
          mvm_array_push(loader->mvm, *result, value) != MVM_RESULT_OK)
        return image_fail(loader, "format", module->path,
                          "invalid array element");
    }
    if (mvm_array_resize(loader->mvm, *result, length) != MVM_RESULT_OK)
      return image_fail(loader, "format", module->path,
                        "cannot initialize array defaults");
  } else if (tag == 0x25) {
    uint32_t struct_type, fields;
    ok = image_take_u32(&cursor, &struct_type) &&
         image_take_u32(&cursor, &fields);
    if (!ok || struct_type >= module->type_count)
      return image_fail(loader, "format", module->path,
                        "invalid struct constant");
    const uint32_t id = image_type_id(loader, module, struct_type);
    if (id >= loader->type_count ||
        loader->type_descs[id].tag != MVM_TYPE_AGGREGATE ||
        loader->type_descs[id].field_count != fields)
      return image_fail(loader, "format", module->path,
                        "struct constant type mismatch");
    if (mvm_typed_construct(loader->mvm, id, result) != MVM_RESULT_OK)
      return image_fail(loader, "resource", module->path,
                        "cannot allocate struct constant");
    result->type_id = id;
    result->has_full_type = true;
    loader->constant_values[global_index] = *result;
    for (size_t i = 0; i < fields; ++i) {
      uint32_t field;
      mvm_value_t value;
      const uint32_t field_type = loader->type_descs[id].fields[i].type_id;
      if (!image_take_u32(&cursor, &field) || field >= index ||
          !image_eval_constant(loader, module, field, field_type, &value) ||
          mvm_typed_field_set(loader->mvm, *result, i, value) != MVM_RESULT_OK)
        return image_fail(loader, "format", module->path,
                          "invalid struct field constant");
    }
  } else if (tag == 0x26) {
    uint32_t reference_type, value_index;
    ok = image_take_u32(&cursor, &reference_type) &&
         image_take_u32(&cursor, &value_index);
    if (!ok || reference_type >= module->type_count || value_index >= index)
      return image_fail(loader, "format", module->path,
                        "invalid object constant");
    const uint32_t id = image_type_id(loader, module, reference_type);
    if (id >= loader->type_count ||
        loader->type_descs[id].tag != MVM_TYPE_REFERENCE)
      return image_fail(loader, "format", module->path,
                        "object constant has non-reference type");
    mvm_value_t value;
    if (!image_eval_constant(loader, module, value_index,
                             loader->type_descs[id].inner_type_id, &value) ||
        mvm_typed_init(loader->mvm, loader->type_descs[id].inner_type_id, value,
                       result) != MVM_RESULT_OK)
      return image_fail(loader, "format", module->path,
                        "invalid object value constant");
    result->type_id = id;
    result->has_full_type = true;
  } else if (tag == 0x28 || tag == 0x29) {
    uint32_t target, variant, payload_index;
    if (module->isa_major < 2 || !image_take_u32(&cursor, &target) ||
        !image_take_u32(&cursor, &variant) ||
        !image_take_u32(&cursor, &payload_index) ||
        target >= module->type_count || variant >= module->type_count ||
        payload_index >= index)
      return image_fail(loader, "format", module->path,
                        "invalid union constant");
    const uint32_t id = image_type_id(loader, module, target);
    const uint32_t active = image_type_id(loader, module, variant);
    mvm_value_t payload;
    if (id != expected_type ||
        !image_eval_constant(loader, module, payload_index, active, &payload))
      return image_fail(loader, "format", module->path,
                        "union constant type mismatch");
    const mvm_result_t status =
        tag == 0x28 ? mvm_union_new(loader->mvm, id, active, payload, result)
                    : mvm_union_widen(loader->mvm, payload, id, result);
    if (status != MVM_RESULT_OK)
      return image_fail(loader, "format", module->path,
                        "invalid union constant payload");
  } else {
    return image_fail(loader, "format", module->path,
                      "unknown constant tag 0x%02X", tag);
  }
  if (!ok || cursor.offset != cursor.size)
    return image_fail(loader, "format", module->path,
                      "invalid or trailing constant data");
  if (expected_type < loader->type_count && result->kind != MVM_KIND_NULL) {
    const mvm_kind_t expected_kind = loader->type_descs[expected_type].kind;
    if (expected_kind != MVM_KIND_VOID && result->kind != expected_kind &&
        !(tag == 0x25 && expected_kind == MVM_KIND_VOID))
      return image_fail(loader, "format", module->path,
                        "constant result type mismatch");
  }
  loader->constant_values[global_index] = *result;
  loader->constant_states[global_index] = 2;
  return true;
}

static bool image_verify_code(ImageLoader *loader) {
  mvm_t *vm = loader->mvm;
  const size_t saved_pc = vm->pc;
  for (size_t i = 0; i < loader->module_count; ++i) {
    ImageModule *module = loader->modules[i];
    for (size_t j = 0; j < module->function_count; ++j) {
      const ImageFunction *function = &module->functions[j];
      const size_t start = function->address;
      const size_t end = start + function->code_size;
      unsigned char *boundaries = image_array(loader, function->code_size, 1);
      if (function->code_size && !boundaries)
        return false;
      for (unsigned pass = 0; pass < 2; ++pass) {
        size_t pc = start;
        while (pc < end) {
          const mvm_opcodes_t op = (mvm_opcodes_t)vm->ram->data[pc];
          vm->pc = pc + 1;
          mvm_instruction_t instruction;
          if ((module->isa_major < 2 && op >= MVM_OP_UNION_NEW) ||
              (module->isa_major == 2 && module->isa_minor < 1 &&
               op >= MVM_OP_INC_LOCAL) ||
              (module->isa_major == 2 && module->isa_minor < 2 &&
               op >= MVM_OP_BREAKPOINT) ||
              (module->isa_major == 2 && module->isa_minor < 4 &&
               op >= MVM_OP_JUMP_IF_LOCAL) ||
              (module->isa_major == 2 && module->isa_minor < 5 &&
               op == MVM_OP_NULL_CHECK) ||
              mvm_decode(vm, NULL, op, &instruction) != MVM_RESULT_OK ||
              instruction.next_pc > end || instruction.next_pc <= pc) {
            vm->pc = saved_pc;
            return image_fail(loader, "verify", module->path,
                              "invalid or truncated instruction");
          }
          boundaries[pc - start] = 1;
          if (pass && (op == MVM_OP_JUMP || op == MVM_OP_JUMP_IF_TRUE ||
                       op == MVM_OP_JUMP_IF_FALSE || op == MVM_OP_JUMP_IF_LOCAL)) {
            const size_t target = op == MVM_OP_JUMP_IF_LOCAL
                                      ? instruction.as.local_jump.address
                                      : instruction.as.jump_address;
            if (target < start || target >= end ||
                !boundaries[target - start]) {
              vm->pc = saved_pc;
              return image_fail(loader, "verify", module->path,
                                "jump does not target an instruction");
            }
          }
          pc = instruction.next_pc;
        }
      }
    }
  }
  vm->pc = saved_pc;
  return true;
}

static bool image_prepare_code(ImageLoader *loader) {
  mvm_t *vm = loader->mvm;
  const size_t saved_pc = vm->pc;
  for (size_t i = 0; i < loader->module_count; ++i) {
    ImageModule *module = loader->modules[i];
    for (size_t j = 0; j < module->function_count; ++j) {
      const ImageFunction *source = &module->functions[j];
      mvm_function_type_t *function = mvm_function_at(vm, source->address);
      if (!function)
        return image_fail(loader, "verify", module->path,
                          "missing function metadata");
      for (unsigned pass = 0; pass < 2; ++pass) {
        if (pass) {
#if SIZE_MAX < UINT64_MAX
          if (function->instruction_count > SIZE_MAX / sizeof(mvm_instruction_t))
            return image_fail(loader, "resource", module->path,
                              "prepared code exceeds size limit");
#endif
          function->instructions = mvm_memory_alloc(
              vm->memory,
              function->instruction_count * sizeof(mvm_instruction_t));
          function->instruction_indices = mvm_memory_calloc(
              vm->memory, function->code_size, sizeof(uint32_t));
          if (!function->instructions || !function->instruction_indices)
            return image_fail(loader, "resource", module->path,
                              "cannot allocate prepared code");
        }
        size_t pc = function->address;
        const size_t end = pc + function->code_size;
        uint32_t index = 0;
        while (pc < end) {
          const mvm_opcodes_t op = (mvm_opcodes_t)vm->ram->data[pc];
          vm->pc = pc + 1;
          mvm_instruction_t instruction;
          if (mvm_decode(vm, NULL, op, &instruction) != MVM_RESULT_OK ||
              instruction.next_pc > end || instruction.next_pc <= pc) {
            vm->pc = saved_pc;
            return image_fail(loader, "verify", module->path,
                              "cannot prepare instruction");
          }
          if (pass) {
            function->instructions[index] = instruction;
            function->instruction_indices[pc - function->address] = index + 1;
          }
          ++index;
          pc = instruction.next_pc;
        }
        if (!pass)
          function->instruction_count = index;
      }
    }
  }
  vm->pc = saved_pc;
  return true;
}

static bool image_load_debug(ImageLoader *loader) {
  mvm_t *vm = loader->mvm;
  bool enabled = false;
  for (size_t i = 0; i < loader->module_count; ++i)
    enabled = enabled || loader->modules[i]->debug_line_count ||
              loader->modules[i]->debug_variable_count;
  if (!enabled)
    return true;
  if (vm->global_count) {
    vm->debug_globals = mvm_memory_calloc(vm->memory, vm->global_count,
                                          sizeof(*vm->debug_globals));
    if (!vm->debug_globals)
      return image_oom(loader);
  }
  for (size_t i = 0; i < loader->module_count; ++i) {
    ImageModule *module = loader->modules[i];
    for (size_t j = 0; j < module->type_count; ++j) {
      ImageType *type = &module->types[j];
      if (image_tag(type) == 0x25 || !type->name[0] ||
          vm->types[type->id].name)
        continue;
      vm->types[type->id].name = image_runtime_string(loader, type->name);
      if (!vm->types[type->id].name)
        return false;
    }
    for (size_t j = 0; j < module->global_count; ++j) {
      ImageGlobal *global = &module->globals[j];
      mvm_debug_global_t *output = &vm->debug_globals[global->id];
      output->type_id = image_type_id(loader, module, global->type);
      output->name = image_runtime_string(loader, global->name);
      if (!output->name)
        return false;
    }
    size_t *counts = image_array(loader, module->function_count, sizeof(*counts));
    size_t *variable_counts =
        image_array(loader, module->function_count, sizeof(*variable_counts));
    if (module->function_count && (!counts || !variable_counts))
      return false;
    for (size_t j = 0; j < module->debug_line_count; ++j) {
      const ImageDebugLine *line = &module->debug_lines[j];
      const ImageFunction *source = &module->functions[line->function];
      mvm_function_type_t *target = mvm_function_at(vm, source->address);
      const uint32_t offset = line->code_offset - source->code_offset;
      if (!target || !target->instruction_indices ||
          !target->instruction_indices[offset])
        return image_fail(loader, "verify", module->path,
                          "debug line does not target an instruction");
      ++counts[line->function];
    }
    for (size_t j = 0; j < module->debug_variable_count; ++j)
      ++variable_counts[module->debug_variables[j].function];
    for (size_t j = 0; j < module->function_count; ++j) {
      if (counts[j] > SIZE_MAX / sizeof(mvm_debug_line_t) ||
          variable_counts[j] > SIZE_MAX / sizeof(mvm_debug_variable_t))
        return image_fail(loader, "resource", module->path,
                          "debug metadata exceeds size limit");
      mvm_function_type_t *target =
          mvm_function_at(vm, module->functions[j].address);
      if (counts[j]) {
        target->debug_lines = mvm_memory_calloc(
            vm->memory, counts[j], sizeof(*target->debug_lines));
        if (!target->debug_lines)
          return image_oom(loader);
        target->debug_line_count = counts[j];
        counts[j] = 0;
      }
      if (variable_counts[j]) {
        target->debug_variables = mvm_memory_calloc(
            vm->memory, variable_counts[j], sizeof(*target->debug_variables));
        if (!target->debug_variables)
          return image_oom(loader);
        target->debug_variable_count = variable_counts[j];
        variable_counts[j] = 0;
      }
    }
    for (size_t j = 0; j < module->debug_line_count; ++j) {
      const ImageDebugLine *line = &module->debug_lines[j];
      const ImageFunction *source = &module->functions[line->function];
      mvm_function_type_t *target = mvm_function_at(vm, source->address);
      mvm_debug_line_t *output = &target->debug_lines[counts[line->function]++];
      output->pc = target->address + line->code_offset - source->code_offset;
      output->line = line->line;
      output->column = line->column;
      output->file = image_runtime_string(loader, line->file);
      if (!output->file)
        return false;
    }
    for (size_t j = 0; j < module->debug_variable_count; ++j) {
      const ImageDebugVariable *variable = &module->debug_variables[j];
      const ImageFunction *source = &module->functions[variable->function];
      mvm_function_type_t *target = mvm_function_at(vm, source->address);
      mvm_debug_variable_t *output =
          &target->debug_variables[variable_counts[variable->function]++];
      output->slot = variable->slot;
      output->type_id = image_type_id(loader, module, variable->type);
      output->name = image_runtime_string(loader, variable->name);
      if (!output->name)
        return false;
    }
  }
  return true;
}

static bool image_initialize_globals(ImageLoader *loader) {
  mvm_gc_root_t root;
  if (!mvm_gc_root_register(loader->mvm, &root, loader->constant_values,
                            loader->constant_count))
    return image_fail(loader, "resource", NULL, "cannot root constant graph");
  for (size_t i = 0; i < loader->module_count && !loader->failed; ++i) {
    ImageModule *module = loader->modules[i];
    for (size_t j = 0; j < module->global_count; ++j) {
      mvm_value_t value;
      if (!image_eval_global(loader, module, (uint32_t)j, &value))
        break;
    }
  }
  mvm_gc_root_unregister(loader->mvm, &root);
  return !loader->failed;
}

static mvm_result_t image_print(mvm_t *mvm, const mvm_value_t *args,
                                mvm_value_t *result) {
  (void)mvm;
  fputs(args[0].as.s, stdout);
  result->kind = MVM_KIND_VOID;
  return MVM_RESULT_OK;
}

static mvm_result_t image_println(mvm_t *mvm, const mvm_value_t *args,
                                  mvm_value_t *result) {
  (void)mvm;
  puts(args[0].as.s);
  result->kind = MVM_KIND_VOID;
  return MVM_RESULT_OK;
}

static mvm_result_t image_eprint(mvm_t *mvm, const mvm_value_t *args,
                                 mvm_value_t *result) {
  (void)mvm;
  fputs(args[0].as.s, stderr);
  result->kind = MVM_KIND_VOID;
  return MVM_RESULT_OK;
}

static mvm_result_t image_eprintln(mvm_t *mvm, const mvm_value_t *args,
                                   mvm_value_t *result) {
  (void)mvm;
  fputs(args[0].as.s, stderr);
  fputc('\n', stderr);
  result->kind = MVM_KIND_VOID;
  return MVM_RESULT_OK;
}

static mvm_result_t image_read_line(mvm_t *mvm, const mvm_value_t *args,
                                    mvm_value_t *result) {
  (void)args;
  size_t length = 0;
  size_t capacity = 128;
  char *buffer = mvm_memory_alloc(mvm->memory, capacity);
  if (!buffer)
    return MVM_RESULT_VM_ERROR;
  int c;
  while ((c = fgetc(stdin)) != EOF) {
    if (c == '\n')
      break;
    if (length == capacity) {
      if (capacity > SIZE_MAX / 2) {
        mvm_memory_free(buffer);
        return MVM_RESULT_VM_ERROR;
      }
      capacity *= 2;
      char *grown = mvm_memory_realloc(mvm->memory, buffer, capacity);
      if (!grown) {
        mvm_memory_free(buffer);
        return MVM_RESULT_VM_ERROR;
      }
      buffer = grown;
    }
    buffer[length++] = (char)c;
  }
  if (ferror(stdin)) {
    mvm_memory_free(buffer);
    return MVM_RESULT_RUNTIME_ERROR;
  }
  if (c == EOF && !length) {
    mvm_memory_free(buffer);
    *result = (mvm_value_t){.kind = MVM_KIND_NULL};
    return MVM_RESULT_OK;
  }
  if (length && buffer[length - 1] == '\r')
    --length;
  mvm_object_t *object = mvm_gc_allocate(mvm, MVM_OBJECT_STRING, length + 1);
  if (!object) {
    mvm_memory_free(buffer);
    return MVM_RESULT_VM_ERROR;
  }
  memcpy(object->data, buffer, length);
  object->data[length] = 0;
  mvm_memory_free(buffer);
  *result =
      (mvm_value_t){.kind = MVM_KIND_STRING, .as.s = (char *)object->data};
  return MVM_RESULT_OK;
}

static mvm_result_t image_string_from_buffer(mvm_t *mvm, const char *buffer,
                                             size_t length,
                                             mvm_value_t *result) {
  mvm_object_t *object = mvm_gc_allocate(mvm, MVM_OBJECT_STRING, length + 1);
  if (!object)
    return MVM_RESULT_VM_ERROR;

  memcpy(object->data, buffer, length + 1);
  *result =
      (mvm_value_t){.kind = MVM_KIND_STRING, .as.s = (char *)object->data};
  return MVM_RESULT_OK;
}

static mvm_result_t image_bool_to_string(mvm_t *mvm, const mvm_value_t *args,
                                         mvm_value_t *result) {
  const char *text = args[0].as.b ? "true" : "false";
  return image_string_from_buffer(mvm, text, strlen(text), result);
}

static mvm_result_t image_long_to_string(mvm_t *mvm, const mvm_value_t *args,
                                         mvm_value_t *result) {
  char buffer[32];
  const int length =
      snprintf(buffer, sizeof(buffer), "%" PRId64, args[0].as.sl);
  if (length < 0 || (size_t)length >= sizeof(buffer))
    return MVM_RESULT_VM_ERROR;
  return image_string_from_buffer(mvm, buffer, (size_t)length, result);
}

static mvm_result_t image_ulong_to_string(mvm_t *mvm, const mvm_value_t *args,
                                          mvm_value_t *result) {
  char buffer[32];
  const int length =
      snprintf(buffer, sizeof(buffer), "%" PRIu64, args[0].as.ul);
  if (length < 0 || (size_t)length >= sizeof(buffer))
    return MVM_RESULT_VM_ERROR;
  return image_string_from_buffer(mvm, buffer, (size_t)length, result);
}

static mvm_result_t image_real_to_string(mvm_t *mvm, const double value,
                                         const bool single_precision,
                                         mvm_value_t *result) {
  char buffer[64];
  const int maximum = single_precision ? FLT_DECIMAL_DIG : DBL_DECIMAL_DIG;

  // Use the fewest significant digits that recover the original value.
  // Float must be parsed as float: promotion to double changes its precision.
  for (int precision = 1; precision <= maximum; ++precision) {
    const int length =
        snprintf(buffer, sizeof(buffer), "%.*g", precision, value);
    if (length < 0 || (size_t)length >= sizeof(buffer))
      return MVM_RESULT_VM_ERROR;
    if (!isfinite(value))
      return image_string_from_buffer(mvm, buffer, (size_t)length, result);

    char *end;
    const double parsed = single_precision ? (double)strtof(buffer, &end)
                                           : strtod(buffer, &end);
    // ERANGE is allowed here: subnormal values can round-trip successfully.
    if (*end == '\0' && parsed == value && !!signbit(parsed) == !!signbit(value))
      return image_string_from_buffer(mvm, buffer, (size_t)length, result);
  }
  return MVM_RESULT_VM_ERROR;
}

static mvm_result_t image_float_to_string(mvm_t *mvm, const mvm_value_t *args,
                                          mvm_value_t *result) {
  return image_real_to_string(mvm, (double)args[0].as.f, true, result);
}

static mvm_result_t image_double_to_string(mvm_t *mvm, const mvm_value_t *args,
                                           mvm_value_t *result) {
  return image_real_to_string(mvm, args[0].as.d, false, result);
}

static mvm_result_t image_string_from_bytes(mvm_t *mvm, const mvm_value_t *args,
                                            mvm_value_t *result) {
  const mvm_array_t *array = mvm_checked_array(mvm, args[0]);
  if (!array || array->kind != MVM_KIND_UBYTE)
    return MVM_RESULT_RUNTIME_ERROR;
  unsigned char *data = mvm_memory_alloc(mvm->memory, array->length);
  if (array->length && !data)
    return MVM_RESULT_VM_ERROR;
  for (size_t i = 0; i < array->length; ++i) {
    if (array->elements[i].kind != MVM_KIND_UBYTE) {
      mvm_memory_free(data);
      return MVM_RESULT_RUNTIME_ERROR;
    }
    data[i] = array->elements[i].as.ub;
  }
  const mvm_result_t status =
      mvm_string_from_bytes(mvm, data, array->length, result);
  mvm_memory_free(data);
  if (status == MVM_RESULT_RUNTIME_ERROR) {
    *result = (mvm_value_t){.kind = MVM_KIND_NULL};
    return MVM_RESULT_OK;
  }
  return status;
}

static mvm_result_t image_string_to_bytes(mvm_t *mvm, const mvm_value_t *args,
                                          mvm_value_t *result) {
  size_t length;
  size_t characters;
  mvm_result_t status =
      mvm_measure_string(mvm, args[0], &length, &characters);
  if (status != MVM_RESULT_OK)
    return status;
  mvm_value_t array;
  status = mvm_array_new_primitive(mvm, MVM_KIND_UBYTE, length, &array);
  if (status != MVM_RESULT_OK)
    return status;
  for (size_t i = 0; i < length; ++i) {
    status = mvm_array_set(mvm, array, i,
                           (mvm_value_t){.kind = MVM_KIND_UBYTE,
                                         .as.ub = (uint8_t)args[0].as.s[i]});
    if (status != MVM_RESULT_OK)
      return status;
  }
  *result = array;
  return MVM_RESULT_OK;
}

static bool image_parse_decimal(const char *text, const uint64_t maximum,
                                uint64_t *result) {
  if (*text < '0' || *text > '9')
    return false;

  uint64_t value = 0;
  do {
    const uint64_t digit = (uint64_t)(*text - '0');
    if (value > maximum / 10 || (value == maximum / 10 && digit > maximum % 10))
      return false;
    value = value * 10 + digit;
    ++text;
  } while (*text >= '0' && *text <= '9');

  if (*text != '\0')
    return false;
  *result = value;
  return true;
}

static bool image_parse_signed(const char *text, const int64_t minimum,
                               const int64_t maximum, int64_t *result) {
  const bool negative = *text == '-';
  if (negative || *text == '+')
    ++text;

  const uint64_t maximum_magnitude =
      negative ? (uint64_t)(-(minimum + 1)) + 1 : (uint64_t)maximum;
  uint64_t magnitude;
  if (!image_parse_decimal(text, maximum_magnitude, &magnitude))
    return false;

  if (negative)
    *result = magnitude == maximum_magnitude ? minimum : -(int64_t)magnitude;
  else
    *result = (int64_t)magnitude;
  return true;
}

static bool image_parse_unsigned(const char *text, const uint64_t maximum,
                                 uint64_t *result) {
  if (*text == '+')
    ++text;
  return image_parse_decimal(text, maximum, result);
}

static mvm_result_t image_string_to_byte(mvm_t *mvm, const mvm_value_t *args,
                                         mvm_value_t *result) {
  (void)mvm;
  int64_t value;
  if (!image_parse_signed(args[0].as.s, INT8_MIN, INT8_MAX, &value))
    return MVM_RESULT_RUNTIME_ERROR;
  *result = (mvm_value_t){.kind = MVM_KIND_SBYTE, .as.sb = (int8_t)value};
  return MVM_RESULT_OK;
}

static mvm_result_t image_string_to_ubyte(mvm_t *mvm, const mvm_value_t *args,
                                          mvm_value_t *result) {
  (void)mvm;
  uint64_t value;
  if (!image_parse_unsigned(args[0].as.s, UINT8_MAX, &value))
    return MVM_RESULT_RUNTIME_ERROR;
  *result = (mvm_value_t){.kind = MVM_KIND_UBYTE, .as.ub = (uint8_t)value};
  return MVM_RESULT_OK;
}

static mvm_result_t image_string_to_half(mvm_t *mvm, const mvm_value_t *args,
                                         mvm_value_t *result) {
  (void)mvm;
  int64_t value;
  if (!image_parse_signed(args[0].as.s, INT16_MIN, INT16_MAX, &value))
    return MVM_RESULT_RUNTIME_ERROR;
  *result = (mvm_value_t){.kind = MVM_KIND_SHALF, .as.sh = (int16_t)value};
  return MVM_RESULT_OK;
}

static mvm_result_t image_string_to_uhalf(mvm_t *mvm, const mvm_value_t *args,
                                          mvm_value_t *result) {
  (void)mvm;
  uint64_t value;
  if (!image_parse_unsigned(args[0].as.s, UINT16_MAX, &value))
    return MVM_RESULT_RUNTIME_ERROR;
  *result = (mvm_value_t){.kind = MVM_KIND_UHALF, .as.uh = (uint16_t)value};
  return MVM_RESULT_OK;
}

static mvm_result_t image_string_to_word(mvm_t *mvm, const mvm_value_t *args,
                                         mvm_value_t *result) {
  (void)mvm;
  int64_t value;
  if (!image_parse_signed(args[0].as.s, INT32_MIN, INT32_MAX, &value))
    return MVM_RESULT_RUNTIME_ERROR;
  *result = (mvm_value_t){.kind = MVM_KIND_SWORD, .as.sw = (int32_t)value};
  return MVM_RESULT_OK;
}

static mvm_result_t image_string_to_uword(mvm_t *mvm, const mvm_value_t *args,
                                          mvm_value_t *result) {
  (void)mvm;
  uint64_t value;
  if (!image_parse_unsigned(args[0].as.s, UINT32_MAX, &value))
    return MVM_RESULT_RUNTIME_ERROR;
  *result = (mvm_value_t){.kind = MVM_KIND_UWORD, .as.uw = (uint32_t)value};
  return MVM_RESULT_OK;
}

static mvm_result_t image_string_to_long(mvm_t *mvm, const mvm_value_t *args,
                                         mvm_value_t *result) {
  (void)mvm;
  int64_t value;
  if (!image_parse_signed(args[0].as.s, INT64_MIN, INT64_MAX, &value))
    return MVM_RESULT_RUNTIME_ERROR;
  *result = (mvm_value_t){.kind = MVM_KIND_SLONG, .as.sl = value};
  return MVM_RESULT_OK;
}

static mvm_result_t image_string_to_ulong(mvm_t *mvm, const mvm_value_t *args,
                                          mvm_value_t *result) {
  (void)mvm;
  uint64_t value;
  if (!image_parse_unsigned(args[0].as.s, UINT64_MAX, &value))
    return MVM_RESULT_RUNTIME_ERROR;
  *result = (mvm_value_t){.kind = MVM_KIND_ULONG, .as.ul = value};
  return MVM_RESULT_OK;
}

static mvm_result_t image_string_to_float(mvm_t *mvm, const mvm_value_t *args,
                                          mvm_value_t *result) {
  (void)mvm;
  const char *text = args[0].as.s;
  if (!*text || *text == ' ' || (*text >= '\t' && *text <= '\r'))
    return MVM_RESULT_RUNTIME_ERROR;

  char *end;
  errno = 0;
  const float value = strtof(text, &end);
  if (errno != 0 || end == text || *end != '\0')
    return MVM_RESULT_RUNTIME_ERROR;
  *result = (mvm_value_t){.kind = MVM_KIND_FLOAT, .as.f = value};
  return MVM_RESULT_OK;
}

static mvm_result_t image_string_to_double(mvm_t *mvm, const mvm_value_t *args,
                                           mvm_value_t *result) {
  (void)mvm;
  const char *text = args[0].as.s;
  if (!*text || *text == ' ' || (*text >= '\t' && *text <= '\r'))
    return MVM_RESULT_RUNTIME_ERROR;

  char *end;
  errno = 0;
  const double value = strtod(text, &end);
  if (errno != 0 || end == text || *end != '\0')
    return MVM_RESULT_RUNTIME_ERROR;
  *result = (mvm_value_t){.kind = MVM_KIND_DOUBLE, .as.d = value};
  return MVM_RESULT_OK;
}

static mvm_result_t image_time_unix(mvm_t *mvm, const mvm_value_t *args,
                                    mvm_value_t *result) {
  (void)mvm;
  (void)args;
#if defined(_WIN32)
  FILETIME file_time;
  GetSystemTimeAsFileTime(&file_time);
  const uint64_t ticks = ((uint64_t)file_time.dwHighDateTime << 32) |
                         (uint64_t)file_time.dwLowDateTime;
  const uint64_t epoch_offset = UINT64_C(116444736000000000);
  if (ticks < epoch_offset)
    return MVM_RESULT_RUNTIME_ERROR;
  const uint64_t milliseconds = (ticks - epoch_offset) / UINT64_C(10000);
  if (milliseconds > INT64_MAX)
    return MVM_RESULT_RUNTIME_ERROR;
  *result = (mvm_value_t){.kind = MVM_KIND_SLONG,
                          .as.sl = (int64_t)milliseconds};
#else
  struct timespec now;
  if (clock_gettime(CLOCK_REALTIME, &now) != 0)
    return MVM_RESULT_RUNTIME_ERROR;
  const int64_t seconds = (int64_t)now.tv_sec;
  if ((time_t)seconds != now.tv_sec || seconds > (INT64_MAX - 999) / 1000 ||
      seconds < INT64_MIN / 1000)
    return MVM_RESULT_RUNTIME_ERROR;
  *result = (mvm_value_t){
      .kind = MVM_KIND_SLONG,
      .as.sl = seconds * 1000 + (int64_t)(now.tv_nsec / 1000000)};
#endif
  return MVM_RESULT_OK;
}

static mvm_result_t image_time_mono(mvm_t *mvm, const mvm_value_t *args,
                                    mvm_value_t *result) {
  (void)mvm;
  (void)args;
  const uint64_t billion = UINT64_C(1000000000);
  uint64_t nanoseconds;
#if defined(_WIN32)
  LARGE_INTEGER counter;
  LARGE_INTEGER frequency;
  if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0 ||
      !QueryPerformanceCounter(&counter) || counter.QuadPart < 0)
    return MVM_RESULT_RUNTIME_ERROR;
  const uint64_t ticks = (uint64_t)counter.QuadPart;
  const uint64_t frequency_hz = (uint64_t)frequency.QuadPart;
  const uint64_t seconds = ticks / frequency_hz;
  const uint64_t remainder = ticks % frequency_hz;
  if (seconds > UINT64_MAX / billion)
    return MVM_RESULT_RUNTIME_ERROR;
  const uint64_t base = seconds * billion;
  const uint64_t fraction =
      (uint64_t)((long double)remainder * billion / frequency_hz);
  if (fraction > UINT64_MAX - base)
    return MVM_RESULT_RUNTIME_ERROR;
  nanoseconds = base + fraction;
#else
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0)
    return MVM_RESULT_RUNTIME_ERROR;
  const uint64_t seconds = (uint64_t)now.tv_sec;
  if ((time_t)seconds != now.tv_sec || seconds > UINT64_MAX / billion)
    return MVM_RESULT_RUNTIME_ERROR;
  const uint64_t base = seconds * billion;
  const uint64_t fraction = (uint64_t)now.tv_nsec;
  if (fraction > UINT64_MAX - base)
    return MVM_RESULT_RUNTIME_ERROR;
  nanoseconds = base + fraction;
#endif
  *result = (mvm_value_t){.kind = MVM_KIND_ULONG, .as.ul = nanoseconds};
  return MVM_RESULT_OK;
}

static mvm_result_t image_time_sleep(mvm_t *mvm, const mvm_value_t *args,
                                     mvm_value_t *result) {
  (void)mvm;
  uint64_t remaining = args[0].as.ul;
#if defined(_WIN32)
  while (remaining) {
    const DWORD duration =
        remaining >= MAXDWORD ? MAXDWORD - 1 : (DWORD)remaining;
    Sleep(duration);
    remaining -= duration;
  }
#else
  const uint64_t seconds = remaining / 1000;
  struct timespec request = {
      .tv_sec = (time_t)seconds,
      .tv_nsec = (long)((remaining % 1000) * 1000000),
  };
  if ((uint64_t)request.tv_sec != seconds)
    return MVM_RESULT_RUNTIME_ERROR;
  while (nanosleep(&request, &request) != 0)
    if (errno != EINTR)
      return MVM_RESULT_RUNTIME_ERROR;
#endif
  result->kind = MVM_KIND_VOID;
  return MVM_RESULT_OK;
}

static mvm_result_t image_library_load(mvm_t *mvm, const mvm_value_t *args,
                                       mvm_value_t *result) {
  const mvm_result_t status = mvm_load_native_library(mvm, args[0].as.s);
  result->kind = MVM_KIND_BOOL;
  result->as.b = (status == MVM_RESULT_OK);
  // Loading failures are observable as false; VM resource failures still trap.
  return status == MVM_RESULT_RUNTIME_ERROR ? MVM_RESULT_OK : status;
}

static mvm_result_t image_library_dispose(mvm_t *mvm, const mvm_value_t *args,
                                          mvm_value_t *result) {
  const mvm_result_t status = mvm_dispose_native_library(mvm, args[0].as.s);
  result->kind = MVM_KIND_BOOL;
  result->as.b = (status == MVM_RESULT_OK);
  return status == MVM_RESULT_RUNTIME_ERROR ? MVM_RESULT_OK : status;
}

static mvm_result_t image_operating_system(mvm_t *mvm, const mvm_value_t *args,
                                          mvm_value_t *result) {
  (void)args;
#if defined(_WIN32)
  const char *name = "windows";
#elif defined(__ANDROID__)
  const char *name = "android";
#elif defined(__linux__)
  const char *name = "linux";
#elif defined(__APPLE__) && defined(__MACH__)
#if defined(TARGET_OS_VISION) && TARGET_OS_VISION
  const char *name = "visionos";
#elif TARGET_OS_WATCH
  const char *name = "watchos";
#elif TARGET_OS_TV
  const char *name = "tvos";
#elif TARGET_OS_IPHONE
  const char *name = "ios";
#elif TARGET_OS_OSX
  const char *name = "macos";
#else
  const char *name = "unknown";
#endif
#elif defined(__FreeBSD__)
  const char *name = "freebsd";
#elif defined(__OpenBSD__)
  const char *name = "openbsd";
#elif defined(__NetBSD__)
  const char *name = "netbsd";
#elif defined(__DragonFly__)
  const char *name = "dragonfly";
#else
  const char *name = "unknown";
#endif
  return image_string_from_buffer(mvm, name, strlen(name), result);
}

static mvm_result_t image_architecture(mvm_t *mvm, const mvm_value_t *args,
                                       mvm_value_t *result) {
  (void)args;
#if defined(_M_X64) || defined(__x86_64__)
  const char *name = "x86_64";
#elif defined(_M_ARM64) || defined(__aarch64__)
  const char *name = "aarch64";
#else
  const char *name = "unknown";
#endif
  return image_string_from_buffer(mvm, name, strlen(name), result);
}

static bool image_register_builtins(ImageLoader *loader) {
  static const mvm_kind_t string_arg[] = {MVM_KIND_STRING};
  static const mvm_kind_t bool_arg[] = {MVM_KIND_BOOL};
  static const mvm_kind_t int_arg[] = {MVM_KIND_SLONG};
  static const mvm_kind_t uint_arg[] = {MVM_KIND_ULONG};
  static const mvm_kind_t float_arg[] = {MVM_KIND_FLOAT};
  static const mvm_kind_t double_arg[] = {MVM_KIND_DOUBLE};
  static const mvm_kind_t array_arg[] = {MVM_KIND_ARRAY};
  struct {
    const char *name;
    mvm_builtin_fn function;
    mvm_kind_t result;
    uint8_t argc;
    const mvm_kind_t *args;
  } builtins[] = {
      // base/io.m
      {"print", image_print, MVM_KIND_VOID, 1, string_arg},
      {"println", image_println, MVM_KIND_VOID, 1, string_arg},
      {"eprint", image_eprint, MVM_KIND_VOID, 1, string_arg},
      {"eprintln", image_eprintln, MVM_KIND_VOID, 1, string_arg},
      {"readLine", image_read_line, MVM_KIND_STRING, 0, NULL},
      // base/string.m
      {"boolToString", image_bool_to_string, MVM_KIND_STRING, 1, bool_arg},
      {"longToString", image_long_to_string, MVM_KIND_STRING, 1, int_arg},
      {"ulongToString", image_ulong_to_string, MVM_KIND_STRING, 1, uint_arg},
      {"floatToString", image_float_to_string, MVM_KIND_STRING, 1, float_arg},
      {"doubleToString", image_double_to_string, MVM_KIND_STRING, 1,
       double_arg},
      {"stringFromBytes", image_string_from_bytes, MVM_KIND_STRING, 1, array_arg},
      {"stringToBytes", image_string_to_bytes, MVM_KIND_ARRAY, 1, string_arg},
      {"stringToByte", image_string_to_byte, MVM_KIND_SBYTE, 1, string_arg},
      {"stringToUByte", image_string_to_ubyte, MVM_KIND_UBYTE, 1, string_arg},
      {"stringToHalf", image_string_to_half, MVM_KIND_SHALF, 1, string_arg},
      {"stringToUHalf", image_string_to_uhalf, MVM_KIND_UHALF, 1, string_arg},
      {"stringToWord", image_string_to_word, MVM_KIND_SWORD, 1, string_arg},
      {"stringToUWord", image_string_to_uword, MVM_KIND_UWORD, 1, string_arg},
      {"stringToLong", image_string_to_long, MVM_KIND_SLONG, 1, string_arg},
      {"stringToULong", image_string_to_ulong, MVM_KIND_ULONG, 1, string_arg},
      {"stringToFloat", image_string_to_float, MVM_KIND_FLOAT, 1, string_arg},
      {"stringToDouble", image_string_to_double, MVM_KIND_DOUBLE, 1,
       string_arg},
      // base/time.m
      {"timeUnix", image_time_unix, MVM_KIND_SLONG, 0, NULL},
      {"timeMono", image_time_mono, MVM_KIND_ULONG, 0, NULL},
      {"timeSleep", image_time_sleep, MVM_KIND_VOID, 1, uint_arg},
      // base/native.m
      {"libraryLoad", image_library_load, MVM_KIND_BOOL, 1, string_arg},
      {"libraryDispose", image_library_dispose, MVM_KIND_BOOL, 1, string_arg},
      {"operatingSystem", image_operating_system, MVM_KIND_STRING, 0, NULL},
      {"architecture", image_architecture, MVM_KIND_STRING, 0, NULL},
  };
  for (size_t i = 0; i < sizeof(builtins) / sizeof(builtins[0]); ++i) {
    uint16_t id;
    if (mvm_register_builtin(loader->mvm, builtins[i].name,
                             builtins[i].function, builtins[i].result,
                             builtins[i].argc, builtins[i].args,
                             &id) != MVM_RESULT_OK)
      return image_fail(loader, "native", NULL, "cannot register '%s'",
                        builtins[i].name);
  }
  if (mvm_math_register(loader->mvm) != MVM_RESULT_OK)
    return image_fail(loader, "native", NULL, "cannot register math functions");
  return true;
}

static bool image_native_bindings(ImageLoader *loader) {
  for (size_t i = 0; i < loader->module_count; ++i) {
    ImageModule *module = loader->modules[i];
    for (size_t j = 0; j < module->import_count; ++j)
      if (module->imports[j].kind == MO_IMPORT_NATIVE &&
          !image_native_signature(loader, module, &module->imports[j]))
        return false;
    for (size_t j = 0; j < module->export_count; ++j) {
      const ImageExport *export = &module->exports[j];
      if (!(export->flags & 1) || export->kind == MO_IMPORT_TYPE)
        continue;
      const uint32_t type = export->kind == MO_IMPORT_FUNCTION
                                ? module->functions[export->index].type
                                : module->globals[export->index].type;
      uint32_t path[256];
      if (image_contains_union(loader, image_type_id(loader, module, type),
                               path, 0))
        return image_fail(loader, "native", module->path,
                          "union types cannot cross the host ABI");
    }
  }
  return true;
}

bool mvm_image_load(mvm_t *mvm, const char *path) {
  ImageLoader loader = {.mvm = mvm};
  bool ok = image_register_builtins(&loader) && image_load_zip(&loader, path) &&
            image_link_imports(&loader) && image_assign_indices(&loader) &&
            image_validate_imports(&loader) &&
            image_build_type_headers(&loader) &&
            image_build_struct_fields(&loader) &&
            image_build_references(&loader) && image_native_bindings(&loader);
  if (ok &&
      mvm_type_load(mvm, loader.type_descs, loader.type_count) != MVM_RESULT_OK)
    ok = image_fail(&loader, "verify", path, "invalid runtime type metadata");
  if (ok) {
    mvm->globals = mvm_memory_calloc(mvm->memory, mvm->global_count,
                                     sizeof(*mvm->globals));
    if (mvm->global_count && !mvm->globals)
      ok =
          image_fail(&loader, "resource", path, "cannot allocate global slots");
    else
      mvm->global_capacity = mvm->global_count;
  }
  ok = ok && image_layout(&loader) && image_apply_relocations(&loader) &&
       image_verify_code(&loader) && image_prepare_code(&loader) &&
       image_load_debug(&loader) && image_initialize_globals(&loader) &&
       image_entry(&loader);
  for (size_t i = 0; i < loader.owned_count; ++i)
    mvm_memory_free(loader.owned[i]);
  mvm_memory_free(loader.owned);
  mvm_memory_free(loader.modules);
  return ok && !loader.failed;
}

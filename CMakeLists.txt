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

cmake_minimum_required(VERSION 3.16)

set(PROJECT_NAME "mvm")
project(${PROJECT_NAME} LANGUAGES C)

# Toolchain components live beside the VM source tree.
set(MVM_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
get_filename_component(M_SOURCE_DIR "${MVM_SOURCE_DIR}/.." ABSOLUTE)

configure_file(LICENSE "${MVM_SOURCE_DIR}/bin/LICENSE" COPYONLY)
configure_file(THIRD_PARTY_LICENSES.txt "${MVM_SOURCE_DIR}/bin/THIRD_PARTY_LICENSES.txt" COPYONLY)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBFFI REQUIRED IMPORTED_TARGET libffi)
find_package(Threads REQUIRED)
find_package(FLEX REQUIRED)
find_package(BISON REQUIRED)

set(MVM_SYSTEM_LIBRARIES)
if(WIN32)
    list(APPEND MVM_SYSTEM_LIBRARIES bcrypt)
endif()

find_package(Git QUIET)

set(MVM_VERSION "unknown")

if(Git_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" describe --tags --always --dirty
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE GIT_VERSION
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE GIT_RESULT
    )

    if(GIT_RESULT STREQUAL "0")
        set(MVM_VERSION "${GIT_VERSION}")
    endif()
endif()

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/include/version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/version.h"
    @ONLY
)

# Keep every runtime consumer on the same set of translation units.
set(MVM_RUNTIME_SOURCES
    source/mvm.c
    source/mvm/allocator.c
    source/mvm/decode.c
    source/mvm/debug.c
    source/mvm/execute.c
    source/mvm/file.c
    source/mvm/resource.c
    source/mvm/gc.c
    source/mvm/image.c
    source/mvm/math.c
    source/mvm/memory.c
    source/mvm/native.c
    source/mvm/numeric.c
    source/mvm/object.c
    source/mvm/ram.c
    source/mvm/stack.c
    source/mvm/string.c
    source/mvm/sync.c
    source/mvm/sync_platform.c
    source/mvm/thread.c
    source/mvm/type.c
    source/mvm/value.c
)

add_library(mvm_zip STATIC vendor/zip/src/zip.c)
target_include_directories(mvm_zip PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/vendor/zip/src")
if(NOT WIN32)
    target_compile_definitions(mvm_zip PRIVATE _POSIX_C_SOURCE=200809L ZIP_HAVE_SYMLINK=1)
endif()
set_target_properties(mvm_zip PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS YES)
if(NOT MSVC)
    target_compile_options(mvm_zip PRIVATE -w)
endif()

add_library(mvm_toml STATIC vendor/tomlc17/src/tomlc17.c)
target_include_directories(mvm_toml PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/vendor/tomlc17/src")
set_target_properties(mvm_toml PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(NOT MSVC)
    target_compile_options(mvm_toml PRIVATE -w)
endif()

include(CTest)
add_subdirectory("${M_SOURCE_DIR}/mvm-c" "${CMAKE_CURRENT_BINARY_DIR}/mvm-c")
add_subdirectory("${M_SOURCE_DIR}/mvm-fmt" "${CMAKE_CURRENT_BINARY_DIR}/mvm-fmt")

add_executable(${PROJECT_NAME} source/main.c ${MVM_RUNTIME_SOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
)

# warnings for our own code only, not for SDL and miniaudio
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /W4)
else()
    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra)
endif()

set_target_properties(${PROJECT_NAME} PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin/$<0:>"
)
target_link_libraries(${PROJECT_NAME} PRIVATE PkgConfig::LIBFFI ${CMAKE_DL_LIBS} Threads::Threads ${MVM_SYSTEM_LIBRARIES} mvm_zip mvm_toml)
if(NOT MSVC)
    target_link_libraries(${PROJECT_NAME} PRIVATE m)
endif()

add_subdirectory("${M_SOURCE_DIR}/mvm-debug" "${CMAKE_CURRENT_BINARY_DIR}/mvm-debug")

if(BUILD_TESTING)
    add_executable(memory_test tests/memory_test.c ${MVM_RUNTIME_SOURCES})
    target_include_directories(memory_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(memory_test PRIVATE PkgConfig::LIBFFI ${CMAKE_DL_LIBS} Threads::Threads ${MVM_SYSTEM_LIBRARIES} mvm_zip mvm_toml)
    set_target_properties(memory_test PROPERTIES
        C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(memory_test PRIVATE /W4)
    else()
        target_compile_options(memory_test PRIVATE -Wall -Wextra)
        target_link_libraries(memory_test PRIVATE m)
    endif()
    add_test(NAME memory COMMAND memory_test)
    set_tests_properties(memory PROPERTIES TIMEOUT 30)

    add_executable(stack_test tests/stack_test.c source/mvm/stack.c source/mvm/allocator.c)
    target_link_libraries(stack_test PRIVATE Threads::Threads)
    target_include_directories(stack_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    set_target_properties(stack_test PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED YES
        C_EXTENSIONS NO
    )
    if(MSVC)
        target_compile_options(stack_test PRIVATE /W4)
    else()
        target_compile_options(stack_test PRIVATE -Wall -Wextra)
    endif()
    add_test(NAME stack COMMAND stack_test)

    add_executable(ram_test tests/ram_test.c source/mvm/ram.c source/mvm/allocator.c)
    target_link_libraries(ram_test PRIVATE Threads::Threads)
    target_include_directories(ram_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    set_target_properties(ram_test PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED YES
        C_EXTENSIONS NO
    )
    if(MSVC)
        target_compile_options(ram_test PRIVATE /W4)
    else()
        target_compile_options(ram_test PRIVATE -Wall -Wextra)
    endif()
    add_test(NAME ram COMMAND ram_test)

    add_executable(mvm_test tests/mvm_test.c ${MVM_RUNTIME_SOURCES})
    target_include_directories(mvm_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    set_target_properties(mvm_test PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED YES
        C_EXTENSIONS NO
    )
    target_link_libraries(mvm_test PRIVATE PkgConfig::LIBFFI ${CMAKE_DL_LIBS} Threads::Threads ${MVM_SYSTEM_LIBRARIES} mvm_zip mvm_toml)
    add_dependencies(mvm_test mvm_native_test_library)
    target_compile_definitions(mvm_test PRIVATE
        MVM_TEST_LIBRARY_PATH="$<TARGET_FILE:mvm_native_test_library>"
    )
    if(MSVC)
        target_compile_options(mvm_test PRIVATE /W4)
    else()
        target_compile_options(mvm_test PRIVATE -Wall -Wextra)
        target_link_libraries(mvm_test PRIVATE m)
    endif()
    add_test(NAME mvm COMMAND mvm_test)

    add_executable(gc_test tests/gc_test.c ${MVM_RUNTIME_SOURCES})
    target_include_directories(gc_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    set_target_properties(gc_test PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED YES
        C_EXTENSIONS NO
    )
    target_link_libraries(gc_test PRIVATE PkgConfig::LIBFFI ${CMAKE_DL_LIBS} Threads::Threads ${MVM_SYSTEM_LIBRARIES} mvm_zip mvm_toml)
    if(MSVC)
        target_compile_options(gc_test PRIVATE /W4)
    else()
        target_compile_options(gc_test PRIVATE -Wall -Wextra)
        target_link_libraries(gc_test PRIVATE m)
    endif()
    add_test(NAME gc COMMAND gc_test)

    add_executable(thread_test tests/thread_test.c ${MVM_RUNTIME_SOURCES})
    target_include_directories(thread_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    set_target_properties(thread_test PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED YES
        C_EXTENSIONS NO
    )
    target_link_libraries(thread_test PRIVATE PkgConfig::LIBFFI ${CMAKE_DL_LIBS} Threads::Threads ${MVM_SYSTEM_LIBRARIES} mvm_zip mvm_toml)
    add_dependencies(thread_test mvm_native_test_library)
    target_compile_definitions(thread_test PRIVATE
        MVM_TEST_LIBRARY_PATH="$<TARGET_FILE:mvm_native_test_library>"
    )
    if(MSVC)
        target_compile_options(thread_test PRIVATE /W4)
    else()
        target_compile_options(thread_test PRIVATE -Wall -Wextra)
        target_link_libraries(thread_test PRIVATE m)
    endif()
    add_test(NAME thread COMMAND thread_test)

    add_executable(sync_test tests/sync_test.c ${MVM_RUNTIME_SOURCES})
    target_include_directories(sync_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    set_target_properties(sync_test PROPERTIES
        C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    target_link_libraries(sync_test PRIVATE PkgConfig::LIBFFI ${CMAKE_DL_LIBS} Threads::Threads ${MVM_SYSTEM_LIBRARIES} mvm_zip mvm_toml)
    if(MSVC)
        target_compile_options(sync_test PRIVATE /W4)
    else()
        target_compile_options(sync_test PRIVATE -Wall -Wextra)
        target_link_libraries(sync_test PRIVATE m)
    endif()
    add_test(NAME sync COMMAND sync_test)
    set_tests_properties(sync PROPERTIES TIMEOUT 60)

    add_executable(typed_test tests/typed_test.c ${MVM_RUNTIME_SOURCES})
    target_include_directories(typed_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    set_target_properties(typed_test PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED YES
        C_EXTENSIONS NO
    )
    target_link_libraries(typed_test PRIVATE PkgConfig::LIBFFI ${CMAKE_DL_LIBS} Threads::Threads ${MVM_SYSTEM_LIBRARIES} mvm_zip mvm_toml)
    if(MSVC)
        target_compile_options(typed_test PRIVATE /W4)
    else()
        target_compile_options(typed_test PRIVATE -Wall -Wextra)
        target_link_libraries(typed_test PRIVATE m)
    endif()
    add_test(NAME typed COMMAND typed_test)

    add_executable(isa11_test tests/isa11_test.c ${MVM_RUNTIME_SOURCES})
    target_include_directories(isa11_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    set_target_properties(isa11_test PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED YES
        C_EXTENSIONS NO
    )
    target_link_libraries(isa11_test PRIVATE PkgConfig::LIBFFI ${CMAKE_DL_LIBS} Threads::Threads ${MVM_SYSTEM_LIBRARIES} mvm_zip mvm_toml)
    if(MSVC)
        target_compile_options(isa11_test PRIVATE /W4)
    else()
        target_compile_options(isa11_test PRIVATE -Wall -Wextra)
        target_link_libraries(isa11_test PRIVATE m)
    endif()
    add_test(NAME isa11 COMMAND isa11_test)

    # Replace only the GC allocator in this test to exercise OOM and retries.
    set(MVM_ARRAY_TEST_SOURCES ${MVM_RUNTIME_SOURCES})
    list(REMOVE_ITEM MVM_ARRAY_TEST_SOURCES source/mvm/gc.c)
    add_executable(array_test tests/array_test.c tests/array_allocator.c
        ${MVM_ARRAY_TEST_SOURCES})
    target_include_directories(array_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    set_target_properties(array_test PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED YES
        C_EXTENSIONS NO
    )
    target_link_libraries(array_test PRIVATE PkgConfig::LIBFFI ${CMAKE_DL_LIBS} Threads::Threads ${MVM_SYSTEM_LIBRARIES} mvm_zip mvm_toml)
    if(MSVC)
        target_compile_options(array_test PRIVATE /W4)
    else()
        target_compile_options(array_test PRIVATE -Wall -Wextra)
        target_link_libraries(array_test PRIVATE m)
    endif()
    add_test(NAME array COMMAND array_test)

    add_library(mvm_native_test_library SHARED tests/native_library.c)
    target_include_directories(mvm_native_test_library PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    set_target_properties(mvm_native_test_library PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED YES
        C_EXTENSIONS NO
    )
    if(MSVC)
        target_compile_options(mvm_native_test_library PRIVATE /W4)
    else()
        target_compile_options(mvm_native_test_library PRIVATE -Wall -Wextra)
    endif()
    foreach(invalid_kind IN ITEMS abi exports)
        set(invalid_target mvm_native_invalid_${invalid_kind}_library)
        add_library(${invalid_target} SHARED tests/native_library_invalid.c)
        target_include_directories(${invalid_target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
        set_target_properties(${invalid_target} PROPERTIES
            C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
        if(MSVC)
            target_compile_options(${invalid_target} PRIVATE /W4)
        else()
            target_compile_options(${invalid_target} PRIVATE -Wall -Wextra)
        endif()
    endforeach()
    target_compile_definitions(mvm_native_invalid_abi_library PRIVATE TEST_INVALID_NATIVE_ABI=1)
    add_dependencies(mvm_test mvm_native_invalid_abi_library mvm_native_invalid_exports_library)
    target_compile_definitions(mvm_test PRIVATE
        MVM_INVALID_ABI_LIBRARY_PATH="$<TARGET_FILE:mvm_native_invalid_abi_library>"
        MVM_INVALID_EXPORTS_LIBRARY_PATH="$<TARGET_FILE:mvm_native_invalid_exports_library>"
    )
    file(GENERATE
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/native_library_errors.m"
        INPUT "${M_SOURCE_DIR}/mvm-c/tests/native_library_errors.m.in"
    )
    add_test(NAME mvm_native_library_recovery
        COMMAND "${CMAKE_COMMAND}"
            -DMC=$<TARGET_FILE:mvm-c>
            -DMVM=$<TARGET_FILE:mvm>
            -DSOURCE=${CMAKE_CURRENT_BINARY_DIR}/native_library_errors.m
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/native-library-recovery.mvm
            "-DEXPECT=native library recovery: OK\n"
            -P "${M_SOURCE_DIR}/mvm-c/tests/mvm_test.cmake"
    )
    set_tests_properties(mvm_native_library_recovery PROPERTIES TIMEOUT 30)

    file(GENERATE
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/native_library.m"
        INPUT "${M_SOURCE_DIR}/mvm-c/tests/native_library.m.in"
    )
    add_test(NAME mvm_native_library_container
        COMMAND "${CMAKE_COMMAND}"
            -DMC=$<TARGET_FILE:mvm-c>
            -DMVM=$<TARGET_FILE:mvm>
            -DSOURCE=${CMAKE_CURRENT_BINARY_DIR}/native_library.m
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/native-library-container.mvm
            "-DEXPECT=42\n"
            -P "${M_SOURCE_DIR}/mvm-c/tests/mvm_test.cmake"
    )

    add_test(NAME mvm_hello_container
        COMMAND "${CMAKE_COMMAND}"
            -DMC=$<TARGET_FILE:mvm-c>
            -DMVM=$<TARGET_FILE:mvm>
            -DSOURCE=${M_SOURCE_DIR}/mvm-c/examples/hello.m
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/hello-container.mvm
            "-DEXPECT=Hello, world!\n"
            -P "${M_SOURCE_DIR}/mvm-c/tests/mvm_test.cmake"
    )
    add_test(NAME mvm_to_string_container
        COMMAND "${CMAKE_COMMAND}"
            -DMC=$<TARGET_FILE:mvm-c>
            -DMVM=$<TARGET_FILE:mvm>
            -DSOURCE=${M_SOURCE_DIR}/mvm-c/tests/to_string.m
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/to-string-container.mvm
            "-DEXPECT=true\nfalse\n-128\n255\n-32768\n65535\n-2147483648\n4294967295\n0\n42\n-42\n9223372036854775807\n-9223372036854775808\n0\n18446744073709551615\n1.5\n3.25\ntext\n"
            -P "${M_SOURCE_DIR}/mvm-c/tests/mvm_test.cmake"
    )
    add_test(NAME mvm_float_to_string_container
        COMMAND "${CMAKE_COMMAND}"
            -DMC=$<TARGET_FILE:mvm-c>
            -DMVM=$<TARGET_FILE:mvm>
            -DSOURCE=${M_SOURCE_DIR}/mvm-c/tests/float_to_string.m
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/float-to-string-container.mvm
            "-DEXPECT=float to string: OK\n"
            -P "${M_SOURCE_DIR}/mvm-c/tests/mvm_test.cmake"
    )
    add_test(NAME mvm_math
        COMMAND "${CMAKE_COMMAND}"
            -DMC=$<TARGET_FILE:mvm-c>
            -DMVM=$<TARGET_FILE:mvm>
            -DSOURCE=${M_SOURCE_DIR}/mvm-c/tests/math.m
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/math.mvm
            "-DEXPECT=math: OK\n"
            -P "${M_SOURCE_DIR}/mvm-c/tests/mvm_test.cmake"
    )
    set_tests_properties(mvm_math PROPERTIES TIMEOUT 30)

    add_test(NAME mvm_random_container
        COMMAND "${CMAKE_COMMAND}"
            -DMC=$<TARGET_FILE:mvm-c>
            -DMVM=$<TARGET_FILE:mvm>
            -DSOURCE=${M_SOURCE_DIR}/mvm-c/tests/random.m
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/random-container.mvm
            "-DEXPECT=random: OK\n"
            -P "${M_SOURCE_DIR}/mvm-c/tests/mvm_test.cmake"
    )
    if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        set(native_os "macos")
    elseif(CMAKE_SYSTEM_NAME MATCHES "^(Windows|Linux|Android|iOS|tvOS|watchOS|visionOS|FreeBSD|OpenBSD|NetBSD|DragonFly)$")
        string(TOLOWER "${CMAKE_SYSTEM_NAME}" native_os)
    else()
        set(native_os "unknown")
    endif()
    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" native_arch)
    if(native_arch MATCHES "^(x86_64|amd64)$")
        set(native_arch "x86_64")
    elseif(native_arch MATCHES "^(aarch64|arm64)$")
        set(native_arch "aarch64")
    else()
        set(native_arch "unknown")
    endif()
    add_test(NAME mvm_operating_system
        COMMAND "${CMAKE_COMMAND}"
            -DMC=$<TARGET_FILE:mvm-c>
            -DMVM=$<TARGET_FILE:mvm>
            -DSOURCE=${M_SOURCE_DIR}/mvm-c/tests/operating_system.m
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/operating-system.mvm
            "-DEXPECT=${native_os}\n${native_arch}\n"
            -P "${M_SOURCE_DIR}/mvm-c/tests/mvm_test.cmake"
    )
    set_tests_properties(mvm_operating_system PROPERTIES TIMEOUT 30)

    if(WIN32)
        set(native_extension ".dll")
    elseif(APPLE)
        set(native_extension ".dylib")
    else()
        set(native_extension ".so")
    endif()
    add_test(NAME mvm_native_library_resource
        COMMAND "${CMAKE_COMMAND}"
            -DMC=$<TARGET_FILE:mvm-c>
            -DMVM=$<TARGET_FILE:mvm>
            -DLIBRARY=$<TARGET_FILE:mvm_native_test_library>
            -DRESOURCE=native/${native_os}/${native_arch}/library${native_extension}
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/native-library-resource.mvm
            -P "${M_SOURCE_DIR}/mvm-c/tests/native_library_resource_test.cmake"
    )
    set_tests_properties(mvm_native_library_resource PROPERTIES TIMEOUT 30)

    add_test(NAME mvm_time_container
        COMMAND "${CMAKE_COMMAND}"
            -DMC=$<TARGET_FILE:mvm-c>
            -DMVM=$<TARGET_FILE:mvm>
            -DSOURCE=${M_SOURCE_DIR}/mvm-c/tests/time.m
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/time-container.mvm
            "-DEXPECT=time: OK\n"
            -P "${M_SOURCE_DIR}/mvm-c/tests/mvm_test.cmake"
    )
    add_test(NAME mvm_string_to_number_container
        COMMAND "${CMAKE_COMMAND}"
            -DMC=$<TARGET_FILE:mvm-c>
            -DMVM=$<TARGET_FILE:mvm>
            -DSOURCE=${M_SOURCE_DIR}/mvm-c/tests/string_to_number.m
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/string-to-number-container.mvm
            "-DEXPECT=string to number: OK\n"
            -P "${M_SOURCE_DIR}/mvm-c/tests/mvm_test.cmake"
    )
    add_test(NAME mvm_string_to_number_errors
        COMMAND "${CMAKE_COMMAND}"
            -DMC=$<TARGET_FILE:mvm-c>
            -DMVM=$<TARGET_FILE:mvm>
            -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/string-to-number-errors
            -P "${M_SOURCE_DIR}/mvm-c/tests/string_to_number_errors.cmake"
    )
    add_test(NAME mvm_modules_container
        COMMAND "${CMAKE_COMMAND}"
            -DMC=$<TARGET_FILE:mvm-c>
            -DMVM=$<TARGET_FILE:mvm>
            -DSOURCE=${M_SOURCE_DIR}/mvm-c/examples/modules.m
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/modules-container.mvm
            "-DEXPECT=510\nЦиклические модули собираются в один образ\n"
            -P "${M_SOURCE_DIR}/mvm-c/tests/mvm_test.cmake"
    )
    add_test(NAME mvm_array_container
        COMMAND "${CMAKE_COMMAND}"
            -DMC=$<TARGET_FILE:mvm-c>
            -DMVM=$<TARGET_FILE:mvm>
            -DSOURCE=${M_SOURCE_DIR}/mvm-c/tests/array.m
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/array-container.mvm
            "-DEXPECT=array: OK\n"
            -P "${M_SOURCE_DIR}/mvm-c/tests/mvm_test.cmake"
    )
    add_test(NAME mvm_array_errors
        COMMAND "${CMAKE_COMMAND}"
            -DMC=$<TARGET_FILE:mvm-c>
            -DMVM=$<TARGET_FILE:mvm>
            -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/array-errors
            -P "${M_SOURCE_DIR}/mvm-c/tests/array_errors.cmake"
    )
    add_test(NAME mvm_collections_container
        COMMAND "${CMAKE_COMMAND}"
            -DMC=$<TARGET_FILE:mvm-c>
            -DMVM=$<TARGET_FILE:mvm>
            -DSOURCE=${M_SOURCE_DIR}/mvm-c/examples/collections.m
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/collections-container.mvm
            "-DEXPECT=List: OK\nMap: OK\nStack: OK\nQueue: OK\nSet: OK\n"
            -P "${M_SOURCE_DIR}/mvm-c/tests/mvm_test.cmake"
    )
    add_test(NAME mvm_ternary_container
        COMMAND "${CMAKE_COMMAND}"
            -DMC=$<TARGET_FILE:mvm-c>
            -DMVM=$<TARGET_FILE:mvm>
            -DSOURCE=${M_SOURCE_DIR}/mvm-c/tests/ternary.m
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/ternary-container.mvm
            "-DEXPECT=42\n4\n2\n8\n1\n1\n2\n2\n12\nyes\n10\n3\n41\n"
            -P "${M_SOURCE_DIR}/mvm-c/tests/mvm_test.cmake"
    )
    add_test(NAME mvm_generics_container
        COMMAND "${CMAKE_COMMAND}"
            -DMC=$<TARGET_FILE:mvm-c>
            -DMVM=$<TARGET_FILE:mvm>
            -DSOURCE=${M_SOURCE_DIR}/mvm-c/examples/generics.m
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/generics-container.mvm
            "-DEXPECT=42\n7\n97\nok\n4\n8\n1\n1\n0\n1\n1\n1\n8\n5\nnested\n1\n"
            -P "${M_SOURCE_DIR}/mvm-c/tests/mvm_test.cmake"
    )
    add_test(NAME mvm_generics_modules_container
        COMMAND "${CMAKE_COMMAND}"
            -DMC=$<TARGET_FILE:mvm-c>
            -DMVM=$<TARGET_FILE:mvm>
            -DSOURCE=${M_SOURCE_DIR}/mvm-c/examples/generics_modules.m
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/generics-modules-container.mvm
            "-DEXPECT=77\n1\n"
            -P "${M_SOURCE_DIR}/mvm-c/tests/mvm_test.cmake"
    )
    add_test(NAME mvm_generics_signature_container
        COMMAND "${CMAKE_COMMAND}"
            -DMC=$<TARGET_FILE:mvm-c>
            -DMVM=$<TARGET_FILE:mvm>
            -DSOURCE=${M_SOURCE_DIR}/mvm-c/examples/generics_signature.m
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/generics-signature-container.mvm
            "-DEXPECT=88\n"
            -P "${M_SOURCE_DIR}/mvm-c/tests/mvm_test.cmake"
    )
endif()

if(BUILD_TESTING)
    foreach(union_target IN ITEMS union_test union_oom_test)
        if(union_target STREQUAL "union_oom_test")
            add_executable(${union_target} tests/union_test.c tests/array_allocator.c ${MVM_ARRAY_TEST_SOURCES})
            target_compile_definitions(${union_target} PRIVATE UNION_TEST_ALLOCATOR=1)
        else()
            add_executable(${union_target} tests/union_test.c ${MVM_RUNTIME_SOURCES})
        endif()
        target_include_directories(${union_target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
        set_target_properties(${union_target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
        target_link_libraries(${union_target} PRIVATE PkgConfig::LIBFFI ${CMAKE_DL_LIBS} Threads::Threads ${MVM_SYSTEM_LIBRARIES} mvm_zip mvm_toml)
        if(MSVC)
            target_compile_options(${union_target} PRIVATE /W4)
        else()
            target_compile_options(${union_target} PRIVATE -Wall -Wextra)
            target_link_libraries(${union_target} PRIVATE m)
        endif()
        add_test(NAME ${union_target} COMMAND ${union_target})
    endforeach()
endif()

if(BUILD_TESTING)
    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME memory_cli
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/memory_cli_test.py"
                --mc $<TARGET_FILE:mvm-c> --mvm $<TARGET_FILE:mvm>
                --work-dir "${CMAKE_CURRENT_BINARY_DIR}/memory-cli")
        set_tests_properties(memory_cli PROPERTIES TIMEOUT 120)
        add_test(NAME union_image_validation
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/union_image_test.py"
                --mc $<TARGET_FILE:mvm-c> --mvm $<TARGET_FILE:mvm>
                --work-dir "${CMAKE_CURRENT_BINARY_DIR}/union-image-validation")
        add_test(NAME debug_cli
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/debug_cli_test.py"
                --mc $<TARGET_FILE:mvm-c> --mvm $<TARGET_FILE:mvm>
                --work-dir "${CMAKE_CURRENT_BINARY_DIR}/debug-cli")
        set_tests_properties(debug_cli PROPERTIES TIMEOUT 120)
        add_test(NAME mc_mo_peephole
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/mo_peephole_test.py"
                --mc $<TARGET_FILE:mvm-c> --mvm $<TARGET_FILE:mvm>
                --work-dir "${CMAKE_CURRENT_BINARY_DIR}/mo-peephole")
        add_test(NAME mc_local_jump
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/local_jump_test.py"
                --mc $<TARGET_FILE:mvm-c> --mvm $<TARGET_FILE:mvm>
                --source "${M_SOURCE_DIR}/mvm-c/tests/local_jump.m"
                --work-dir "${CMAKE_CURRENT_BINARY_DIR}/local-jump")
    endif()
endif()

add_subdirectory(benchmarks)

if(BUILD_TESTING)
    add_subdirectory("${M_SOURCE_DIR}/mvm-templates/hello-world"
        "${CMAKE_CURRENT_BINARY_DIR}/mvm-templates/hello-world")
    add_subdirectory("${M_SOURCE_DIR}/mvm-templates/native-library"
        "${CMAKE_CURRENT_BINARY_DIR}/mvm-templates/native-library")
endif()

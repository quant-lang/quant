cmake_minimum_required(VERSION 3.20)

project(Quant VERSION 0.42 LANGUAGES CXX)
include(FetchContent)

# Build type default
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Build type" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        "Debug" "Release" "RelWithDebInfo" "MinSizeRel")
endif()

# C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Output all binaries into one place, easier for scripts/CI to find qu
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Project-wide options
option(QUANT_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)
option(QUANT_ENABLE_LTO "Enable interprocedural optimization in Release builds" ON)
option(QUANT_ENABLE_ASAN "Enable AddressSanitizer/UBSan in Debug builds" OFF)
option(QUANT_ENABLE_TESTS "Build tests" OFF)
option(QUANT_ENABLE_BENCH "Build the IR optimizer benchmark (quant_bench)" ON)

# Self-hosted frontend: 'qu' (C++) or 'quant' (self-hosted Quant)
# When set to 'quant', the C++ compiler spawns the self-hosted frontend
# binary for parsing and deserializes its AST output
set(QUANT_FRONTEND "qu" CACHE STRING "Frontend to use: qu (C++, default) or quant (self-hosted)")
set_property(CACHE QUANT_FRONTEND PROPERTY STRINGS "qu" "quant")

# Self-hosted frontend integration (auto-detected at runtime).
# When ON, the C++ compiler looks for src/self-hosted/out and spawns it
# to parse .qu files, then deserializes the QAST output
option(QUANT_SELF_HOSTED "Enable self-hosted Quant frontend (requires pre-built out binary)" ON)

# Backends compiled into the compiler binary. CMake answers
# "what targets can this compiler build for?"; --target then picks one of the
# enabled entries at compile time (see src/backend/targets.cpp).
set(QUANT_KNOWN_BACKENDS "x86_64-linux;aarch64-linux;x86_64-windows;aarch64-zeropoint")
if(NOT DEFINED QUANT_BACKENDS OR QUANT_BACKENDS STREQUAL "")
  # The compiler is host-independent: every build can emit any target that
  # was compiled in. Enable all known backends by default.
  set(QUANT_BACKENDS "${QUANT_KNOWN_BACKENDS}")
endif()
foreach(_backend ${QUANT_BACKENDS})
  if(NOT _backend IN_LIST QUANT_KNOWN_BACKENDS)
    message(FATAL_ERROR
      "Unknown backend '${_backend}' in QUANT_BACKENDS. Known backends: ${QUANT_KNOWN_BACKENDS}")
  endif()
endforeach()
message(STATUS "Quant backends: ${QUANT_BACKENDS}")

# Optional default target used when no --target is given on the command line.
# By default the compiler picks its host-native backend; set this for
# cross-compilation-oriented builds (e.g. a ZeroPoint-only compiler).
set(QUANT_DEFAULT_TARGET "" CACHE STRING
    "Default --target when none is passed (must be one of QUANT_BACKENDS)")
if(QUANT_DEFAULT_TARGET)
  if(NOT QUANT_DEFAULT_TARGET IN_LIST QUANT_KNOWN_BACKENDS)
    message(FATAL_ERROR
      "Unknown backend '${QUANT_DEFAULT_TARGET}' in QUANT_DEFAULT_TARGET. Known backends: ${QUANT_KNOWN_BACKENDS}")
  endif()
  if(NOT QUANT_DEFAULT_TARGET IN_LIST QUANT_BACKENDS)
    message(FATAL_ERROR
      "Default target '${QUANT_DEFAULT_TARGET}' must also be enabled in QUANT_BACKENDS (${QUANT_BACKENDS})")
  endif()
endif()

# Derive feature flags from the backend list: only the selected architectures
# and executable flavors are compiled into the binary at all.
set(QUANT_HAS_X86_64 OFF)
set(QUANT_HAS_AARCH64 OFF)
set(QUANT_HAS_PE OFF)
set(QUANT_HAS_ELF OFF)
foreach(_backend ${QUANT_BACKENDS})
  if(_backend MATCHES "^x86_64")
    set(QUANT_HAS_X86_64 ON)
  elseif(_backend MATCHES "^aarch64")
    set(QUANT_HAS_AARCH64 ON)
  endif()
  if(_backend MATCHES "-windows$")
    set(QUANT_HAS_PE ON)
  else()
    set(QUANT_HAS_ELF ON)
  endif()
endforeach()

# Compiler warnings / flags, branched per compiler
add_library(quant_compiler_flags INTERFACE)

if(MSVC)
  target_compile_options(quant_compiler_flags INTERFACE
        /W4
        /permissive-
        /Zc:__cplusplus
        # Serialize PDB writes across parallel cl.exe instances (fixes C1041).
        /FS
        $<$<CONFIG:Debug>:/RTC1>
    )
  if(QUANT_WARNINGS_AS_ERRORS)
    target_compile_options(quant_compiler_flags INTERFACE /WX)
  endif()
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(quant_compiler_flags INTERFACE
        -Wall -Wextra -Wpedantic
        -Wshadow -Wconversion -Wsign-conversion
        -Wnon-virtual-dtor -Woverloaded-virtual
        $<$<CONFIG:Debug>:-Og -g>
        $<$<CONFIG:Release>:-O3>
    )
  if(QUANT_WARNINGS_AS_ERRORS)
    target_compile_options(quant_compiler_flags INTERFACE -Werror)
  endif()
  if(QUANT_ENABLE_ASAN)
    target_compile_options(quant_compiler_flags INTERFACE
            $<$<CONFIG:Debug>:-fsanitize=address,undefined -fno-omit-frame-pointer>)
    target_link_options(quant_compiler_flags INTERFACE
            $<$<CONFIG:Debug>:-fsanitize=address,undefined>)
  endif()
endif()

# Recursive passes (parser/semantic/IRGen) need more than the 1 MB default
# stack Windows gives an executable regardless of host compiler.
if(MSVC)
  string(APPEND CMAKE_EXE_LINKER_FLAGS " /STACK:16777216")
elseif(WIN32)
  string(APPEND CMAKE_EXE_LINKER_FLAGS " -Wl,--stack=16777216")
endif()

# LTO only for optimized configs, and only if the toolchain actually supports it
if(QUANT_ENABLE_LTO)
  include(CheckIPOSupported)
  check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error)
  if(ipo_supported)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL ON)
  else()
    message(STATUS "LTO not supported by this toolchain: ${ipo_error}")
  endif()
endif()

# Embed the stdlib (.qu sources) into the compiler binary.
set(STD_EMBED_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/quant_std_embedded.h")
file(GLOB_RECURSE STD_QU_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/std/*.qu")

add_custom_command(
    OUTPUT "${STD_EMBED_HEADER}"
    COMMAND ${CMAKE_COMMAND}
        -DQUANT_STD_DIR=${CMAKE_CURRENT_SOURCE_DIR}/std
        -DOUTPUT_HEADER=${STD_EMBED_HEADER}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/scripts/embed_std.cmake
    DEPENDS ${STD_QU_FILES} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/embed_std.cmake
    COMMENT "Embedding stdlib into the compiler"
    VERBATIM
)

# Explicit target so other targets can depend on it without pulling the header
# into a sources list (cleaner separation, avoids surprises with IDE file lists).
add_custom_target(quant_std_embed DEPENDS "${STD_EMBED_HEADER}")

# Library libquant (C++ files).
# Backend sources are compiled in only when a backend that needs them is
# enabled: an x86_64-only build contains no AArch64 codegen and no PE writer.
set(QUANT_BACKEND_SOURCES
    src/backend/native_backend.cpp
    src/backend/targets.cpp
)
if(QUANT_HAS_X86_64)
  list(APPEND QUANT_BACKEND_SOURCES src/backend/isel.cpp src/backend/x86_64.cpp)
endif()
if(QUANT_HAS_AARCH64)
  list(APPEND QUANT_BACKEND_SOURCES src/backend/aarch64_isel.cpp src/backend/aarch_64.cpp)
endif()
if(QUANT_HAS_ELF)
  list(APPEND QUANT_BACKEND_SOURCES src/backend/elf_writer.cpp)
endif()
if(QUANT_HAS_PE)
  list(APPEND QUANT_BACKEND_SOURCES src/backend/pe_writer.cpp)
endif()

add_library(libquant
    src/frontend/ast.cpp
    src/frontend/ast_deserialize.cpp
    src/frontend/lexer.cpp
    src/frontend/parser.cpp
    src/semantic/semantic.cpp
    src/semantic/symbol_table.cpp
    src/utils/logger.cpp
    src/ir/ir_gen.cpp
    src/ir/ir_dump.cpp
    src/ir/opt.cpp
    src/modules/modules.cpp
    ${QUANT_BACKEND_SOURCES}
    src/backend/fasmcodegen.cpp
    src/utils/file_manager.cpp
    src/utils/options.cpp
    src/utils/errors.cpp
    src/linker/linker.cpp
    src/support/type_context.cpp
    src/support/symbol_path.cpp
    src/comptime/evaluator.cpp
)
add_dependencies(libquant quant_std_embed)

set_target_properties(libquant PROPERTIES
    PREFIX ""
    POSITION_INDEPENDENT_CODE ON
)

target_link_libraries(libquant PUBLIC quant_compiler_flags)

# Dependencies
FetchContent_Declare(arena-alloc
    GIT_REPOSITORY https://github.com/grafmorkov/quant-alloc.git
    GIT_TAG        v1.0.2
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(arena-alloc)

add_library(quant-alloc INTERFACE)
# SYSTEM keeps third-party headers from triggering your -Wall/-Wextra/-Wpedantic
target_include_directories(quant-alloc SYSTEM INTERFACE ${arena-alloc_SOURCE_DIR})

target_include_directories(libquant
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/include/utils
        ${CMAKE_CURRENT_BINARY_DIR}/generated
)
target_compile_definitions(libquant PUBLIC
    QUANT_ROOT="${PROJECT_SOURCE_DIR}"
)
# QUANT_BACKENDS is a CMake list (semicolons); pass it to C++ as a
# comma-separated string literal so no escaping survives the command line.
list(JOIN QUANT_BACKENDS "," QUANT_BACKENDS_CSV)
set(QUANT_BACKEND_DEFINES
    "QUANT_ENABLED_BACKENDS=\"${QUANT_BACKENDS_CSV}\""
)
if(QUANT_HAS_X86_64)
  list(APPEND QUANT_BACKEND_DEFINES QUANT_HAS_X86_64)
endif()
if(QUANT_HAS_AARCH64)
  list(APPEND QUANT_BACKEND_DEFINES QUANT_HAS_AARCH64)
endif()
if(QUANT_HAS_ELF)
  list(APPEND QUANT_BACKEND_DEFINES QUANT_HAS_ELF)
endif()
if(QUANT_HAS_PE)
  list(APPEND QUANT_BACKEND_DEFINES QUANT_HAS_PE)
endif()
if(QUANT_DEFAULT_TARGET)
  list(APPEND QUANT_BACKEND_DEFINES "QUANT_DEFAULT_TARGET=\"${QUANT_DEFAULT_TARGET}\"")
endif()
target_compile_definitions(libquant PUBLIC ${QUANT_BACKEND_DEFINES})
target_link_libraries(libquant PUBLIC quant-alloc)

# Executable qu (main.cpp)
add_executable(qu
    src/main.cpp
)
target_link_libraries(qu
    PRIVATE libquant quant_compiler_flags
)

# Self-hosted frontend
if(QUANT_SELF_HOSTED)
  set(QUANT_SELF_HOSTED_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/src/self-hosted")

  set(QUANT_SELF_HOSTED_SOURCE
        "${QUANT_SELF_HOSTED_DIR}/main.qu")

  set(QUANT_SELF_HOSTED_BIN
        "${QUANT_SELF_HOSTED_DIR}/out")

  add_custom_command(
        TARGET qu POST_BUILD
        COMMAND "$<TARGET_FILE:qu>"
            "${QUANT_SELF_HOSTED_SOURCE}"
            -o "${QUANT_SELF_HOSTED_BIN}"
            -O3
            --time
        BYPRODUCTS "${QUANT_SELF_HOSTED_BIN}"
        COMMENT "Building self-hosted Quant frontend"
        VERBATIM
  )

  target_compile_definitions(qu PRIVATE
        QUANT_SELF_HOSTED_ENABLED
        QUANT_SELF_HOSTED_BIN="${QUANT_SELF_HOSTED_BIN}"
    )
endif()

# IR optimizer benchmark (benchmark.cpp): times optimize() per level
if(QUANT_ENABLE_BENCH)
  add_executable(quant_bench benchmark.cpp)
  target_link_libraries(quant_bench PRIVATE libquant quant_compiler_flags)
endif()

# Install rules
include(GNUInstallDirs)
install(TARGETS qu
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# Tests (optional)
if(QUANT_ENABLE_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

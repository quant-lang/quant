cmake_minimum_required(VERSION 3.20)

project(Quant VERSION 0.42 LANGUAGES CXX)

include(FetchContent)

# ---------------------------------------------------------------------------
# Build configuration
# ---------------------------------------------------------------------------

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Build type" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        "Debug" "Release" "RelWithDebInfo" "MinSizeRel")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# One place for every produced binary/library, so scripts and CI find them.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

option(QUANT_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)
option(QUANT_ENABLE_LTO "Enable interprocedural optimization in Release builds" ON)
option(QUANT_ENABLE_ASAN "Enable AddressSanitizer/UBSan in Debug builds" OFF)
option(QUANT_ENABLE_TESTS "Build tests" OFF)
option(QUANT_ENABLE_BENCH "Build the IR optimizer benchmark (quant_bench)" ON)
option(QUANT_USE_LIBQU
       "Embed a prebuilt stdlib (libqu-<arch>-linux.a) into the compiler instead of \
regenerating stdlib code on every run (Linux only)" OFF)
set(QUANT_LD "" CACHE STRING
    "Linker for the libqu build: ld, ld.lld, mold, or a path (empty: pick by target)")
set(QUANT_AR "" CACHE STRING
    "Archiver for the libqu build: ar, llvm-ar, or a path (empty: ar)")

# ---------------------------------------------------------------------------
# Native backend
#
# qu-backend owns the list of supported targets and validates QUANT_BACKENDS
# and QUANT_DEFAULT_TARGET. It defines the quant-backend target, which the
# compiler links below.
# ---------------------------------------------------------------------------

set(QUANT_BACKEND_SOURCE_DIR "" CACHE PATH
    "Local checkout of qu-backend (empty: fetch from Git)")
set(QUANT_BACKEND_GIT_REPOSITORY
    "https://github.com/quant-lang/qu-backend.git" CACHE STRING
    "Git repository for qu-backend when no local checkout is used")
set(QUANT_BACKEND_GIT_TAG "main" CACHE STRING
    "Git tag or branch for qu-backend when no local checkout is used")

if(NOT QUANT_BACKEND_SOURCE_DIR)
  get_filename_component(_quant_backend_sibling
      "${PROJECT_SOURCE_DIR}/../qu-backend" ABSOLUTE)
  if(EXISTS "${_quant_backend_sibling}/CMakeLists.txt")
    set(QUANT_BACKEND_SOURCE_DIR "${_quant_backend_sibling}")
  endif()
endif()

if(QUANT_BACKEND_SOURCE_DIR)
  FetchContent_Declare(quant-backend
      SOURCE_DIR "${QUANT_BACKEND_SOURCE_DIR}")
else()
  FetchContent_Declare(quant-backend
      GIT_REPOSITORY "${QUANT_BACKEND_GIT_REPOSITORY}"
      GIT_TAG        "${QUANT_BACKEND_GIT_TAG}"
      GIT_SHALLOW    TRUE)
endif()
FetchContent_MakeAvailable(quant-backend)

# Linux targets are the ones that get a stdlib archive built below.
set(QUANT_LINUX_TARGETS "")

foreach(_backend IN LISTS QUANT_BACKENDS)
  if(_backend MATCHES "-linux$")
    list(APPEND QUANT_LINUX_TARGETS ${_backend})
  endif()
endforeach()

if(QUANT_USE_LIBQU)
  if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR
      "QUANT_USE_LIBQU builds the stdlib with a host compiler binary, so it needs a Linux host")
  endif()
  if(NOT QUANT_LINUX_TARGETS)
    message(FATAL_ERROR
      "QUANT_USE_LIBQU needs at least one *-linux backend in QUANT_BACKENDS (got ${QUANT_BACKENDS})")
  endif()
  message(STATUS "Quant stdlib archives: ${QUANT_LINUX_TARGETS}")
endif()

# ---------------------------------------------------------------------------
# Compiler flags
# ---------------------------------------------------------------------------

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

# ---------------------------------------------------------------------------
# Embedded stdlib sources (always compiled in)
#
# The .qu sources are needed at runtime for parsing and semantic analysis even
# when the machine code comes from a prebuilt archive (QUANT_USE_LIBQU).
# ---------------------------------------------------------------------------

file(GLOB_RECURSE QUANT_STD_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/std/*.qu")

set(QUANT_STD_EMBED_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/quant_std_embedded.h")

add_custom_command(
    OUTPUT "${QUANT_STD_EMBED_HEADER}"
    COMMAND ${CMAKE_COMMAND}
        -DQUANT_STD_DIR=${CMAKE_CURRENT_SOURCE_DIR}/std
        -DOUTPUT_HEADER=${QUANT_STD_EMBED_HEADER}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/scripts/embed_std.cmake
    DEPENDS ${QUANT_STD_SOURCES} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/embed_std.cmake
    COMMENT "Embedding stdlib sources into the compiler"
    VERBATIM
)

add_custom_target(quant_std_embed DEPENDS "${QUANT_STD_EMBED_HEADER}")

# ---------------------------------------------------------------------------
# libquant: the compiler itself (C++)
# ---------------------------------------------------------------------------

add_library(libquant
    src/frontend/ast.cpp
    src/frontend/lexer.cpp
    src/frontend/parser.cpp
    src/semantic/semantic.cpp
    src/semantic/symbol_table.cpp
    src/utils/logger.cpp
    src/ir/ir_gen.cpp
    src/ir/opt.cpp
    src/modules/modules.cpp
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

FetchContent_Declare(arena-alloc
    GIT_REPOSITORY https://github.com/grafmorkov/quant-alloc.git
    GIT_TAG        v1.0.2
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(arena-alloc)

add_library(quant-alloc INTERFACE)
# SYSTEM keeps third-party headers out of our -Wall/-Wextra/-Wpedantic.
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

target_link_libraries(libquant PUBLIC quant-alloc quant-backend)

# ---------------------------------------------------------------------------
# Executables
# ---------------------------------------------------------------------------

add_executable(qu src/main.cpp)
target_link_libraries(qu PRIVATE libquant quant_compiler_flags)

# QUANT_USE_LIBQU: build std/std.qu into one archive per Linux target with a
# bootstrap compiler (this very source tree, sources embedded), then embed the
# archives into qu. qu still embeds the .qu sources for semantic analysis, but
# links stdlib machine code from the archive instead of regenerating it.
if(QUANT_USE_LIBQU)
  add_executable(qu_bootstrap src/main.cpp)
  target_link_libraries(qu_bootstrap PRIVATE libquant quant_compiler_flags)
  set_target_properties(qu_bootstrap PROPERTIES OUTPUT_NAME qu-bootstrap)

  set(QUANT_LIBQU_ARCHIVES "")
  set(QUANT_LIBQU_SPEC "")

  foreach(_target IN LISTS QUANT_LINUX_TARGETS)
    string(REGEX REPLACE "-linux$" "" _arch "${_target}")
    set(_archive "${CMAKE_ARCHIVE_OUTPUT_DIRECTORY}/libqu-${_arch}-linux.a")

    set(_build_cmd $<TARGET_FILE:qu_bootstrap>
        --static-lib ${CMAKE_CURRENT_SOURCE_DIR}/std/std.qu
        -o ${_archive} --target ${_target})
    if(QUANT_LD)
      list(APPEND _build_cmd --ld ${QUANT_LD})
    endif()
    if(QUANT_AR)
      list(APPEND _build_cmd --ar ${QUANT_AR})
    endif()

    add_custom_command(
        OUTPUT ${_archive}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_ARCHIVE_OUTPUT_DIRECTORY}
        COMMAND ${_build_cmd}
        DEPENDS qu_bootstrap
        COMMENT "Building stdlib libqu-${_arch}-linux.a (${_target})"
        VERBATIM
    )

    list(APPEND QUANT_LIBQU_ARCHIVES ${_archive})
    if(QUANT_LIBQU_SPEC)
      string(APPEND QUANT_LIBQU_SPEC "|")
    endif()
    string(APPEND QUANT_LIBQU_SPEC "${_target}=${_archive}")
  endforeach()

  set(QUANT_LIBQU_EMBED_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/quant_libqu_embedded.h")

  add_custom_command(
      OUTPUT "${QUANT_LIBQU_EMBED_HEADER}"
      COMMAND ${CMAKE_COMMAND}
          -DQUANT_LIBQU_SPEC=${QUANT_LIBQU_SPEC}
          -DOUTPUT_HEADER=${QUANT_LIBQU_EMBED_HEADER}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/scripts/embed_lib.cmake
      DEPENDS ${QUANT_LIBQU_ARCHIVES} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/embed_lib.cmake
      COMMENT "Embedding stdlib archives into the compiler"
      VERBATIM
  )

  add_custom_target(quant_libqu
      DEPENDS ${QUANT_LIBQU_ARCHIVES} "${QUANT_LIBQU_EMBED_HEADER}")

  add_dependencies(qu quant_libqu)
  target_compile_definitions(qu PRIVATE QUANT_USE_LIBQU=1)
endif()

if(QUANT_ENABLE_BENCH)
  add_executable(quant_bench benchmark.cpp)
  target_link_libraries(quant_bench PRIVATE libquant quant_compiler_flags)
endif()

# ---------------------------------------------------------------------------
# Install and tests
# ---------------------------------------------------------------------------

include(GNUInstallDirs)
install(TARGETS qu
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
if(QUANT_USE_LIBQU)
  install(FILES ${QUANT_LIBQU_ARCHIVES} DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()

if(QUANT_ENABLE_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

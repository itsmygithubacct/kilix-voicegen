cmake_minimum_required(VERSION 3.24)

project(kilix_voicegen VERSION 0.1.0 LANGUAGES C CXX)

include(GNUInstallDirs)
include(CTest)

option(KGV_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(KGV_WARNINGS_AS_ERRORS "Treat project compiler warnings as errors" ON)

set(KGV_UTF8PROC_SOURCE_DIR "" CACHE PATH
  "Clean source checkout of the pinned utf8proc revision")
if(KGV_UTF8PROC_SOURCE_DIR STREQUAL "" AND DEFINED ENV{KGV_UTF8PROC_SOURCE_DIR})
  set(KGV_UTF8PROC_SOURCE_DIR "$ENV{KGV_UTF8PROC_SOURCE_DIR}")
endif()
if(NOT EXISTS "${KGV_UTF8PROC_SOURCE_DIR}/CMakeLists.txt")
  message(FATAL_ERROR
    "Set KGV_UTF8PROC_SOURCE_DIR to the clean pinned utf8proc source checkout")
endif()
execute_process(
  COMMAND git -C "${KGV_UTF8PROC_SOURCE_DIR}" rev-parse HEAD
  RESULT_VARIABLE KGV_UTF8PROC_REVISION_RESULT
  OUTPUT_VARIABLE KGV_UTF8PROC_REVISION
  ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(
  COMMAND git -C "${KGV_UTF8PROC_SOURCE_DIR}" status --porcelain=v1
  RESULT_VARIABLE KGV_UTF8PROC_STATUS_RESULT
  OUTPUT_VARIABLE KGV_UTF8PROC_STATUS
  ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT KGV_UTF8PROC_REVISION_RESULT EQUAL 0 OR
   NOT KGV_UTF8PROC_STATUS_RESULT EQUAL 0 OR
   NOT KGV_UTF8PROC_REVISION STREQUAL "0075ed7d0adba45682ee6bf7a83b10f8fd110163" OR
   NOT KGV_UTF8PROC_STATUS STREQUAL "")
  message(FATAL_ERROR "utf8proc source must be clean at the locked revision")
endif()
set(UTF8PROC_INSTALL OFF CACHE BOOL "Do not install the private utf8proc build target" FORCE)
set(UTF8PROC_ENABLE_TESTING OFF CACHE BOOL "Do not build upstream tests" FORCE)
add_subdirectory("${KGV_UTF8PROC_SOURCE_DIR}"
  "${CMAKE_CURRENT_BINARY_DIR}/_deps/utf8proc" EXCLUDE_FROM_ALL)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

function(kgv_project_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4)
    if(KGV_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow)
    if(KGV_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()

function(kgv_sanitizers target)
  if(KGV_ENABLE_SANITIZERS)
    if(MSVC)
      message(FATAL_ERROR "KGV_ENABLE_SANITIZERS is not configured for MSVC")
    endif()
    target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
  endif()
endfunction()

add_library(kilix_voicegen_frontend STATIC
  src/frontend/frontend.cpp
  src/frontend/pipeline.cpp)
target_include_directories(kilix_voicegen_frontend PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/src)
set_target_properties(kilix_voicegen_frontend PROPERTIES
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN YES)
kgv_project_warnings(kilix_voicegen_frontend)
kgv_sanitizers(kilix_voicegen_frontend)
target_link_libraries(kilix_voicegen_frontend PUBLIC utf8proc::utf8proc)

add_library(kilix_voicegen SHARED
  src/runtime/engine.cpp
  src/runtime/json.cpp
  src/runtime/model_package.cpp
  src/runtime/sha256.cpp)
add_library(Kilix::Voicegen ALIAS kilix_voicegen)

target_compile_definitions(kilix_voicegen PRIVATE KGV_BUILDING_LIBRARY=1)
target_link_libraries(kilix_voicegen PRIVATE kilix_voicegen_frontend)
target_include_directories(kilix_voicegen
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
set_target_properties(kilix_voicegen PROPERTIES
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN YES
  VERSION ${PROJECT_VERSION}
  SOVERSION 1)
if(UNIX AND NOT APPLE)
  target_link_options(kilix_voicegen PRIVATE
    "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/cmake/kilix_voicegen.map")
endif()
kgv_project_warnings(kilix_voicegen)
kgv_sanitizers(kilix_voicegen)

add_executable(kilix-voicegen src/cli/main.cpp)
target_link_libraries(kilix-voicegen PRIVATE Kilix::Voicegen kilix_voicegen_frontend)
target_include_directories(kilix-voicegen PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(kilix-voicegen PRIVATE
  KGV_PROJECT_VERSION="${PROJECT_VERSION}")
if(UNIX AND NOT APPLE)
  set_target_properties(kilix-voicegen PROPERTIES
    INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
endif()
kgv_project_warnings(kilix-voicegen)
kgv_sanitizers(kilix-voicegen)

install(TARGETS kilix_voicegen kilix-voicegen
  EXPORT KilixVoicegenTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES include/kilix_voicegen.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES LICENSE NOTICE THIRD_PARTY.md VERSION
  DESTINATION ${CMAKE_INSTALL_DATADIR}/kilix-voicegen)
install(FILES "${KGV_UTF8PROC_SOURCE_DIR}/LICENSE.md"
  DESTINATION ${CMAKE_INSTALL_DATADIR}/kilix-voicegen/licenses
  RENAME utf8proc-LICENSE.md)
install(DIRECTORY schemas/ DESTINATION ${CMAKE_INSTALL_DATADIR}/kilix-voicegen/schemas
  FILES_MATCHING PATTERN "*.json")
install(EXPORT KilixVoicegenTargets
  FILE KilixVoicegenTargets.cmake
  NAMESPACE Kilix::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/KilixVoicegen)

if(BUILD_TESTING)
  find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)

  set(KGV_SANITIZER_TEST_ENV)
  if(KGV_ENABLE_SANITIZERS AND NOT MSVC)
    # The host Python executable is not sanitizer-linked. Let the instrumented
    # ctypes library initialize ASan when dlopened, and do leak checking in the
    # native executables where allocations belong entirely to this project.
    list(APPEND KGV_SANITIZER_TEST_ENV
      "ASAN_OPTIONS=verify_asan_link_order=0:detect_leaks=0:halt_on_error=1")
  endif()

  set(KGV_FIXTURE_MODEL_DIR "${CMAKE_CURRENT_BINARY_DIR}/fixture-model")
  execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/make_fixture_model.py"
            --output "${KGV_FIXTURE_MODEL_DIR}"
            --graph "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/fixture.graph"
    RESULT_VARIABLE KGV_FIXTURE_RESULT
    OUTPUT_VARIABLE KGV_FIXTURE_OUTPUT
    ERROR_VARIABLE KGV_FIXTURE_ERROR)
  if(NOT KGV_FIXTURE_RESULT EQUAL 0)
    message(FATAL_ERROR "Could not generate fixture model: ${KGV_FIXTURE_ERROR}")
  endif()
  file(READ "${KGV_FIXTURE_MODEL_DIR}/RELEASE.sha256" KGV_FIXTURE_RELEASE_SHA)
  string(STRIP "${KGV_FIXTURE_RELEASE_SHA}" KGV_FIXTURE_RELEASE_SHA)

  add_executable(test_c_abi tests/test_c_abi.c)
  target_link_libraries(test_c_abi PRIVATE Kilix::Voicegen)
  kgv_project_warnings(test_c_abi)
  kgv_sanitizers(test_c_abi)
  add_test(NAME c_abi COMMAND test_c_abi
    "${KGV_FIXTURE_MODEL_DIR}" "${KGV_FIXTURE_RELEASE_SHA}")

  if(UNIX AND NOT APPLE AND CMAKE_NM)
    add_test(NAME exported_abi
      COMMAND "${CMAKE_COMMAND}"
        "-DKGV_LIBRARY=$<TARGET_FILE:kilix_voicegen>"
        "-DKGV_NM=${CMAKE_NM}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_exports.cmake")
  endif()

  add_executable(test_sha256 tests/test_sha256.cpp src/runtime/sha256.cpp)
  target_include_directories(test_sha256 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  kgv_project_warnings(test_sha256)
  kgv_sanitizers(test_sha256)
  add_test(NAME sha256 COMMAND test_sha256)

  add_executable(test_json tests/test_json.cpp src/runtime/json.cpp)
  target_include_directories(test_json PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  kgv_project_warnings(test_json)
  kgv_sanitizers(test_json)
  add_test(NAME strict_json COMMAND test_json)

  add_test(NAME python_binding
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_binding.py")
  set(KGV_BINDING_TEST_ENV
    "KGV_LIBRARY=$<TARGET_FILE:kilix_voicegen>"
    "KGV_FIXTURE_DIR=${KGV_FIXTURE_MODEL_DIR}"
    "KGV_FIXTURE_SHA256=${KGV_FIXTURE_RELEASE_SHA}"
    "PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}/bindings/python"
    "PYTHONDONTWRITEBYTECODE=1")
  list(APPEND KGV_BINDING_TEST_ENV ${KGV_SANITIZER_TEST_ENV})
  set_tests_properties(python_binding PROPERTIES ENVIRONMENT "${KGV_BINDING_TEST_ENV}")

  add_test(NAME frontend_conformance
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_conformance.py")
  set(KGV_CONFORMANCE_TEST_ENV
    "KGV_LIBRARY=$<TARGET_FILE:kilix_voicegen>"
    "KGV_CLI=$<TARGET_FILE:kilix-voicegen>"
    "KGV_FIXTURE_DIR=${KGV_FIXTURE_MODEL_DIR}"
    "KGV_FIXTURE_SHA256=${KGV_FIXTURE_RELEASE_SHA}"
    "PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}/bindings/python"
    "PYTHONDONTWRITEBYTECODE=1")
  list(APPEND KGV_CONFORMANCE_TEST_ENV ${KGV_SANITIZER_TEST_ENV})
  set_tests_properties(frontend_conformance PROPERTIES ENVIRONMENT "${KGV_CONFORMANCE_TEST_ENV}")

  add_test(NAME model_verifier
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_model_verifier.py")
  set(KGV_MODEL_TEST_ENV
    "KGV_LIBRARY=$<TARGET_FILE:kilix_voicegen>"
    "KGV_FIXTURE_GRAPH=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/fixture.graph"
    "PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}/bindings/python"
    "PYTHONDONTWRITEBYTECODE=1")
  list(APPEND KGV_MODEL_TEST_ENV ${KGV_SANITIZER_TEST_ENV})
  set_tests_properties(model_verifier PROPERTIES ENVIRONMENT "${KGV_MODEL_TEST_ENV}")

  add_test(NAME cli
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cli.py")
  set(KGV_CLI_TEST_ENV
    "KGV_CLI=$<TARGET_FILE:kilix-voicegen>"
    "KGV_FIXTURE_DIR=${KGV_FIXTURE_MODEL_DIR}"
    "KGV_FIXTURE_SHA256=${KGV_FIXTURE_RELEASE_SHA}"
    "PYTHONDONTWRITEBYTECODE=1")
  set_tests_properties(cli PROPERTIES ENVIRONMENT "${KGV_CLI_TEST_ENV}")

  add_test(NAME repository_boundaries
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_repository.py")
  set_tests_properties(repository_boundaries PROPERTIES ENVIRONMENT
    "PYTHONDONTWRITEBYTECODE=1")

  add_test(NAME lexicon_candidate
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_lexicon_candidate.py")
  set(KGV_LEXICON_TEST_ENV
    "KGV_LEXICON_TOOL=${CMAKE_CURRENT_SOURCE_DIR}/tools/build_en_au_lexicon_candidate.py"
    "PYTHONDONTWRITEBYTECODE=1")
  set_tests_properties(lexicon_candidate PROPERTIES ENVIRONMENT
    "${KGV_LEXICON_TEST_ENV}")

  add_executable(test_frontend tests/test_frontend.cpp)
  target_link_libraries(test_frontend PRIVATE kilix_voicegen_frontend)
  target_include_directories(test_frontend PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
  kgv_project_warnings(test_frontend)
  kgv_sanitizers(test_frontend)
  add_test(NAME frontend_native COMMAND test_frontend)

  if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT KGV_ENABLE_SANITIZERS)
    find_package(Threads REQUIRED)
    add_executable(frontend_benchmark benchmarks/frontend_benchmark.cpp)
    target_link_libraries(frontend_benchmark PRIVATE
      kilix_voicegen_frontend Threads::Threads)
    target_include_directories(frontend_benchmark PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/include
      ${CMAKE_CURRENT_SOURCE_DIR}/src)
    kgv_project_warnings(frontend_benchmark)
    add_test(NAME frontend_performance COMMAND frontend_benchmark)
    set_tests_properties(frontend_performance PROPERTIES LABELS benchmark)
  endif()
endif()

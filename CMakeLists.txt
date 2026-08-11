cmake_minimum_required(VERSION 3.24)
project(polyphorm CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# Dawn, pinned. DAWN_FETCH_DEPENDENCIES makes Dawn pull its own deps
# (incl. GLFW) without depot_tools. First configure+build is heavy
# (~10-20 min); afterwards it is cached.
#
# NOTE: the brief called for GIT_TAG chromium/6800, but as of this fetch
# the dawn repo no longer publishes chromium/NNNN branches at all (only a
# single `main` branch exists; `git ls-remote --heads ... 'chromium/*'`
# returns nothing). Dawn has moved to dated release tags instead
# (vYYYYMMDD.HHMMSS). Pinned to the newest such tag found via
# `git ls-remote --tags https://github.com/google/dawn`, sorted: v20260807.193620.
#
# GIT_SUBMODULES "" : Dawn's own dependency fetch (tools/fetch_dawn_dependencies.py,
# invoked from third_party/CMakeLists.txt when DAWN_FETCH_DEPENDENCIES=ON) selectively
# populates only the third_party/ dirs actually needed, honoring the
# `gclient-condition` gates in .gitmodules. Without this, CMake's own clone step
# does a blind `git submodule update --init --recursive` first, which ignores those
# gates and also pulls in third_party/litert-lm (gated on checkout_litert_lm, normally
# off) whose git-lfs blob 404/405s against the googlesource mirror and aborts the
# populate step entirely. Skipping submodules at clone time and letting Dawn's own
# script run avoids that broken/unrelated dependency.
set(DAWN_FETCH_DEPENDENCIES ON)
set(DAWN_USE_GLFW ON)
set(DAWN_BUILD_SAMPLES OFF)
set(TINT_BUILD_TESTS OFF)
set(TINT_BUILD_CMD_TOOLS OFF)
FetchContent_Declare(
  dawn
  GIT_REPOSITORY https://github.com/google/dawn
  GIT_TAG        v20260807.193620
  GIT_SHALLOW    ON
  GIT_SUBMODULES ""
)
FetchContent_MakeAvailable(dawn)

add_executable(polyphorm
  main_m1.cpp
  cpplib/platform.cpp
  cpplib/input.cpp
  cpplib/maths.cpp
  cpplib/memory.cpp
  cpplib/random.cpp
  cpplib/logging.cpp
  cpplib/file_system.cpp
  cpplib/gpu/gpu_context.cpp
  cpplib/graphics.cpp
  cpplib/ui_stub.cpp
)
target_include_directories(polyphorm PRIVATE cpplib)
target_link_libraries(polyphorm PRIVATE webgpu_dawn webgpu_glfw glfw)
# -UNDEBUG: see the note below on the test targets — Release mode compiles
# out every load-bearing assert() (set_constant_buffer slot==0, run_compute
# valid-shader, clear format asserts, get_mesh non-indexed, ...) unless we
# re-enable them here too. Without this, contract violations in the real app
# binary become silent instead of failing loudly.
target_compile_options(polyphorm PRIVATE -UNDEBUG)

enable_testing()
# NOTE: the top-level build type in this cache is Release, which passes
# -DNDEBUG globally and silently compiles out every assert() the test
# binaries rely on for validation (discovered in Task 5 while wiring up
# graphics_tests: a deliberately-broken shader path still printed "passed").
# Fixing this by setting CMAKE_BUILD_TYPE would force a full recompile of
# Dawn's ~200 targets under the incremental-build-only constraint, so instead
# -UNDEBUG is applied narrowly to the test targets only, re-enabling assert()
# for them without touching Dawn's build flags.
add_executable(cpplib_tests
  tests/platform_input_tests.cpp
  cpplib/platform.cpp
  cpplib/input.cpp
  cpplib/maths.cpp
  cpplib/random.cpp
)
target_include_directories(cpplib_tests PRIVATE cpplib)
target_link_libraries(cpplib_tests PRIVATE glfw)
target_compile_options(cpplib_tests PRIVATE -UNDEBUG)
add_test(NAME cpplib_tests COMMAND cpplib_tests)

add_executable(file_system_tests
  tests/file_system_tests.cpp
  cpplib/file_system.cpp
)
target_compile_features(file_system_tests PRIVATE cxx_std_17)
target_compile_options(file_system_tests PRIVATE -UNDEBUG)
add_test(NAME file_system_tests COMMAND file_system_tests)

add_executable(graphics_tests
  tests/graphics_tests.cpp
  cpplib/graphics.cpp
  cpplib/file_system.cpp
  cpplib/gpu/gpu_context.cpp
  cpplib/platform.cpp
  cpplib/input.cpp
  cpplib/logging.cpp
)
target_compile_features(graphics_tests PRIVATE cxx_std_17)
target_compile_definitions(graphics_tests PRIVATE
  TEST_SHADER_DIR="${CMAKE_SOURCE_DIR}/shaders/tests")
target_include_directories(graphics_tests PRIVATE ${CMAKE_SOURCE_DIR}/cpplib)
target_link_libraries(graphics_tests PRIVATE webgpu_dawn webgpu_glfw glfw)
target_compile_options(graphics_tests PRIVATE -UNDEBUG)
add_test(NAME graphics_tests COMMAND graphics_tests)

add_executable(shader_compile_tests
  tests/shader_compile_tests.cpp
  cpplib/graphics.cpp
  cpplib/file_system.cpp
  cpplib/gpu/gpu_context.cpp
  cpplib/platform.cpp
  cpplib/input.cpp
  cpplib/logging.cpp
)
target_compile_features(shader_compile_tests PRIVATE cxx_std_17)
target_compile_definitions(shader_compile_tests PRIVATE
  SHADER_DIR="${CMAKE_SOURCE_DIR}/shaders")
target_include_directories(shader_compile_tests PRIVATE ${CMAKE_SOURCE_DIR}/cpplib)
target_link_libraries(shader_compile_tests PRIVATE webgpu_dawn webgpu_glfw glfw)
target_compile_options(shader_compile_tests PRIVATE -UNDEBUG)
add_test(NAME shader_compile_tests COMMAND shader_compile_tests)

add_executable(sim_kernel_tests
  tests/sim_kernel_tests.cpp
  cpplib/graphics.cpp
  cpplib/file_system.cpp
  cpplib/gpu/gpu_context.cpp
  cpplib/platform.cpp
  cpplib/input.cpp
  cpplib/logging.cpp
)
target_compile_features(sim_kernel_tests PRIVATE cxx_std_17)
target_compile_definitions(sim_kernel_tests PRIVATE
  SHADER_DIR="${CMAKE_SOURCE_DIR}/shaders")
target_include_directories(sim_kernel_tests PRIVATE ${CMAKE_SOURCE_DIR}/cpplib)
target_link_libraries(sim_kernel_tests PRIVATE webgpu_dawn webgpu_glfw glfw)
target_compile_options(sim_kernel_tests PRIVATE -UNDEBUG)
add_test(NAME sim_kernel_tests COMMAND sim_kernel_tests)

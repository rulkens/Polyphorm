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
  # cpplib/memory.cpp         # enabled in M2
  # cpplib/random.cpp         # enabled in M2
  # cpplib/logging.cpp        # enabled in M2
  cpplib/file_system.cpp
  cpplib/gpu/gpu_context.cpp
)
target_include_directories(polyphorm PRIVATE cpplib)
target_link_libraries(polyphorm PRIVATE webgpu_dawn webgpu_glfw glfw)

enable_testing()
add_executable(cpplib_tests
  tests/platform_input_tests.cpp
  cpplib/platform.cpp
  cpplib/input.cpp
  cpplib/maths.cpp
)
target_include_directories(cpplib_tests PRIVATE cpplib)
target_link_libraries(cpplib_tests PRIVATE glfw)
add_test(NAME cpplib_tests COMMAND cpplib_tests)

add_executable(file_system_tests
  tests/file_system_tests.cpp
  cpplib/file_system.cpp
)
target_compile_features(file_system_tests PRIVATE cxx_std_17)
add_test(NAME file_system_tests COMMAND file_system_tests)

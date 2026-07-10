cmake_minimum_required(VERSION 3.20)
project(dpor_model_checker LANGUAGES CXX)

# The verification gates are exhaustive sweeps; unoptimized builds make them
# ~10x slower. Default to Release when the user does not choose.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(model_checker
    src/model/action.cpp
    src/model/vector_clock.cpp
    src/model/checker.cpp
)
target_include_directories(model_checker PUBLIC include)
target_compile_options(model_checker PRIVATE -Wall -Wextra -Wpedantic)

add_library(dpor_cli_lib
    src/cli/program_parser.cpp
    src/cli/report.cpp
)
target_include_directories(dpor_cli_lib PUBLIC src/cli)
target_link_libraries(dpor_cli_lib PUBLIC model_checker)
target_compile_options(dpor_cli_lib PRIVATE -Wall -Wextra -Wpedantic)

add_executable(dpor
    src/cli/main.cpp
)
target_link_libraries(dpor PRIVATE dpor_cli_lib)
target_compile_options(dpor PRIVATE -Wall -Wextra -Wpedantic)

add_executable(dpor_smoke tests/smoke.cpp)
target_link_libraries(dpor_smoke PRIVATE model_checker)

add_executable(dpor_phase1 tests/phase1.cpp)
target_link_libraries(dpor_phase1 PRIVATE model_checker)

add_executable(vector_clock_tests tests/vector_clock_tests.cpp)
target_link_libraries(vector_clock_tests PRIVATE model_checker)

add_executable(independence_tests tests/independence.cpp)
target_link_libraries(independence_tests PRIVATE model_checker)

add_executable(dpor_oracle tests/dpor_oracle.cpp)
target_link_libraries(dpor_oracle PRIVATE model_checker)

add_executable(dpor_optimality tests/dpor_optimality.cpp)
target_link_libraries(dpor_optimality PRIVATE model_checker)

add_executable(dpor_oracle_3threads tests/dpor_oracle_3threads.cpp)
target_link_libraries(dpor_oracle_3threads PRIVATE model_checker)

add_executable(minimize_tests tests/minimize.cpp)
target_link_libraries(minimize_tests PRIVATE model_checker)

add_executable(condvar_tests tests/condvar.cpp)
target_link_libraries(condvar_tests PRIVATE model_checker)

add_executable(hb_edge_probes tests/hb_edge_probes.cpp)
target_link_libraries(hb_edge_probes PRIVATE model_checker)

add_executable(atomics_tests tests/atomics.cpp)
target_link_libraries(atomics_tests PRIVATE model_checker)

add_executable(values_tests tests/values.cpp)
target_link_libraries(values_tests PRIVATE model_checker)

add_executable(tso_litmus tests/tso_litmus.cpp)
target_link_libraries(tso_litmus PRIVATE model_checker)

add_executable(pso_litmus tests/pso_litmus.cpp)
target_link_libraries(pso_litmus PRIVATE model_checker)

add_executable(tso_oracle tests/tso_oracle.cpp)
target_link_libraries(tso_oracle PRIVATE model_checker)

add_executable(pso_oracle tests/pso_oracle.cpp)
target_link_libraries(pso_oracle PRIVATE model_checker)

add_executable(model_inclusion tests/model_inclusion.cpp)
target_link_libraries(model_inclusion PRIVATE model_checker)

add_executable(nontermination_tests tests/nontermination.cpp)
target_link_libraries(nontermination_tests PRIVATE dpor_cli_lib)

add_executable(spawn_tests tests/spawn.cpp)
target_link_libraries(spawn_tests PRIVATE dpor_cli_lib)

add_executable(cli_tests tests/cli.cpp)
target_compile_options(cli_tests PRIVATE -Wall -Wextra -Wpedantic)

add_executable(classic_gallery_tests tests/classic_gallery.cpp)
target_link_libraries(classic_gallery_tests PRIVATE dpor_cli_lib)
target_compile_options(classic_gallery_tests PRIVATE -Wall -Wextra -Wpedantic)

add_executable(dpor_fuzz_differential
    tests/fuzz_differential.cpp
)
target_link_libraries(dpor_fuzz_differential PRIVATE dpor_cli_lib)
target_compile_options(dpor_fuzz_differential PRIVATE -Wall -Wextra -Wpedantic)

include(CTest)
add_test(NAME dpor_smoke COMMAND dpor_smoke)
add_test(NAME dpor_phase1 COMMAND dpor_phase1)
add_test(NAME vector_clock_tests COMMAND vector_clock_tests)
add_test(NAME independence_tests COMMAND independence_tests)
add_test(NAME dpor_oracle COMMAND dpor_oracle)
add_test(NAME dpor_optimality COMMAND dpor_optimality)
add_test(NAME dpor_oracle_3threads COMMAND dpor_oracle_3threads)
add_test(NAME minimize_tests COMMAND minimize_tests)
add_test(NAME condvar_tests COMMAND condvar_tests)
add_test(NAME hb_edge_probes COMMAND hb_edge_probes)
add_test(NAME atomics_tests COMMAND atomics_tests)
add_test(NAME values_tests COMMAND values_tests)
add_test(NAME tso_litmus COMMAND tso_litmus)
add_test(NAME pso_litmus COMMAND pso_litmus)
add_test(NAME tso_oracle COMMAND tso_oracle)
add_test(NAME pso_oracle COMMAND pso_oracle)
add_test(NAME model_inclusion COMMAND model_inclusion)
add_test(NAME nontermination_tests COMMAND nontermination_tests)
add_test(NAME spawn_tests COMMAND spawn_tests)
add_test(NAME cli_tests COMMAND cli_tests $<TARGET_FILE:dpor> ${CMAKE_SOURCE_DIR})
add_test(NAME classic_gallery_tests COMMAND classic_gallery_tests $<TARGET_FILE:dpor> ${CMAKE_SOURCE_DIR})
add_test(NAME dpor_fuzz_differential COMMAND dpor_fuzz_differential)

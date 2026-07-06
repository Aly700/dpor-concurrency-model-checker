cmake_minimum_required(VERSION 3.20)
project(dpor_model_checker LANGUAGES CXX)

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

add_executable(dpor_oracle_3threads tests/dpor_oracle_3threads.cpp)
target_link_libraries(dpor_oracle_3threads PRIVATE model_checker)

include(CTest)
add_test(NAME dpor_smoke COMMAND dpor_smoke)
add_test(NAME dpor_phase1 COMMAND dpor_phase1)
add_test(NAME vector_clock_tests COMMAND vector_clock_tests)
add_test(NAME independence_tests COMMAND independence_tests)
add_test(NAME dpor_oracle COMMAND dpor_oracle)
add_test(NAME dpor_oracle_3threads COMMAND dpor_oracle_3threads)

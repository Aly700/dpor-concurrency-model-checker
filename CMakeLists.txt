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

include(CTest)
add_test(NAME dpor_smoke COMMAND dpor_smoke)

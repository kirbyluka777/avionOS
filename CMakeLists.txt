cmake_minimum_required(VERSION 3.20)
project(avionOS LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

find_package(Threads REQUIRED)

add_executable(avionOS src/main.cpp)
target_include_directories(avionOS PRIVATE include)
target_link_libraries(avionOS PRIVATE Threads::Threads)
set_target_properties(avionOS PROPERTIES OUTPUT_NAME "avionOS")

add_executable(tests tests/self_test.cpp)
target_include_directories(tests PRIVATE include)
target_link_libraries(tests PRIVATE Threads::Threads)
set_target_properties(tests PROPERTIES OUTPUT_NAME "tests")

enable_testing()
add_test(NAME avionOS_self_test COMMAND tests)
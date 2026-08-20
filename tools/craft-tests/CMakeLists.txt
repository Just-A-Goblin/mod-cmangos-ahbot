# Optional, self-contained offline test target for the craft-layer cost engine
# (crafting addendum §10.2). NOT wired into the AzerothCore build — the cost engine
# has no core dependencies, so this stands alone. Build it directly:
#
#   cmake -S . -B build && cmake --build build && ./build/craft-tests
#
# or without CMake at all:
#
#   g++ -std=c++17 -I../../src test_craft.cpp ../../src/CMangosAHBotCost.cpp -o craft-tests
#
cmake_minimum_required(VERSION 3.16)
project(cmangos_ahbot_craft_tests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(craft-tests
    test_craft.cpp
    ../../src/CMangosAHBotCost.cpp
    ../../src/CMangosAHBotCraft.cpp)
target_include_directories(craft-tests PRIVATE ../../src)

enable_testing()
add_test(NAME craft-tests COMMAND craft-tests)

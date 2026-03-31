cmake_minimum_required(VERSION 3.16)
project(CephPGStateMachine CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Boost (statechart + mpl + system)
find_package(Boost REQUIRED COMPONENTS system)

add_executable(pg_sm main.cpp)

target_include_directories(pg_sm PRIVATE ${Boost_INCLUDE_DIRS})
target_link_libraries(pg_sm PRIVATE ${Boost_LIBRARIES})

# Clang/GCC warnings
target_compile_options(pg_sm PRIVATE
    -Wall -Wextra -Wpedantic
    -Wno-unused-parameter
)

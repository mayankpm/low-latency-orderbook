cmake_minimum_required(VERSION 3.20)
project(low_latency_orderbook LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(OB_NATIVE "Optimize for the build machine's CPU (-march=native)" OFF)
set(OB_SANITIZE "" CACHE STRING "Sanitizer to build with: thread, address, undefined")

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
  set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
  if(OB_NATIVE)
    add_compile_options(-march=native)
  endif()
  if(OB_SANITIZE)
    add_compile_options(-fsanitize=${OB_SANITIZE} -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=${OB_SANITIZE})
  endif()
endif()
if(MINGW)
  add_link_options(-static)
endif()

find_package(Threads REQUIRED)

# The engine is header-only so the matching loop and the event listener
# inline into the caller.
add_library(orderbook INTERFACE)
target_include_directories(orderbook INTERFACE include)
target_link_libraries(orderbook INTERFACE Threads::Threads)

add_executable(obbench apps/obbench.cpp)
target_link_libraries(obbench PRIVATE orderbook)

add_executable(obtests
  tests/main.cpp
  tests/structures_test.cpp
  tests/book_test.cpp
  tests/differential_test.cpp
)
target_link_libraries(obtests PRIVATE orderbook)

enable_testing()
add_test(NAME obtests COMMAND obtests)

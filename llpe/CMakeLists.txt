cmake_minimum_required(VERSION 3.6)
cmake_policy(SET CMP0074 NEW)

find_package(LLVM REQUIRED CONFIG)

add_definitions("-std=c++11")

# RTTI-ness must match the LLVM we're building against.
if(NOT ${LLVM_ENABLE_RTTI})
  add_definitions("-fno-rtti")
endif()

add_definitions(${LLVM_DEFINITIONS})

include_directories(${LLVM_INCLUDE_DIRS} include)

add_subdirectory(main)
add_subdirectory(driver)
add_subdirectory(utils)

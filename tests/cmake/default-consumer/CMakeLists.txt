cmake_minimum_required(VERSION 3.24)
project(plugin-sdk-default-consumer LANGUAGES CXX)

if(NOT ONELASTLEAF_PLUGIN_SDK_SOURCE_DIR)
  message(FATAL_ERROR "ONELASTLEAF_PLUGIN_SDK_SOURCE_DIR is required")
endif()

add_subdirectory("${ONELASTLEAF_PLUGIN_SDK_SOURCE_DIR}" plugin-sdk)

foreach(SDK_TEST_TARGET IN ITEMS
  plugin-sdk-conformance
  plugin-sdk-test
  plugin-sdk-internal-test
)
  if(TARGET "${SDK_TEST_TARGET}")
    message(FATAL_ERROR "${SDK_TEST_TARGET} must not be created by default")
  endif()
endforeach()

# CTest reserves this name when a dependency enables testing. Consumers must be
# free to use it for their own plugin executable.
add_executable(test main.cpp)
target_link_libraries(test PRIVATE onelastleaf::plugin_sdk)

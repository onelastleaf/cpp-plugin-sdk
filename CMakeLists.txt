cmake_minimum_required(VERSION 3.24)
project(onelastleaf-plugin-sdk VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)
find_package(PkgConfig REQUIRED)
find_package(OpenSSL REQUIRED)
pkg_check_modules(GRPC REQUIRED IMPORTED_TARGET grpc++)
pkg_check_modules(PROTOBUF REQUIRED IMPORTED_TARGET protobuf)
find_program(PROTOC_EXECUTABLE protoc REQUIRED)
find_program(GRPC_CPP_PLUGIN grpc_cpp_plugin REQUIRED)

set(PROTO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/proto")
set(GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${GENERATED_DIR}/oll")
set(PROTO_FILES common config document plugin)
set(PROTO_INPUTS)
set(GENERATED_SOURCES)
set(GENERATED_HEADERS)
foreach(PROTO IN LISTS PROTO_FILES)
  list(APPEND PROTO_INPUTS "${PROTO_ROOT}/oll/${PROTO}.proto")
  list(APPEND GENERATED_SOURCES "${GENERATED_DIR}/oll/${PROTO}.pb.cc")
  list(APPEND GENERATED_HEADERS "${GENERATED_DIR}/oll/${PROTO}.pb.h")
endforeach()
list(APPEND GENERATED_SOURCES "${GENERATED_DIR}/oll/plugin.grpc.pb.cc")
list(APPEND GENERATED_HEADERS "${GENERATED_DIR}/oll/plugin.grpc.pb.h")

add_custom_command(
  OUTPUT ${GENERATED_SOURCES} ${GENERATED_HEADERS}
  COMMAND "${PROTOC_EXECUTABLE}"
    --proto_path ${PROTO_ROOT}
    --cpp_out ${GENERATED_DIR}
    --grpc_out ${GENERATED_DIR}
    --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN}
    oll/common.proto oll/config.proto oll/document.proto oll/plugin.proto
  DEPENDS ${PROTO_INPUTS}
  WORKING_DIRECTORY "${PROTO_ROOT}"
  VERBATIM
)

add_library(onelastleaf-plugin-sdk
  src/artifact.cpp
  src/host.cpp
  src/job_registry.cpp
  src/parent_liveness.cpp
  src/plugin.cpp
  src/session.cpp
  src/validation.cpp
  ${GENERATED_SOURCES}
)
add_library(onelastleaf::plugin_sdk ALIAS onelastleaf-plugin-sdk)
set_target_properties(onelastleaf-plugin-sdk PROPERTIES EXPORT_NAME plugin_sdk)

target_compile_features(onelastleaf-plugin-sdk PUBLIC cxx_std_20)
target_include_directories(onelastleaf-plugin-sdk
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${GENERATED_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(onelastleaf-plugin-sdk PUBLIC PkgConfig::GRPC PkgConfig::PROTOBUF OpenSSL::Crypto)

include(CMakePackageConfigHelpers)
configure_package_config_file(
  cmake/onelastleaf-plugin-sdk-config.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/onelastleaf-plugin-sdk-config.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/onelastleaf-plugin-sdk"
)
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/onelastleaf-plugin-sdk-config-version.cmake"
  VERSION "${PROJECT_VERSION}"
  COMPATIBILITY SameMajorVersion
)
install(TARGETS onelastleaf-plugin-sdk EXPORT onelastleaf-plugin-sdk-targets)
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(FILES ${GENERATED_HEADERS} DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/oll")
install(EXPORT onelastleaf-plugin-sdk-targets
  FILE onelastleaf-plugin-sdk-targets.cmake
  NAMESPACE onelastleaf::
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/onelastleaf-plugin-sdk"
)
install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/onelastleaf-plugin-sdk-config.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/onelastleaf-plugin-sdk-config-version.cmake"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/onelastleaf-plugin-sdk"
)

option(
  ONELASTLEAF_PLUGIN_SDK_BUILD_TESTS
  "Build the onelastleaf C++ plugin SDK tests"
  OFF
)
if(ONELASTLEAF_PLUGIN_SDK_BUILD_TESTS)
  enable_testing()

  add_executable(
    plugin-sdk-conformance
    EXCLUDE_FROM_ALL
    tests/fixtures/conformance/main.cpp
  )
  target_link_libraries(plugin-sdk-conformance PRIVATE onelastleaf::plugin_sdk)

  add_executable(plugin-sdk-test tests/plugin_test.cpp)
  target_link_libraries(plugin-sdk-test PRIVATE onelastleaf::plugin_sdk)
  add_test(NAME plugin-sdk-test COMMAND plugin-sdk-test)

  add_executable(plugin-sdk-internal-test tests/internal_test.cpp)
  target_include_directories(plugin-sdk-internal-test PRIVATE src)
  target_link_libraries(plugin-sdk-internal-test PRIVATE onelastleaf::plugin_sdk)
  add_test(NAME plugin-sdk-internal-test COMMAND plugin-sdk-internal-test)

  add_test(
    NAME plugin-sdk-cmake-defaults
    COMMAND "${CMAKE_COMMAND}"
      -S "${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/default-consumer"
      -B "${CMAKE_CURRENT_BINARY_DIR}/tests/default-consumer"
      -DONELASTLEAF_PLUGIN_SDK_SOURCE_DIR:PATH=${CMAKE_CURRENT_SOURCE_DIR}
  )
endif()

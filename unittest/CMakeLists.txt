option(BUILD_LEGACY_INTEGRATION_TESTS
  "Build the old network-heavy tests and their optional Poco test server"
  OFF
)

if(NOT TARGET GTest::gtest_main)
  find_package(GTest CONFIG QUIET)
  if(NOT GTest_FOUND)
    find_package(GTest REQUIRED)
  endif()
endif()

set(COS_TEST_INCLUDE_DIRS
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${PROJECT_SOURCE_DIR}/include"
)

function(add_cos_test target_name)
  add_executable(${target_name} ${ARGN})
  target_compile_features(${target_name} PRIVATE cxx_std_17)
  if(MSVC)
    target_compile_options(${target_name} PRIVATE /utf-8)
  endif()
  target_include_directories(${target_name} PRIVATE ${COS_TEST_INCLUDE_DIRS})
  target_link_libraries(${target_name} PRIVATE cos::cossdk GTest::gtest_main)
  add_test(NAME ${target_name} COMMAND ${target_name})
endfunction()

# Fast tests that exercise request/response serialization and utility code do
# not need the legacy test server or its dependency graph.
add_cos_test(cos-cpp-sdk-unit-tests
  src/object_request_test.cpp
  src/object_response_test.cpp
  src/bucket_response_test.cpp
  src/util_test.cpp
  src/auditing_req_test.cpp
)

if(BUILD_LEGACY_INTEGRATION_TESTS)
  find_package(Poco CONFIG REQUIRED
    COMPONENTS Foundation Util XML JSON Net NetSSL Crypto
  )

  set(COS_LEGACY_TEST_SOURCES
    src/test_utils.cpp
    src/object_op_test_common.cpp
    src/object_basic_op_test.cpp
    src/object_multipart_test.cpp
    src/object_copy_test.cpp
    src/object_media_test.cpp
    src/object_auditing_test.cpp
    src/object_misc_test.cpp
    src/object_op_retry_test.cpp
    src/bucket_op_test.cpp
    src/live_channel_test.cpp
    src/async_op_test.cpp
    src/resumable_upload_test.cpp
  )
  add_cos_test(all-test ${COS_LEGACY_TEST_SOURCES})
  target_link_libraries(all-test PRIVATE
    Poco::NetSSL
    Poco::Net
    Poco::Crypto
    Poco::Util
    Poco::JSON
    Poco::XML
    Poco::Foundation
  )

  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/conf/config.json")
    add_custom_command(TARGET all-test POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/conf/config.json"
        "$<TARGET_FILE_DIR:all-test>/config.json"
    )
  endif()
endif()

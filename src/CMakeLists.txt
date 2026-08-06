file(GLOB_RECURSE COS_SDK_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)
file(GLOB_RECURSE COS_SDK_PUBLIC_HEADERS CONFIGURE_DEPENDS
  "${PROJECT_SOURCE_DIR}/include/*.h"
)

function(configure_cos_target target_name target_type)
  add_library(${target_name} ${target_type}
    ${COS_SDK_SOURCES}
    ${COS_SDK_PUBLIC_HEADERS}
  )

  target_compile_features(${target_name} PUBLIC cxx_std_17)
  if(MSVC)
    target_compile_options(${target_name} PRIVATE /utf-8)
  endif()
  target_include_directories(${target_name}
    PUBLIC
      $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
      $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}
  )
  target_link_libraries(${target_name}
    PRIVATE
      $<BUILD_INTERFACE:CURL::libcurl>
      $<BUILD_INTERFACE:OpenSSL::SSL>
      $<BUILD_INTERFACE:OpenSSL::Crypto>
      Threads::Threads
  )

  if(WIN32)
    target_link_libraries(${target_name} PRIVATE ws2_32)
  endif()

  if(ENABLE_COVERAGE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${target_name} PRIVATE --coverage)
    target_link_options(${target_name} PRIVATE --coverage)
  endif()
endfunction()

configure_cos_target(cossdk STATIC)
add_library(cos::cossdk ALIAS cossdk)
set_target_properties(cossdk PROPERTIES OUTPUT_NAME cossdk)

if(BUILD_SHARED_LIB)
  configure_cos_target(cossdk-shared SHARED)
  add_library(cos::cossdk-shared ALIAS cossdk-shared)
  set_target_properties(cossdk-shared PROPERTIES
    OUTPUT_NAME cossdk-shared
    WINDOWS_EXPORT_ALL_SYMBOLS ON
  )
endif()

install(
  TARGETS cossdk
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
if(BUILD_SHARED_LIB)
  install(
    TARGETS cossdk-shared
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
  )
endif()

file(GLOB_RECURSE COS_DEMO_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)

set(COS_DEMO_OUTPUT_DIR "${CMAKE_BINARY_DIR}/bin")
foreach(source_file IN LISTS COS_DEMO_SOURCES)
  get_filename_component(demo_name "${source_file}" NAME_WE)
  add_executable(${demo_name} "${source_file}")
  if(MSVC)
    target_compile_options(${demo_name} PRIVATE /utf-8)
  endif()
  target_link_libraries(${demo_name} PRIVATE cos::cossdk)
  if(demo_name STREQUAL "get_object_demo")
    target_link_libraries(${demo_name} PRIVATE OpenSSL::SSL)
  endif()
  set_target_properties(${demo_name} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${COS_DEMO_OUTPUT_DIR}"
  )
endforeach()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/config.json")
  file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/config.json"
       DESTINATION "${COS_DEMO_OUTPUT_DIR}")
endif()

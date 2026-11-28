set(TESTS
  Allocator
  ArgumentParser
  ExtendedVolatileMetadata
  fextl_function
  FileMappingBaseAddress
  Filesystem
  InterruptableConditionVariable
  PatchableDataMasks
  SecureExecution
  StringUtils
  WildcardMatcher)

list(APPEND LIBS Common FEXCore FEXCore_Base JemallocLibs)

foreach(API_TEST ${TESTS})
  add_executable(${API_TEST} ${API_TEST}.cpp)
  target_link_libraries(${API_TEST} PRIVATE ${LIBS} Catch2::Catch2WithMain)

  catch_discover_tests(${API_TEST}
    TEST_SUFFIX ".${API_TEST}.APITest")
endforeach()

target_include_directories(PatchableDataMasks PRIVATE "${CMAKE_SOURCE_DIR}/FEXCore/Source/")
if (ENABLE_VIXL_DISASSEMBLER)
  target_compile_definitions(PatchableDataMasks PRIVATE VIXL_DISASSEMBLER=1)
endif()
if (ENABLE_VIXL_DISASSEMBLER OR ENABLE_VIXL_SIMULATOR)
  target_link_libraries(PatchableDataMasks PRIVATE vixl::vixl)
endif()

add_custom_target(api_tests
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
  USES_TERMINAL
  COMMAND "ctest" "--output-on-failure" "--timeout" "302" ${TEST_JOB_FLAG} "-R" "\.*.APITest")

foreach(API_TEST ${TESTS})
  add_dependencies(api_tests ${API_TEST})
endforeach()

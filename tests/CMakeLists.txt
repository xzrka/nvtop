# The BAR0 junction and memory temperature reader is tested by standing a device tree
# with the shape of sysfs, so it needs neither a GPU nor a test framework of its own.
if (BUILD_TESTING)
  add_executable(nvidia_vram_temps_test
    nvidia_vram_temps_test.c
    ${PROJECT_SOURCE_DIR}/src/nvidia_vram_temps.c
  )
  target_include_directories(nvidia_vram_temps_test PRIVATE
    ${PROJECT_SOURCE_DIR}/include)
  add_test(NAME nvidia_vram_temps COMMAND nvidia_vram_temps_test)
endif()

find_package(GTest)
if (BUILD_TESTING AND GTest_FOUND)

  option(THOROUGH_TESTING "Enable extensive testing (takes hours, e.g., use once per release)" OFF)

  # Create a library for testing
  add_library(testLib
    ${PROJECT_SOURCE_DIR}/src/interface_layout_selection.c
    ${PROJECT_SOURCE_DIR}/src/extract_processinfo_fdinfo.c
    ${PROJECT_SOURCE_DIR}/src/interface_options.c
    ${PROJECT_SOURCE_DIR}/src/pcie_utilization.c
    ${PROJECT_SOURCE_DIR}/src/ini.c
  )
  target_include_directories(testLib PUBLIC
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_BINARY_DIR}/include)

  # Tests
  add_executable(
    interfaceTests
    interfaceTests.cpp
  )
  target_link_libraries(interfaceTests PRIVATE testLib GTest::gtest_main)
  gtest_discover_tests(interfaceTests)

  if (THOROUGH_TESTING)
    target_compile_definitions(interfaceTests PRIVATE THOROUGH_TESTING)
  endif()


endif()

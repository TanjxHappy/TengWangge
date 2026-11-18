tw_add_tool(param_check_high SOURCES
    check_high_tool.cpp
    check_high_tool.h
    check_high_page.cpp
    check_high_page.h
    structure_model.cpp
    structure_model.h
    notation.cpp
    notation.h
    byte_store.cpp
    byte_store.h
)

find_package(Qt5 5.15 REQUIRED COMPONENTS Concurrent)
target_link_libraries(tw_tool_param_check_high PUBLIC Qt5::Concurrent)

if(TENGWANGGE_BUILD_TESTS)
    enable_testing()
    find_package(Qt5 5.15 REQUIRED COMPONENTS Test)
    add_executable(tengwangge_param_check_high_tests check_high_tests.cpp)
    target_link_libraries(tengwangge_param_check_high_tests PRIVATE tw_tool_param_check_high Qt5::Test)
    tw_apply_warnings(tengwangge_param_check_high_tests)
    add_test(NAME param_check_high_tests COMMAND tengwangge_param_check_high_tests)
    set_tests_properties(param_check_high_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        TIMEOUT 180
    )
    # 顶层 enable_testing() 写在 add_subdirectory(src/tools) 之后，本目录自己的
    # CTestTestfile 不会出现在 ctest 的子目录链上。把它挂到源码根目录，ctest 才能看到。
    set_property(DIRECTORY "${CMAKE_SOURCE_DIR}" APPEND PROPERTY
        TEST_INCLUDE_FILES "${CMAKE_CURRENT_BINARY_DIR}/CTestTestfile.cmake")
endif()

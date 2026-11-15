tw_add_tool(param_check SOURCES
    param_check_tool.cpp
    param_check_tool.h
    param_editor_page.cpp
    param_editor_page.h
    param_tree_model.cpp
    param_tree_model.h
    param_codec.cpp
    param_document.h
    param_io.cpp
    param_io.h
)

find_package(Qt5 5.15 REQUIRED COMPONENTS Xml)
target_link_libraries(tw_tool_param_check PRIVATE Qt5::Xml)

if(TENGWANGGE_BUILD_TESTS)
    enable_testing()
    find_package(Qt5 5.15 REQUIRED COMPONENTS Test)
    add_executable(tengwangge_param_check_tests param_check_tests.cpp)
    target_link_libraries(tengwangge_param_check_tests PRIVATE tw_tool_param_check Qt5::Test)
    tw_apply_warnings(tengwangge_param_check_tests)
    add_test(NAME param_check_tests COMMAND tengwangge_param_check_tests)
    set_tests_properties(param_check_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        TIMEOUT 120
    )
    # 顶层 enable_testing() 写在 add_subdirectory(src/tools) 之后，本目录自己的
    # CTestTestfile 不会出现在 ctest 的子目录链上。把它挂到源码根目录，ctest 才能看到。
    set_property(DIRECTORY "${CMAKE_SOURCE_DIR}" APPEND PROPERTY
        TEST_INCLUDE_FILES "${CMAKE_CURRENT_BINARY_DIR}/CTestTestfile.cmake")
endif()

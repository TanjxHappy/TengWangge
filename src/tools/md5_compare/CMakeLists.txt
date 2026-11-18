tw_add_tool(md5_compare SOURCES
    compare_engine.cpp
    compare_engine.h
    md5_compare_tool.cpp
    md5_compare_tool.h
)

# 测试可执行文件属于本工具。根目录的 enable_testing() 在 src/ 之后才执行，
# 所以用 DEFER（或旧版 CMake 的 TEST_INCLUDE_FILES）把用例登记到顶层。
if(TENGWANGGE_BUILD_TESTS)
    find_package(Qt5 5.15 REQUIRED COMPONENTS Test)
    add_executable(tengwangge_md5_compare_tests md5_compare_test.cpp)
    target_link_libraries(tengwangge_md5_compare_tests PRIVATE tw_tool_md5_compare Qt5::Test)
    tw_apply_warnings(tengwangge_md5_compare_tests)

    if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.19")
        cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}"
            CALL add_test NAME md5_compare_tests COMMAND tengwangge_md5_compare_tests)
        cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}"
            CALL set_tests_properties md5_compare_tests PROPERTIES
                ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
                TIMEOUT 120)
    else()
        set(_tw_md5_ctest "${CMAKE_CURRENT_BINARY_DIR}/md5_compare_ctest.cmake")
        file(GENERATE OUTPUT "${_tw_md5_ctest}" CONTENT
"add_test([=[md5_compare_tests]=] \"$<TARGET_FILE:tengwangge_md5_compare_tests>\")
set_tests_properties([=[md5_compare_tests]=] PROPERTIES ENVIRONMENT \"QT_QPA_PLATFORM=offscreen\" TIMEOUT 120)
")
        set_property(DIRECTORY "${CMAKE_SOURCE_DIR}" APPEND PROPERTY TEST_INCLUDE_FILES "${_tw_md5_ctest}")
    endif()
endif()

tw_add_tool(filter_unique SOURCES
    filter_unique_scan.cpp
    filter_unique_scan.h
    filter_unique_tool.cpp
    filter_unique_tool.h
)

# 测试源码放在本目录。顶层 enable_testing() 在 src/ 之后才执行，子目录里的
# add_test() 不会进入 ctest，所以在配置阶段把测试挂到源码根的 TEST_INCLUDE_FILES。
if(TENGWANGGE_BUILD_TESTS)
    find_package(Qt5 5.15 REQUIRED COMPONENTS Test)
    add_executable(tengwangge_filter_unique_tests filter_unique_test.cpp)
    target_link_libraries(tengwangge_filter_unique_tests PRIVATE tw_tool_filter_unique Qt5::Test)
    tw_apply_warnings(tengwangge_filter_unique_tests)

    file(GENERATE
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/register_filter_unique_tests.cmake"
        CONTENT "add_test(filter_unique_tests \"$<TARGET_FILE:tengwangge_filter_unique_tests>\")
set_tests_properties(filter_unique_tests PROPERTIES ENVIRONMENT \"QT_QPA_PLATFORM=offscreen\" TIMEOUT \"120\")
")
    set_property(DIRECTORY "${CMAKE_SOURCE_DIR}" APPEND PROPERTY TEST_INCLUDE_FILES
        "${CMAKE_CURRENT_BINARY_DIR}/register_filter_unique_tests.cmake")
endif()

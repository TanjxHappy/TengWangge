tw_add_tool(filter_unique_high SOURCES
    term_list.cpp
    term_list.h
    bounded_tree.cpp
    bounded_tree.h
    high_filter_page.cpp
    high_filter_page.h
    high_filter_tool.cpp
    high_filter_tool.h
)

# 顶层 enable_testing() 在 src/ 之后才执行，子目录里的 add_test()
# 进不了 ctest，所以把注册脚本挂到源码根的 TEST_INCLUDE_FILES。
if(TENGWANGGE_BUILD_TESTS)
    find_package(Qt5 5.15 REQUIRED COMPONENTS Test)
    add_executable(tengwangge_filter_unique_high_tests high_filter_test.cpp)
    target_link_libraries(tengwangge_filter_unique_high_tests
        PRIVATE tw_tool_filter_unique_high Qt5::Test)
    tw_apply_warnings(tengwangge_filter_unique_high_tests)

    file(GENERATE
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/register_filter_unique_high_tests.cmake"
        CONTENT "add_test(filter_unique_high_tests \"$<TARGET_FILE:tengwangge_filter_unique_high_tests>\")
set_tests_properties(filter_unique_high_tests PROPERTIES ENVIRONMENT \"QT_QPA_PLATFORM=offscreen\" TIMEOUT \"180\")
")
    set_property(DIRECTORY "${CMAKE_SOURCE_DIR}" APPEND PROPERTY TEST_INCLUDE_FILES
        "${CMAKE_CURRENT_BINARY_DIR}/register_filter_unique_high_tests.cmake")
endif()

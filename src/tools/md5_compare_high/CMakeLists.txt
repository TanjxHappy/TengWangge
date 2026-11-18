tw_add_tool(md5_compare_high SOURCES
    high_digest.cpp
    high_digest.h
    md5_compare_high_tool.cpp
    md5_compare_high_tool.h
)

# enable_testing() 在 src/ 之后才执行，所以把 ctest 登记推迟到顶层目录结束。
if(TENGWANGGE_BUILD_TESTS)
    find_package(Qt5 5.15 REQUIRED COMPONENTS Test)
    add_executable(tengwangge_md5_compare_high_tests md5_compare_high_test.cpp)
    target_link_libraries(tengwangge_md5_compare_high_tests PRIVATE tw_tool_md5_compare_high Qt5::Test)
    tw_apply_warnings(tengwangge_md5_compare_high_tests)

    if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.19")
        cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}"
            CALL add_test NAME md5_compare_high_tests COMMAND tengwangge_md5_compare_high_tests)
        cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}"
            CALL set_tests_properties md5_compare_high_tests PROPERTIES
                ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
                TIMEOUT 120)
    else()
        set(_tw_md5_high_ctest "${CMAKE_CURRENT_BINARY_DIR}/md5_compare_high_ctest.cmake")
        file(GENERATE OUTPUT "${_tw_md5_high_ctest}" CONTENT
"add_test([=[md5_compare_high_tests]=] \"$<TARGET_FILE:tengwangge_md5_compare_high_tests>\")
set_tests_properties([=[md5_compare_high_tests]=] PROPERTIES ENVIRONMENT \"QT_QPA_PLATFORM=offscreen\" TIMEOUT 120)
")
        set_property(DIRECTORY "${CMAKE_SOURCE_DIR}" APPEND PROPERTY TEST_INCLUDE_FILES "${_tw_md5_high_ctest}")
    endif()
endif()

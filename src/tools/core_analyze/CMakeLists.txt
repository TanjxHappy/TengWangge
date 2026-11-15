tw_add_tool(core_analyze SOURCES
    core_analyze_tool.cpp
    core_analyze_tool.h
    core_analyzer.cpp
    core_analyzer.h
    command_runner.cpp
    command_runner.h
)

if(TENGWANGGE_BUILD_TESTS)
    # 本目录在根目录 enable_testing() 之前配置。先打开测试开关，add_test 才会写进 ctest。
    find_package(Qt5 5.15 REQUIRED COMPONENTS Test)
    add_executable(tengwangge_core_analyze_tests core_analyze_tests.cpp)
    target_link_libraries(tengwangge_core_analyze_tests PRIVATE tw_tool_core_analyze Qt5::Test)
    tw_apply_warnings(tengwangge_core_analyze_tests)
    set(CMAKE_TESTING_ENABLED 1)
    set(CMAKE_TESTING_ENABLED 1 PARENT_SCOPE)
    add_test(NAME core_analyze_tests COMMAND tengwangge_core_analyze_tests)
    set_tests_properties(core_analyze_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        TIMEOUT 180
    )
endif()

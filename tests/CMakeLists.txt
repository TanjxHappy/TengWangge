add_executable(tengwangge_core_tests core_tests.cpp)
target_link_libraries(tengwangge_core_tests PRIVATE tengwangge_core Qt5::Test)
tw_apply_warnings(tengwangge_core_tests)
add_test(NAME core_tests COMMAND tengwangge_core_tests)
set_tests_properties(core_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

# Keep the core library free of Widgets/Gui includes.
add_test(NAME core_layering
    COMMAND ${CMAKE_COMMAND}
        -DCORE_DIR=${PROJECT_SOURCE_DIR}/src/core
        -P ${PROJECT_SOURCE_DIR}/cmake/check_core_layering.cmake)

add_executable(tengwangge_ui_tests ui_tests.cpp)
target_link_libraries(tengwangge_ui_tests PRIVATE tengwangge_builtin_tools Qt5::Test)
tw_apply_warnings(tengwangge_ui_tests)
add_test(NAME ui_tests COMMAND tengwangge_ui_tests)
set_tests_properties(ui_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    TIMEOUT 120
)

# Core: platform state and rules. Qt Core only. No Widgets, no Gui.
add_library(tengwangge_core STATIC
    app_info.cpp
    data_paths.cpp
    log_buffer.cpp
    navigation_gate.cpp
    platform_services.cpp
    status_board.cpp
    tool_info.cpp
)
target_include_directories(tengwangge_core PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(tengwangge_core PUBLIC Qt5::Core)
target_compile_definitions(tengwangge_core PRIVATE TENGWANGGE_VERSION="${PROJECT_VERSION}")
tw_apply_warnings(tengwangge_core)

# Fails if anything under src/core pulls in Qt Widgets or Qt Gui.
# Usage: cmake -DCORE_DIR=<path to src/core> -P check_core_layering.cmake
if(NOT CORE_DIR)
    message(FATAL_ERROR "CORE_DIR is required")
endif()

file(GLOB_RECURSE core_files "${CORE_DIR}/*.h" "${CORE_DIR}/*.cpp" "${CORE_DIR}/CMakeLists.txt")
if(NOT core_files)
    message(FATAL_ERROR "No files found under ${CORE_DIR}")
endif()

set(forbidden_include
    "#[ \t]*include[ \t]*[<\"](QtWidgets|QtGui|QWidget|QApplication|QGuiApplication|QPixmap|QIcon|QImage|QColor|QPainter|QFont|QPalette|QBrush|QAction|QDialog|QMessageBox|QStyle)"
)
set(failed FALSE)
foreach(file IN LISTS core_files)
    file(READ "${file}" content)
    if(content MATCHES "${forbidden_include}")
        message(SEND_ERROR "Core must not depend on Widgets/Gui: ${file}")
        set(failed TRUE)
    endif()
    if(content MATCHES "Qt5::(Widgets|Gui)")
        message(SEND_ERROR "Core must not link Qt5::Widgets/Gui: ${file}")
        set(failed TRUE)
    endif()
endforeach()

if(failed)
    message(FATAL_ERROR "Core layering check failed")
endif()
message(STATUS "Core layering OK (${CORE_DIR})")

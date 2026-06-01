cmake_minimum_required(VERSION 3.31)

project(caret C CXX)

find_package(PkgConfig)
pkg_check_modules(ncursesw REQUIRED IMPORTED_TARGET ncursesw)

add_library(text-buffer text_buffer.cpp syntax.cpp)
target_include_directories(text-buffer PUBLIC include)

set_target_properties(text-buffer PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
)
add_subdirectory(tree-sitter)

add_executable(caret caret.cpp)
set_target_properties(caret PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
)
target_include_directories(caret PRIVATE include)

target_link_libraries(caret PRIVATE PkgConfig::ncursesw text-buffer)
install(TARGETS caret RUNTIME DESTINATION bin)

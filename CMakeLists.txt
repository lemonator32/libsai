cmake_minimum_required(VERSION 3.10)
project(libsai VERSION 1.0.0 LANGUAGES C)

option(SAI_BUILD_SHARED "Build libsai as a shared library" OFF)
option(SAI_BUILD_TOOLS  "Build the saitool command line program" ON)
option(SAI_BUILD_TESTS  "Build the test suite" ON)

set(SAI_SOURCES
    src/crypto.c
    src/document.c
    src/key.c
    src/rle.c
    src/util.c
    src/vfs_read.c
    src/vfs_write.c)

if(SAI_BUILD_SHARED)
    add_library(sai SHARED ${SAI_SOURCES})
    target_compile_definitions(sai PRIVATE SAI_BUILD_SHARED
                               INTERFACE SAI_USE_SHARED)
else()
    add_library(sai STATIC ${SAI_SOURCES})
endif()

target_include_directories(sai PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
set_target_properties(sai PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON)

if(MSVC)
    target_compile_options(sai PRIVATE /W4 /D_CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(sai PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(SAI_BUILD_TOOLS)
    add_executable(saitool tools/saitool.c)
    target_link_libraries(saitool PRIVATE sai)
    if(MSVC)
        target_compile_definitions(saitool PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
endif()

if(SAI_BUILD_TESTS)
    enable_testing()
    add_executable(test_libsai tests/test_main.c)
    target_link_libraries(test_libsai PRIVATE sai)
    if(MSVC)
        target_compile_definitions(test_libsai PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
    add_test(NAME libsai COMMAND test_libsai)
endif()

install(TARGETS sai ARCHIVE DESTINATION lib LIBRARY DESTINATION lib
        RUNTIME DESTINATION bin)
install(FILES include/sai.h DESTINATION include)

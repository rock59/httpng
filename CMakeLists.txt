cmake_minimum_required(VERSION 2.8)

project(httpng C CXX)

set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake" ${CMAKE_MODULE_PATH})
set(CMAKE_INCLUDE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake"
    ${CMAKE_INCLUDE_PATH})

include(CheckCSourceCompiles)

macro(add_compile_flags langs)
    foreach(_lang ${langs})
        string (REPLACE ";" " " _flags "${ARGN}")
        set ("CMAKE_${_lang}_FLAGS" "${CMAKE_${_lang}_FLAGS} ${_flags}")
        unset (${_lang})
        unset (${_flags})
    endforeach()
endmacro(add_compile_flags)

option(ENABLE_ASAN "Enable AddressSanitizer, a fast memory error detector based on compiler instrumentation" OFF)
if (ENABLE_ASAN)
    if (CMAKE_COMPILER_IS_GNUCC)
        message(FATAL_ERROR
            "\n"
            " Tarantool does not support GCC's AddressSanitizer. Use clang:\n"
            " $ git clean -xfd; git submodule foreach --recursive git clean -xfd\n"
            " $ CC=clang CXX=clang++ cmake . -DENABLE_ASAN=ON && make -j\n"
            "\n")
    endif()

    set(CMAKE_REQUIRED_FLAGS "-fsanitize=address -fsanitize-blacklist=${CMAKE_SOURCE_DIR}/asan/asan.supp")
    check_c_source_compiles("int main(void) {
        #include <sanitizer/asan_interface.h>
        void *x;
	    __sanitizer_finish_switch_fiber(x);
        return 0;
        }" ASAN_INTERFACE_OLD)
    check_c_source_compiles("int main(void) {
        #include <sanitizer/asan_interface.h>
        void *x;
	    __sanitizer_finish_switch_fiber(x, 0, 0);
        return 0;
    }" ASAN_INTERFACE_NEW)
    set(CMAKE_REQUIRED_FLAGS "")

    if (ASAN_INTERFACE_OLD)
        add_definitions(-DASAN_INTERFACE_OLD=1)
    elseif (ASAN_INTERFACE_NEW)
        add_definitions(-UASAN_INTERFACE_OLD)
    else()
        message(FATAL_ERROR "Cannot enable AddressSanitizer")
    endif()

    add_compile_flags("C;CXX" -fsanitize=address -fsanitize-blacklist=${CMAKE_SOURCE_DIR}/asan/asan.supp)
endif()

# Find Tarantool and Lua dependecies
set(Tarantool_FIND_REQUIRED ON)
find_package(Tarantool REQUIRED)
include_directories(${TARANTOOL_INCLUDE_DIRS} third_party/h2o/include
    third_party/h2o/deps/picotls/include
    third_party/h2o/deps/quicly/include
    third_party include)

function(build_module module files)
    add_library(${module} SHARED ${files})
    set_target_properties(${module} PROPERTIES PREFIX "")
    if(${CMAKE_SYSTEM_NAME} STREQUAL "Darwin")
        set_target_properties(${module} PROPERTIES LINK_FLAGS
            "-undefined dynamic_lookup")
    endif()
endfunction()

add_subdirectory(third_party/h2o EXCLUDE_FROM_ALL)
add_subdirectory(third_party/xtm EXCLUDE_FROM_ALL)

include_directories(${LUAJIT_INCLUDE_DIRS})
add_compile_options("-Wall" "-Werror")

set(HEADERS ${HEADERS} httpng/openssl_utils.h)

set(HTTPNG_C_SRC httpng/httpng.c
    httpng/httpng_http_thr.c
    httpng/openssl_utils.c
)

set(HTTPNG_SRC ${HTTPNG_C_SRC} ${HEADERS})

build_module(httpng "${HTTPNG_SRC}")
build_module(test_helpers httpng/test_helpers.c)
target_compile_options(test_helpers PRIVATE "-Wextra")

set(LIBS ${LIBS} xtm libh2o-evloop-pic)

target_link_libraries(httpng ${LIBS})
# FIXME: Make runtime configurable from Lua.
if (DISABLE_HTTP2)
    target_compile_definitions(httpng PRIVATE "DISABLE_HTTP2")
endif(DISABLE_HTTP2)

if (NOT(ENABLE_ASAN))
    # Just test
    add_custom_target(test-linkability ALL COMMAND
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/linkability-helper.sh"
	    "tests/stub"
	    "${CMAKE_CURRENT_SOURCE_DIR}/tests/linkability.lua")
    add_dependencies(test-linkability httpng)
endif (NOT(ENABLE_ASAN))

# Install library
install(TARGETS httpng LIBRARY DESTINATION ${TARANTOOL_INSTALL_LIBDIR})
install(TARGETS test_helpers LIBRARY DESTINATION ${TARANTOOL_INSTALL_LIBDIR})

add_executable(process_helper httpng/process_helper.c)
target_compile_options(process_helper PRIVATE "-Wextra")

add_custom_target(test_certs ALL COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_certs_helper.sh" "${CMAKE_CURRENT_SOURCE_DIR}")
add_custom_target(test COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/tests_helper.sh" "${CMAKE_CURRENT_SOURCE_DIR}")
add_dependencies(test test_helpers test_certs process_helper httpng)

cmake_minimum_required(VERSION 2.8.12...3.10)

if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(hostpid_broker_tests C)
    enable_testing()
endif()

add_executable(test_hostpid_broker
    ../test_hostpid_broker.c
    ../../src/hostpid_broker.c)
target_compile_definitions(test_hostpid_broker PRIVATE
    _GNU_SOURCE HOSTPID_TIMEOUT_MS=100 HOSTPID_BROKER_TESTING)
target_compile_options(test_hostpid_broker PRIVATE
    -std=c11 -Wall -Wextra -Werror -pedantic -UNDEBUG)
target_link_libraries(test_hostpid_broker -lpthread)
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(test_hostpid_broker -lrt)
endif()

add_test(NAME hostpid_broker COMMAND test_hostpid_broker)
set_tests_properties(hostpid_broker PROPERTIES TIMEOUT 20)

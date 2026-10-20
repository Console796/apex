include(doctest)

add_executable(apex_tests main.cpp core/version_test.cpp)
target_link_libraries(apex_tests PRIVATE apex_core apex_warnings doctest::doctest)
doctest_discover_tests(apex_tests)

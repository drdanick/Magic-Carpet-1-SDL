find_package(GTest CONFIG REQUIRED)

add_executable(remc1-tests
    test_main.cpp
)
target_link_libraries(remc1-tests PRIVATE remc1_engine GTest::gtest)

include(GoogleTest)
gtest_discover_tests(remc1-tests DISCOVERY_MODE PRE_TEST)

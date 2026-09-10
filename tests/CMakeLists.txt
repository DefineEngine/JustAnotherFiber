include(FetchContent)

# Pinned GoogleTest release. Bump deliberately, not implicitly.
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2
    GIT_SHALLOW    TRUE
)

# Match MSVC's dynamic CRT (/MD) used by the rest of the build.
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(jaf_tests
    TestMain.cpp
    FiberTest.cpp
    TaskTest.cpp
    CounterTest.cpp
    CounterStressTest.cpp
    StackGuardTest.cpp
    PoolExhaustionTest.cpp
    ParallelForTest.cpp
    IoTest.cpp
    IoWriteTest.cpp
    SyncTest.cpp
    CancellationTest.cpp
    SchedulerTest.cpp
    GraphTest.cpp
    GraphAsyncTest.cpp
    TimedWaitTest.cpp
    BlockingWaitTest.cpp
    CoroTest.cpp)

# Custom main (TestMain.cpp) suppresses Windows crash dialogs for death tests,
# so link GTest::gtest, NOT GTest::gtest_main.
target_link_libraries(jaf_tests
    PRIVATE
        jaf::jaf
        GTest::gtest)

include(GoogleTest)
# TIMEOUT so a regression in any hang path (pool exhaustion, deadlock) fails the
# test instead of blocking CI forever.
gtest_discover_tests(jaf_tests PROPERTIES TIMEOUT 30)

// Custom GoogleTest entry point (links GTest::gtest, not gtest_main).
//
// The library's misuse checks are always-on JAF_ASSERTs that call std::abort.
// On Windows an unconfigured abort()/access-violation pops a modal dialog and
// invokes Windows Error Reporting, which would hang EXPECT_DEATH tests waiting
// on a human. We suppress those here and route diagnostics to stderr so death
// tests observe the JAF_ASSERT message and exit deterministically.

#include <gtest/gtest.h>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#  include <crtdbg.h>
#  include <cstdlib>
#endif

int main(int argc, char** argv) {
#if defined(_WIN32)
    // Clear _WRITE_ABORT_MSG (no "abort() has been called" dialog) and
    // _CALL_REPORTFAULT (no Windows Error Reporting) -- passing 0 as the value
    // for both flags in the mask.
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    // No GP-fault / critical-error / open-file dialogs.
    ::SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    // Route CRT assert/error reports to stderr instead of a dialog.
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

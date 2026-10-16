// REMC1 entry point: port settings are resolved here, then control passes to the decompiled game.
#include "sub_main.h"
#include "portability/Settings.h"
#include "portability/port_watchdog.h"

#include <string>
#include <vector>

#if defined(_WIN32)
#include <crtdbg.h>
#include <stdlib.h>
#endif

static void ConfigureCrashReporting()
{
#if defined(_WIN32)
    // Never block on modal CRT/WER dialogs (heap corruption, asserts, abort): automated runs and
    // tests must fail fast and print the reason.
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
    _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif
}

int main(int argc, char* argv[])
{
    ConfigureCrashReporting();

    std::vector<std::string> gameArgs = InitSettings(argc, argv);
    if (gameArgs.empty())
        return 1;
    Watchdog_Start(g_settings.watchdogSeconds);

    std::vector<char*> gameArgv;
    for (auto& a : gameArgs)
        gameArgv.push_back(a.data());
    gameArgv.push_back(nullptr);

    sub_main((int)gameArgs.size(), gameArgv.data());
    return 0;
}

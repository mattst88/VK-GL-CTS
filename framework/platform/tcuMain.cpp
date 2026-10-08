/*-------------------------------------------------------------------------
 * drawElements Quality Program Tester Core
 * ----------------------------------------
 *
 * Copyright 2014 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 *//*!
 * \file
 * \brief Generic main().
 *//*--------------------------------------------------------------------*/

#include "tcuDefs.hpp"
#include "tcuCommandLine.hpp"
#include "tcuPlatform.hpp"
#include "tcuApp.hpp"
#include "tcuResource.hpp"
#include "tcuTestLog.hpp"
#include "tcuTestSessionExecutor.hpp"
#include "tcuZygote.hpp"
#include "deUniquePtr.hpp"
#include "qpDebugOut.h"

#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <vector>

#if (DE_OS == DE_OS_UNIX)
#include <unistd.h>
#endif

// Implement this in your platform port.
tcu::Platform *createPlatform(void);

namespace
{

bool disableRawWrites(int, const char *)
{
    return false;
}
bool disableFmtWrites(int, const char *, va_list)
{
    return false;
}
void disableStdout()
{
    qpRedirectOut(disableRawWrites, disableFmtWrites);
}

// Steps the application until the test session is complete.
int runSession(tcu::App &app, const tcu::CommandLine &cmdLine)
{
    for (;;)
    {
        if (!app.iterate())
        {
            if (cmdLine.getRunMode() == tcu::RUNMODE_EXECUTE &&
                (!app.getResult().isComplete || app.getResult().numFailed))
            {
                return EXIT_FAILURE;
            }

            return EXIT_SUCCESS;
        }
    }
}

// Creates the test hierarchy and runs a test session in a forked process
// for every client. See tcuZygote.hpp.
[[noreturn]] void runZygoteServer(int argc, char **argv, tcu::CommandLine &cmdLine)
{
    tcu::DirArchive archive(cmdLine.getArchiveDir());
    tcu::TestLog log("/dev/null", cmdLine.getLogFlags());
    de::UniquePtr<tcu::Platform> platform(createPlatform());
    tcu::App *const app = new tcu::App(*platform, archive, log, cmdLine, tcu::App::DEFER_SESSION);

    app->inflateTestHierarchy();

    // Only returns in a forked process, which takes over the client's command line.
    const std::vector<std::string> args = tcu::zygote::serve(cmdLine.getZygoteServerPath(), argc, argv);
    std::vector<const char *> sessionArgv;

    for (const std::string &arg : args)
        sessionArgv.push_back(arg.c_str());

    // The test hierarchy refers to the command line and the log, so these
    // are replaced in place.
    {
        // Reports errors (or prints help) like the constructor in main() does.
        tcu::CommandLine sessionCmdLine((int)sessionArgv.size(), sessionArgv.data());
    }
    cmdLine.~CommandLine();
    new (&cmdLine) tcu::CommandLine((int)sessionArgv.size(), sessionArgv.data());

    if (cmdLine.quietMode())
        disableStdout();

    log.~TestLog();
    try
    {
        new (&log) tcu::TestLog(cmdLine.getLogFileName(), cmdLine.getLogFlags());
    }
    catch (const std::exception &e)
    {
        // There is no log object to unwind with.
        tcu::die("%s", e.what());
    }

    app->startSession();

    const int exitStatus = runSession(*app, cmdLine);

    // Tearing down the hierarchy would only unshare its memory.
    app->endSession();
    log.~TestLog();
    fflush(nullptr);
    _exit(exitStatus);
}

} // anonymous namespace

int main(int argc, char **argv)
{
    int exitStatus = EXIT_SUCCESS;

#if (DE_OS != DE_OS_WIN32)
    // Set stdout to line-buffered mode (will be fully buffered by default if stdout is pipe).
    setvbuf(stdout, nullptr, _IOLBF, 4 * 1024);
#endif

    if (const char *const zygotePath = tcu::zygote::getClientSocketPath(argc, argv))
    {
        if (tcu::zygote::runInZygote(zygotePath, argc, argv, exitStatus))
            return exitStatus;
    }

    try
    {
        tcu::CommandLine cmdLine(argc, argv);

        if (cmdLine.quietMode())
            disableStdout();

        if (cmdLine.getZygoteServerPath()[0] != 0)
            runZygoteServer(argc, argv, cmdLine);

        tcu::DirArchive archive(cmdLine.getArchiveDir());
        tcu::TestLog log(cmdLine.getLogFileName(), cmdLine.getLogFlags());
        de::UniquePtr<tcu::Platform> platform(createPlatform());
        de::UniquePtr<tcu::App> app(new tcu::App(*platform, archive, log, cmdLine));

        exitStatus = runSession(*app, cmdLine);
    }
    catch (const std::exception &e)
    {
        tcu::die("%s", e.what());
    }

    return exitStatus;
}

#ifndef _TCUZYGOTE_HPP
#define _TCUZYGOTE_HPP
/*-------------------------------------------------------------------------
 * drawElements Quality Program Tester Core
 * ----------------------------------------
 *
 * Copyright (c) 2026 Matt Turner
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
 * \brief Zygote (fork server) for sharing the test hierarchy.
 *//*--------------------------------------------------------------------*/

#include "tcuDefs.hpp"

#include <string>
#include <vector>

namespace tcu
{
namespace zygote
{

/*--------------------------------------------------------------------*//*!
 * Creating the test hierarchy takes a significant part of the run time of
 * a short test session, and test runners typically split a test run into
 * many such sessions, each in a process of its own.
 *
 * A zygote server (--deqp-zygote-server=<socket>) creates the hierarchy
 * once and then forks a process for every session requested by a client
 * (--deqp-zygote=<socket>). The forked process takes over the client's
 * command line, environment and standard streams, and the client exits
 * the same way the forked process did, so to its parent the client looks
 * like a process that ran the tests itself.
 *
 * Requests are only served if the client is the same executable, runs in
 * the same directory and, apart from the options that select and log the
 * tests of a session, has the same command line as the server. Otherwise
 * the client runs the tests itself.
 *
 * A server only starts if all the test packages it would create support
 * it (TestPackage::supportsZygote()), since whatever creating the
 * hierarchy leaves behind is shared by all the sessions.
 *
 * The server runs until it is sent SIGTERM or, if its standard input is a
 * pipe, until the other end of the pipe is closed.
 *//*--------------------------------------------------------------------*/

//! Returns the socket path given with --deqp-zygote, or nullptr.
const char *getClientSocketPath(int argc, const char *const *argv);

//! Runs the command line in a process forked from the zygote server.
//! Returns false if the tests must be run by the calling process instead.
bool runInZygote(const char *socketPath, int argc, const char *const *argv, int &exitStatus);

//! Serves session requests. Only returns in a forked session process, with
//! the command line of the client (including the application name).
std::vector<std::string> serve(const char *socketPath, int argc, const char *const *argv);

} // namespace zygote
} // namespace tcu

#endif // _TCUZYGOTE_HPP

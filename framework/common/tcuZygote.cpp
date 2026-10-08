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

#include "tcuZygote.hpp"

#include <cstring>

#if (DE_OS == DE_OS_UNIX)

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>

#include <fcntl.h>
#include <poll.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

namespace tcu
{
namespace zygote
{

namespace
{

const char *const s_clientOption = "--deqp-zygote=";

// Options that only affect how a session is executed. All other options may
// affect the test hierarchy and must be the same for the server and client.
const char *const s_sessionOptions[] = {
    "deqp-case",
    "deqp-caselist",
    "deqp-caselist-file",
    "deqp-caselist-resource",
    "deqp-stdin-caselist",
    "deqp-fraction",
    "deqp-fraction-mandatory-caselist-file",
    "deqp-log-filename",
    "deqp-log-images",
    "deqp-log-all-images",
    "deqp-log-shader-sources",
    "deqp-log-decompiled-spirv",
    "deqp-log-empty-loginfo",
    "deqp-log-flush",
    "deqp-log-compact",
    "deqp-watchdog",
    "deqp-crashhandler",
    "deqp-shadercache",
    "deqp-shadercache-filename",
    "deqp-shadercache-truncate",
    "deqp-terminate-on-fail",
    "deqp-terminate-on-device-lost",
    "deqp-vk-device-id",
    "deqp-zygote",
    "deqp-zygote-server",
};

const uint32_t s_requestMagic = 0x7a79676fu;
const int s_numPassedFds      = 3; // stdin, stdout, stderr

struct RequestHeader
{
    uint32_t magic;
    uint32_t payloadSize;
};

enum ReplyType
{
    REPLY_EXITED  = 1, //!< value is the wait status of the session process.
    REPLY_REFUSED = 2, //!< value is the size of the reason string that follows.
};

struct Reply
{
    uint32_t type;
    int32_t value;
};

bool readAll(int fd, void *data, size_t size)
{
    char *ptr = static_cast<char *>(data);

    while (size > 0)
    {
        const ssize_t n = read(fd, ptr, size);

        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;

        ptr += n;
        size -= (size_t)n;
    }

    return true;
}

bool writeAll(int fd, const void *data, size_t size)
{
    const char *ptr = static_cast<const char *>(data);

    while (size > 0)
    {
        const ssize_t n = send(fd, ptr, size, MSG_NOSIGNAL);

        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;

        ptr += n;
        size -= (size_t)n;
    }

    return true;
}

bool initAddress(sockaddr_un &addr, const char *socketPath)
{
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;

    if (strlen(socketPath) >= sizeof(addr.sun_path))
        return false;

    strcpy(addr.sun_path, socketPath);
    return true;
}

std::string getWorkingDirectory(void)
{
    char *const cwd = getcwd(nullptr, 0);
    const std::string result(cwd ? cwd : "");

    free(cwd);
    return result;
}

// Identifies the executable, so that a binary that was rebuilt after the
// server was started does not use the hierarchy of the old one.
std::string getExecutablePath(void)
{
    char buf[4096];
    const ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf));

    return len > 0 ? std::string(buf, (size_t)len) : std::string();
}

std::vector<std::string> getHierarchyArgs(const std::vector<std::string> &args)
{
    std::vector<std::string> result;

    for (size_t ndx = 1; ndx < args.size(); ndx++)
    {
        const std::string &arg = args[ndx];

        if (arg == "-n")
        {
            ndx++; // Skip the case name pattern too.
            continue;
        }

        if (arg.compare(0, 2, "--") == 0)
        {
            const std::string name = arg.substr(2, arg.find('=') - 2);

            if (std::find(std::begin(s_sessionOptions), std::end(s_sessionOptions), name) != std::end(s_sessionOptions))
                continue;
        }

        result.push_back(arg);
    }

    std::sort(result.begin(), result.end());
    return result;
}

int getNumThreads(void)
{
    FILE *const file = fopen("/proc/self/status", "r");
    int numThreads   = 1;
    char line[256];

    if (!file)
        return 1;

    while (fgets(line, sizeof(line), file))
    {
        if (sscanf(line, "Threads: %d", &numThreads) == 1)
            break;
    }

    fclose(file);
    return numThreads;
}

// Server

struct Session
{
    pid_t pid;
    int connection; //!< -1 once the client is gone.
};

int s_signalPipe[2] = {-1, -1};

void onSignal(int signo)
{
    const int savedErrno     = errno;
    const unsigned char byte = (unsigned char)signo;

    if (write(s_signalPipe[1], &byte, 1) < 0)
    {
        // Nothing to be done in a signal handler.
    }

    errno = savedErrno;
}

const int s_serverSignals[] = {SIGCHLD, SIGTERM, SIGINT, SIGHUP};

[[noreturn]] void refuse(int connection, const std::string &reason)
{
    const Reply reply = {REPLY_REFUSED, (int32_t)reason.size()};

    if (writeAll(connection, &reply, sizeof(reply)))
        writeAll(connection, reason.data(), reason.size());

    _exit(EXIT_SUCCESS);
}

// Turns the forked process into the session requested through connection.
std::vector<std::string> becomeSession(int connection, pid_t serverPid, const std::vector<std::string> &serverArgs)
{
    for (int signo : s_serverSignals)
        signal(signo, SIG_DFL);
    signal(SIGPIPE, SIG_DFL);

    // Don't outlive the server; nobody would report the exit status.
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    if (getppid() != serverPid)
        _exit(EXIT_FAILURE);

    // Receive the request.
    RequestHeader header;
    int fds[s_numPassedFds];
    {
        char control[CMSG_SPACE(sizeof(fds))];
        iovec iov = {&header, sizeof(header)};
        msghdr msg;

        memset(&msg, 0, sizeof(msg));
        msg.msg_iov        = &iov;
        msg.msg_iovlen     = 1;
        msg.msg_control    = control;
        msg.msg_controllen = sizeof(control);

        ssize_t n;
        do
        {
            n = recvmsg(connection, &msg, MSG_WAITALL);
        } while (n < 0 && errno == EINTR);

        const cmsghdr *const cmsg = CMSG_FIRSTHDR(&msg);

        if (n != (ssize_t)sizeof(header) || header.magic != s_requestMagic || !cmsg || cmsg->cmsg_level != SOL_SOCKET ||
            cmsg->cmsg_type != SCM_RIGHTS || cmsg->cmsg_len != CMSG_LEN(sizeof(fds)))
            _exit(EXIT_FAILURE);

        memcpy(fds, CMSG_DATA(cmsg), sizeof(fds));
    }

    std::vector<char> payload(header.payloadSize);
    if (payload.empty() || !readAll(connection, payload.data(), payload.size()) || payload.back() != 0)
        _exit(EXIT_FAILURE);

    // The payload is a sequence of null-terminated strings: the working
    // directory, the executable, and the arguments and environment
    // variables prefixed with 'A' and 'E'.
    std::vector<std::string> strings;
    for (size_t pos = 0; pos < payload.size(); pos += strings.back().size() + 1)
        strings.push_back(&payload[pos]);

    if (strings.size() < 3)
        _exit(EXIT_FAILURE);

    std::vector<std::string> args;
    std::vector<std::string> env;
    for (size_t ndx = 2; ndx < strings.size(); ndx++)
    {
        if (strings[ndx][0] == 'A')
            args.push_back(strings[ndx].substr(1));
        else if (strings[ndx][0] == 'E')
            env.push_back(strings[ndx].substr(1));
        else
            _exit(EXIT_FAILURE);
    }

    if (args.empty())
        _exit(EXIT_FAILURE);

    // The hierarchy was created by this executable, in this directory,
    // with the options of the server.
    if (strings[0] != getWorkingDirectory())
        refuse(connection, "working directory differs from the server's");
    if (strings[1] != getExecutablePath())
        refuse(connection, "executable differs from the server's");
    if (getHierarchyArgs(args) != getHierarchyArgs(serverArgs))
        refuse(connection, "options differ from the server's");

    for (int ndx = 0; ndx < s_numPassedFds; ndx++)
    {
        if (dup2(fds[ndx], ndx) < 0)
            _exit(EXIT_FAILURE);
        close(fds[ndx]);
    }

    clearenv();
    for (const std::string &var : env)
        putenv(strdup(var.c_str()));

    close(connection);
    return args;
}

} // namespace

const char *getClientSocketPath(int argc, const char *const *argv)
{
    const size_t optionLen = strlen(s_clientOption);
    const char *path       = nullptr;

    for (int ndx = 1; ndx < argc; ndx++)
    {
        if (strncmp(argv[ndx], s_clientOption, optionLen) == 0)
            path = argv[ndx] + optionLen;
    }

    return (path && path[0]) ? path : nullptr;
}

bool runInZygote(const char *socketPath, int argc, const char *const *argv, int &exitStatus)
{
    sockaddr_un addr;
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);

    if (fd < 0 || !initAddress(addr, socketPath) || connect(fd, (const sockaddr *)&addr, sizeof(addr)) != 0)
    {
        fprintf(stderr, "dEQP zygote: failed to connect to '%s' (%s), running the tests in this process\n", socketPath,
                strerror(errno));
        if (fd >= 0)
            close(fd);
        return false;
    }

    std::string payload;
    payload += getWorkingDirectory() + '\0';
    payload += getExecutablePath() + '\0';
    for (int ndx = 0; ndx < argc; ndx++)
        payload += std::string("A") + argv[ndx] + '\0';
    for (char **var = environ; *var; var++)
        payload += std::string("E") + *var + '\0';

    // Send the header along with the standard streams.
    {
        RequestHeader header          = {s_requestMagic, (uint32_t)payload.size()};
        const int fds[s_numPassedFds] = {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO};
        char control[CMSG_SPACE(sizeof(fds))];
        iovec iov = {&header, sizeof(header)};
        msghdr msg;

        memset(&msg, 0, sizeof(msg));
        memset(control, 0, sizeof(control));
        msg.msg_iov        = &iov;
        msg.msg_iovlen     = 1;
        msg.msg_control    = control;
        msg.msg_controllen = sizeof(control);

        cmsghdr *const cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level    = SOL_SOCKET;
        cmsg->cmsg_type     = SCM_RIGHTS;
        cmsg->cmsg_len      = CMSG_LEN(sizeof(fds));
        memcpy(CMSG_DATA(cmsg), fds, sizeof(fds));

        ssize_t n;
        do
        {
            n = sendmsg(fd, &msg, MSG_NOSIGNAL);
        } while (n < 0 && errno == EINTR);

        if (n != (ssize_t)sizeof(header) || !writeAll(fd, payload.data(), payload.size()))
        {
            fprintf(stderr, "dEQP zygote: failed to send the request, running the tests in this process\n");
            close(fd);
            return false;
        }
    }

    // Wait for the session to end. If this process is killed, the server
    // notices the closed connection and kills the session process.
    Reply reply;
    if (!readAll(fd, &reply, sizeof(reply)))
    {
        fprintf(stderr, "dEQP zygote: connection to the server was lost\n");
        close(fd);
        exitStatus = EXIT_FAILURE;
        return true;
    }

    if (reply.type == REPLY_EXITED)
    {
        close(fd);

        if (WIFSIGNALED(reply.value))
        {
            // Die the same way, without a second core dump.
            const rlimit noCore = {0, 0};
            setrlimit(RLIMIT_CORE, &noCore);
            signal(WTERMSIG(reply.value), SIG_DFL);
            fflush(nullptr);
            raise(WTERMSIG(reply.value));
            exitStatus = 128 + WTERMSIG(reply.value);
        }
        else
            exitStatus = WEXITSTATUS(reply.value);

        return true;
    }

    std::string reason("unexpected reply");
    if (reply.type == REPLY_REFUSED && reply.value >= 0)
    {
        std::vector<char> buf((size_t)reply.value);
        if (buf.empty() || readAll(fd, buf.data(), buf.size()))
            reason.assign(buf.begin(), buf.end());
    }

    fprintf(stderr, "dEQP zygote: request refused (%s), running the tests in this process\n", reason.c_str());
    close(fd);
    return false;
}

std::vector<std::string> serve(const char *socketPath, int argc, const char *const *argv)
{
    const std::vector<std::string> serverArgs(argv, argv + argc);
    const pid_t serverPid = getpid();
    std::vector<Session> sessions;
    sockaddr_un addr;

    // Threads do not survive fork(), nor does the state they share with
    // the rest of the process.
    if (getNumThreads() != 1)
        throw Exception("Creating the test hierarchy started threads; can't serve as a zygote");

    if (!initAddress(addr, socketPath))
        throw Exception(std::string("Socket path is too long: ") + socketPath);

    const int listenFd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listenFd < 0)
        throw Exception(std::string("socket() failed: ") + strerror(errno));

    if (connect(listenFd, (const sockaddr *)&addr, sizeof(addr)) == 0)
        throw Exception(std::string("A zygote server is already listening on ") + socketPath);
    unlink(socketPath);

    if (bind(listenFd, (const sockaddr *)&addr, sizeof(addr)) != 0 || listen(listenFd, SOMAXCONN) != 0)
        throw Exception(std::string("Failed to listen on ") + socketPath + ": " + strerror(errno));

    if (pipe2(s_signalPipe, O_CLOEXEC | O_NONBLOCK) != 0)
        throw Exception(std::string("pipe2() failed: ") + strerror(errno));

    for (int signo : s_serverSignals)
    {
        struct sigaction action;

        memset(&action, 0, sizeof(action));
        action.sa_handler = onSignal;
        action.sa_flags   = SA_RESTART | (signo == SIGCHLD ? SA_NOCLDSTOP : 0);
        sigaction(signo, &action, nullptr);
    }
    signal(SIGPIPE, SIG_IGN);

    // A runner that passes a pipe as standard input ties the lifetime of
    // the server to its own, however it exits.
    struct stat stdinStat;
    const bool watchStdin = fstat(STDIN_FILENO, &stdinStat) == 0 && S_ISFIFO(stdinStat.st_mode);

    printf("dEQP zygote: listening on %s\n", socketPath);
    fflush(nullptr);

    for (;;)
    {
        std::vector<pollfd> pollFds;

        pollFds.push_back({listenFd, POLLIN, 0});
        pollFds.push_back({s_signalPipe[0], POLLIN, 0});
        pollFds.push_back({watchStdin ? STDIN_FILENO : -1, 0, 0});
        for (const Session &session : sessions)
        {
            // \note poll() ignores negative fds. The request is read by the
            //       session process, so only wait for the client to hang up.
            pollFds.push_back({session.connection, POLLRDHUP, 0});
        }

        if (poll(pollFds.data(), pollFds.size(), -1) < 0)
        {
            if (errno == EINTR)
                continue;
            break;
        }

        // A client that is gone takes its session with it.
        for (size_t ndx = 0; ndx < sessions.size(); ndx++)
        {
            if ((pollFds[3 + ndx].revents & (POLLRDHUP | POLLHUP | POLLERR)) != 0)
            {
                kill(sessions[ndx].pid, SIGKILL);
                close(sessions[ndx].connection);
                sessions[ndx].connection = -1;
            }
        }

        if ((pollFds[2].revents & (POLLHUP | POLLERR)) != 0)
            break;

        if (pollFds[1].revents != 0)
        {
            bool terminate = false;
            unsigned char signo;

            while (read(s_signalPipe[0], &signo, 1) == 1)
            {
                if (signo != SIGCHLD)
                    terminate = true;
            }

            if (terminate)
                break;

            int status;
            pid_t pid;
            while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
            {
                for (size_t ndx = 0; ndx < sessions.size(); ndx++)
                {
                    if (sessions[ndx].pid != pid)
                        continue;

                    if (sessions[ndx].connection >= 0)
                    {
                        const Reply reply = {REPLY_EXITED, status};
                        writeAll(sessions[ndx].connection, &reply, sizeof(reply));
                        close(sessions[ndx].connection);
                    }

                    sessions.erase(sessions.begin() + (ptrdiff_t)ndx);
                    break;
                }
            }
        }

        if (pollFds[0].revents != 0)
        {
            const int connection = accept4(listenFd, nullptr, nullptr, SOCK_CLOEXEC);

            if (connection < 0)
                continue;

            const pid_t pid = fork();

            if (pid == 0)
            {
                close(listenFd);
                close(s_signalPipe[0]);
                close(s_signalPipe[1]);
                for (const Session &session : sessions)
                {
                    if (session.connection >= 0)
                        close(session.connection);
                }

                return becomeSession(connection, serverPid, serverArgs);
            }
            else if (pid > 0)
                sessions.push_back({pid, connection});
            else
            {
                const std::string reason = std::string("fork() failed: ") + strerror(errno);
                const Reply reply        = {REPLY_REFUSED, (int32_t)reason.size()};

                if (writeAll(connection, &reply, sizeof(reply)))
                    writeAll(connection, reason.data(), reason.size());
                close(connection);
            }
        }
    }

    for (const Session &session : sessions)
        kill(session.pid, SIGKILL);
    unlink(socketPath);

    // The hierarchy is not worth tearing down.
    _exit(EXIT_SUCCESS);
}

} // namespace zygote
} // namespace tcu

#else // DE_OS != DE_OS_UNIX

namespace tcu
{
namespace zygote
{

const char *getClientSocketPath(int, const char *const *)
{
    return nullptr;
}

bool runInZygote(const char *, int, const char *const *, int &)
{
    return false;
}

std::vector<std::string> serve(const char *, int, const char *const *)
{
    throw NotSupportedError("Zygote server is not supported on this platform");
}

} // namespace zygote
} // namespace tcu

#endif // DE_OS == DE_OS_UNIX

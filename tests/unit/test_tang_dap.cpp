// SPDX-License-Identifier: LGPL-3.0-only
//
// Copyright (C) 2024-2026 Corey Pennycuff
//
// This file is part of Ghoti.io Lang-tang.
//
// Ghoti.io Lang-tang is free software: you can redistribute it and/or modify it
// under the terms of the GNU Lesser General Public License version 3 as
// published by the Free Software Foundation.
//
// Ghoti.io Lang-tang is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
// FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License
// for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

// The `tang` command as a debugger host (story 13, CAP-1 and CAP-5): a scripted
// Debug Adapter Protocol session against the real binary, forked with pipes.
//
// This runner is a host, not the library: it names no debugger header and links
// nothing of runtime-debug. It frames the protocol by hand (`Content-Length`),
// which is also what a client does, so the session it plays is the one a
// stranger's editor would have. Every wait is bounded; a hung `tang` is a failed
// test, not a hung build.

#include <gtest/gtest.h>

#ifdef _WIN32

// The session is played with fork, pipes, poll and /proc/self/exe, and the
// command's --dap needs the descriptor transport, which is a stub on Windows
// (runtime-debug's transport.h). What the command does there instead (a message
// and exit status 7) is checked by tests/cli-test.sh.
TEST(TangDap, NeedsTheDescriptorTransportAndSoIsNotRunOnWindows) {
  GTEST_SKIP() << "fork, pipes and poll; --dap is unsupported on Windows (see cli-test.sh)";
}

#else  // !_WIN32

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

const int kWaitMs = 20000;

// Where the command is. The Makefile builds it before any suite that needs it:
// next to the tests, or (for the sanitizer trees, whose apps directory is a
// sibling of the release one) in the release or debug tree.
std::string tang_path() {
  if (const char * env = std::getenv("GLTANG_TANG")) {
    return env;
  }
  char self[4096];
  ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
  std::string dir = ".";
  if (n > 0) {
    self[n] = 0;
    dir = self;
    dir = dir.substr(0, dir.rfind('/'));
  }
  for (const char * candidate : {"/tang", "/../../release/apps/tang", "/../../debug/apps/tang", "/../release/apps/tang"}) {
    std::string path = dir + candidate;
    if (access(path.c_str(), X_OK) == 0) {
      return path;
    }
  }
  return dir + "/tang";  // not found: the test that runs it says so
}

// A directory for the sources the tests run.
struct TempDir {
  std::string path;
  TempDir() {
    const char * base = std::getenv("TMPDIR");
    std::string pattern = std::string(base && *base ? base : "/tmp") + "/tang-dap-XXXXXX";
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back(0);
    if (mkdtemp(buffer.data())) {
      path = buffer.data();
    }
  }
  ~TempDir() {
    if (!path.empty()) {
      std::string command = "rm -rf '" + path + "'";
      int ignored = std::system(command.c_str());
      (void)ignored;
    }
  }
  std::string write(const std::string & name, const std::string & text) const {
    std::string file = path + "/" + name;
    std::ofstream out(file, std::ios::binary);
    out << text;
    return file;
  }
};

std::string slurp(int fd) {
  std::string out;
  char chunk[4096];
  ssize_t n;
  while ((n = read(fd, chunk, sizeof(chunk))) > 0) {
    out.append(chunk, (size_t)n);
  }
  return out;
}

// The first integer after `key` in `text` (a JSON field, found without a JSON
// parser: the shapes read here are fixed), or -1.
long number_after(const std::string & text, const std::string & key, size_t from = 0) {
  size_t at = text.find(key, from);
  if (at == std::string::npos) {
    return -1;
  }
  return std::strtol(text.c_str() + at + key.size(), nullptr, 10);
}

struct Result {
  std::string out;
  std::string err;
  int status = -1;  // the exit status, or 128 + the signal
};

// A child `tang` with its stdin and stdout piped to us and its stderr in a file.
class Tang {
 public:
  explicit Tang(const std::vector<std::string> & args, bool pipe_stdin = true) {
    char errname[] = "/tmp/tang-dap-err-XXXXXX";
    err_fd_ = mkstemp(errname);
    err_name_ = errname;
    int to_child[2] = {-1, -1};
    int from_child[2] = {-1, -1};
    EXPECT_EQ(pipe(to_child), 0);
    EXPECT_EQ(pipe(from_child), 0);
    pid_ = fork();
    if (pid_ == 0) {
      dup2(pipe_stdin ? to_child[0] : open("/dev/null", O_RDONLY), 0);
      dup2(from_child[1], 1);
      dup2(err_fd_, 2);
      close(to_child[0]);
      close(to_child[1]);
      close(from_child[0]);
      close(from_child[1]);
      std::vector<const char *> argv;
      std::string path = tang_path();
      // The command is an ordinary release build with its own library next to
      // it. A sanitizer run puts a different apps directory first in
      // LD_LIBRARY_PATH (where an installed, older lang-tang can win over the
      // command's own RUNPATH) and may preload its runtime: neither belongs in
      // the child.
      std::string own = path.substr(0, path.rfind('/'));
      const char * inherited = std::getenv("LD_LIBRARY_PATH");
      setenv("LD_LIBRARY_PATH", (own + (inherited && *inherited ? std::string(":") + inherited : std::string())).c_str(), 1);
      unsetenv("LD_PRELOAD");
      argv.push_back(path.c_str());
      for (const std::string & a : args) {
        argv.push_back(a.c_str());
      }
      argv.push_back(nullptr);
      execv(path.c_str(), const_cast<char * const *>(argv.data()));
      _exit(127);
    }
    close(to_child[0]);
    close(from_child[1]);
    in_ = to_child[1];
    out_ = from_child[0];
    signal(SIGPIPE, SIG_IGN);
  }

  ~Tang() {
    if (in_ >= 0) {
      close(in_);
    }
    if (out_ >= 0) {
      close(out_);
    }
    if (pid_ > 0 && !reaped_) {
      kill(pid_, SIGKILL);
      waitpid(pid_, nullptr, 0);
    }
    if (err_fd_ >= 0) {
      close(err_fd_);
    }
    unlink(err_name_.c_str());
  }

  void send(const std::string & body) {
    std::string message = "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    size_t done = 0;
    while (done < message.size()) {
      ssize_t n = write(in_, message.data() + done, message.size() - done);
      ASSERT_GT(n, 0) << "tang stopped reading its stdin";
      done += (size_t)n;
    }
  }

  // The next framed message, or "" at the end of the stream or after a bounded wait.
  std::string read_message(int timeout_ms = kWaitMs) {
    auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
      size_t header_end = buffer_.find("\r\n\r\n");
      if (header_end != std::string::npos) {
        size_t length = (size_t)std::strtoul(buffer_.c_str() + std::string("Content-Length: ").size(), nullptr, 10);
        if (buffer_.size() >= header_end + 4 + length) {
          std::string body = buffer_.substr(header_end + 4, length);
          buffer_.erase(0, header_end + 4 + length);
          return body;
        }
      }
      int left = (int)std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
      if (left <= 0) {
        ADD_FAILURE() << "no message from tang within " << timeout_ms << " ms; buffered: " << buffer_;
        return "";
      }
      struct pollfd p = {out_, POLLIN, 0};
      int ready = poll(&p, 1, left);
      if (ready <= 0) {
        continue;
      }
      char chunk[4096];
      ssize_t n = read(out_, chunk, sizeof(chunk));
      if (n <= 0) {
        return "";
      }
      buffer_.append(chunk, (size_t)n);
    }
  }

  // Sends a request and returns its response; events that come first are kept.
  std::string request(const std::string & command, const std::string & arguments = "{}") {
    int seq = ++seq_;
    send("{\"seq\":" + std::to_string(seq) + ",\"type\":\"request\",\"command\":\"" + command + "\",\"arguments\":" + arguments + "}");
    std::string needle = "\"request_seq\":" + std::to_string(seq) + ",";
    for (;;) {
      std::string message = read_message();
      if (message.empty()) {
        return "";
      }
      if (message.find(needle) != std::string::npos) {
        return message;
      }
      events_.push_back(message);
    }
  }

  // The next event named `name`, from those already read or the stream.
  std::string event(const std::string & name) {
    std::string needle = "\"event\":\"" + name + "\"";
    for (size_t i = 0; i < events_.size(); ++i) {
      if (events_[i].find(needle) != std::string::npos) {
        std::string found = events_[i];
        events_.erase(events_.begin() + (long)i);
        return found;
      }
    }
    for (;;) {
      std::string message = read_message();
      if (message.empty()) {
        return "";
      }
      if (message.find(needle) != std::string::npos) {
        return message;
      }
    }
  }

  // The next of "stopped" and "terminated" (the run was stopped, or it ended),
  // by name; "" if neither comes. Other events are skipped.
  std::string next_stop_or_end() {
    for (size_t i = 0; i < events_.size(); ++i) {
      for (const char * name : {"stopped", "terminated"}) {
        if (events_[i].find(std::string("\"event\":\"") + name + "\"") != std::string::npos) {
          events_.erase(events_.begin() + (long)i);
          return name;
        }
      }
    }
    for (;;) {
      std::string message = read_message();
      if (message.empty()) {
        return "";
      }
      for (const char * name : {"stopped", "terminated"}) {
        if (message.find(std::string("\"event\":\"") + name + "\"") != std::string::npos) {
          return name;
        }
      }
    }
  }

  // The usual opening: initialize, launch, the breakpoints, configurationDone.
  void configure(const std::string & file, const std::vector<int> & lines) {
    EXPECT_NE(request("initialize", "{\"adapterID\":\"tang\",\"linesStartAt1\":true}").find("\"success\":true"), std::string::npos);
    EXPECT_NE(request("launch").find("\"success\":true"), std::string::npos);
    if (!lines.empty()) {
      std::string list;
      for (int line : lines) {
        list += (list.empty() ? "" : ",") + std::string("{\"line\":") + std::to_string(line) + "}";
      }
      std::string reply = request("setBreakpoints", "{\"source\":{\"path\":\"" + file + "\"},\"breakpoints\":[" + list + "]}");
      EXPECT_NE(reply.find("\"success\":true"), std::string::npos) << reply;
    }
    EXPECT_NE(request("configurationDone").find("\"success\":true"), std::string::npos);
  }

  // The top frame's line, the number of frames, and the top frame's id.
  struct Where {
    long line = -1;
    long frames = -1;
    long id = -1;
  };
  Where where() {
    std::string reply = request("stackTrace", "{\"threadId\":1}");
    Where w;
    w.line = number_after(reply, "\"line\":");
    w.frames = number_after(reply, "\"totalFrames\":");
    w.id = number_after(reply, "\"id\":");
    return w;
  }

  // The variables of the first scope of a frame, as one string.
  std::string locals(long frame_id, std::string * scopes_out = nullptr) {
    std::string scopes = request("scopes", "{\"frameId\":" + std::to_string(frame_id) + "}");
    if (scopes_out) {
      *scopes_out = scopes;
    }
    long reference = number_after(scopes, "\"variablesReference\":");
    return request("variables", "{\"variablesReference\":" + std::to_string(reference) + "}");
  }

  // Closes stdin and waits (bounded) for the exit; the rest of stdout is read.
  Result finish() {
    Result r;
    close(in_);
    in_ = -1;
    auto deadline = Clock::now() + std::chrono::milliseconds(kWaitMs);
    int wstatus = 0;
    for (;;) {
      pid_t done = waitpid(pid_, &wstatus, WNOHANG);
      if (done == pid_) {
        break;
      }
      if (Clock::now() > deadline) {
        ADD_FAILURE() << "tang did not exit within " << kWaitMs << " ms";
        kill(pid_, SIGKILL);
        waitpid(pid_, &wstatus, 0);
        break;
      }
      // Drain stdout so that a child blocked writing can finish.
      struct pollfd p = {out_, POLLIN, 0};
      if (poll(&p, 1, 20) > 0) {
        char chunk[4096];
        ssize_t n = read(out_, chunk, sizeof(chunk));
        if (n > 0) {
          buffer_.append(chunk, (size_t)n);
        }
      }
    }
    reaped_ = true;
    r.status = WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : 128 + WTERMSIG(wstatus);
    fcntl(out_, F_SETFL, fcntl(out_, F_GETFL) | O_NONBLOCK);
    r.out = buffer_ + slurp(out_);
    lseek(err_fd_, 0, SEEK_SET);
    r.err = slurp(err_fd_);
    return r;
  }

 private:
  pid_t pid_ = -1;
  int in_ = -1;
  int out_ = -1;
  int err_fd_ = -1;
  std::string err_name_;
  std::string buffer_;
  std::vector<std::string> events_;
  int seq_ = 0;
  bool reaped_ = false;
};

// Runs `tang` with no debugger and returns what it wrote and how it ended.
Result run_plain(const std::vector<std::string> & args) {
  Tang tang(args, false);
  return tang.finish();
}

const char * const kProgram =
    "function inner(n) {\n"      // 1
    "  total = n * 2;\n"         // 2
    "  return total;\n"          // 3
    "}\n"                        // 4
    "a = 1;\n"                   // 5
    "b = inner(a);\n"            // 6
    "c = b + 1;\n"               // 7
    "print(c);\n";               // 8

// A loop, a branch never taken, and output.
const char * const kLoop =
    "sum = 0;\n"                          // 1
    "for (i = 0; i < 3; i += 1) {\n"      // 2
    "  sum += i;\n"                       // 3
    "  print(sum + \",\");\n"             // 4
    "}\n"                                 // 5
    "if (sum > 100) {\n"                  // 6
    "  print(\"never\");\n"               // 7
    "}\n"                                 // 8
    "print(\"done\");\n";                 // 9

}  // namespace

TEST(TangDap, TheCommandIsWhereTheTestExpectsIt) {
  EXPECT_EQ(access(tang_path().c_str(), X_OK), 0) << "no tang at " << tang_path() << " (the Makefile builds it first)";
}

TEST(TangDap, AScriptedSessionHitsABreakpointReadsLocalsAndStepsInOverAndOut) {
  TempDir dir;
  std::string file = dir.write("session.tang", kProgram);
  Tang tang({"--script", "--dap", file});
  tang.configure(file, {6});

  // The breakpoint: line 6, one frame.
  std::string stopped = tang.event("stopped");
  EXPECT_NE(stopped.find("\"reason\":\"breakpoint\""), std::string::npos) << stopped;
  Tang::Where at = tang.where();
  EXPECT_EQ(at.line, 6);
  EXPECT_EQ(at.frames, 1);

  // Locals and scopes: the program's variables, `a` already assigned.
  std::string scopes;
  std::string variables = tang.locals(at.id, &scopes);
  EXPECT_NE(scopes.find("\"name\":\"program\""), std::string::npos) << scopes;
  EXPECT_NE(variables.find("\"name\":\"a\",\"value\":\"1\""), std::string::npos) << variables;
  EXPECT_NE(variables.find("\"name\":\"b\",\"value\":\"null\""), std::string::npos) << variables;

  // Step in: the call. The function's frame is on top, two deep, at its entry.
  EXPECT_NE(tang.request("stepIn", "{\"threadId\":1}").find("\"success\":true"), std::string::npos);
  stopped = tang.event("stopped");
  EXPECT_NE(stopped.find("\"reason\":\"step\""), std::string::npos) << stopped;
  at = tang.where();
  EXPECT_EQ(at.frames, 2);
  EXPECT_EQ(at.line, 1) << "a call polls at its entry, which is the line of the `function`";
  std::string inner_scopes;
  std::string inner_locals = tang.locals(at.id, &inner_scopes);
  EXPECT_NE(inner_scopes.find("\"name\":\"inner\""), std::string::npos) << inner_scopes;
  EXPECT_NE(inner_locals.find("\"name\":\"n\",\"value\":\"1\""), std::string::npos) << inner_locals;

  // Step over: the next statement in the same frame.
  EXPECT_NE(tang.request("next", "{\"threadId\":1}").find("\"success\":true"), std::string::npos);
  stopped = tang.event("stopped");
  EXPECT_NE(stopped.find("\"reason\":\"step\""), std::string::npos) << stopped;
  at = tang.where();
  EXPECT_EQ(at.frames, 2);
  EXPECT_EQ(at.line, 2);
  EXPECT_NE(tang.locals(at.id).find("\"name\":\"n\",\"value\":\"1\""), std::string::npos);

  // Step out: back in the caller, at the statement after the call's.
  EXPECT_NE(tang.request("stepOut", "{\"threadId\":1}").find("\"success\":true"), std::string::npos);
  stopped = tang.event("stopped");
  EXPECT_NE(stopped.find("\"reason\":\"step\""), std::string::npos) << stopped;
  at = tang.where();
  EXPECT_EQ(at.frames, 1);
  EXPECT_EQ(at.line, 7);
  EXPECT_NE(tang.locals(at.id).find("\"name\":\"b\",\"value\":\"2\""), std::string::npos) << "the call's value is assigned";

  // Continue: the run finishes; `terminated` follows `exited`.
  EXPECT_NE(tang.request("continue", "{\"threadId\":1}").find("\"success\":true"), std::string::npos);
  std::string exited = tang.event("exited");
  EXPECT_NE(exited.find("\"exitCode\":0"), std::string::npos) << exited;
  EXPECT_FALSE(tang.event("terminated").empty());
  EXPECT_NE(tang.request("disconnect").find("\"success\":true"), std::string::npos);
  Result result = tang.finish();
  EXPECT_EQ(result.status, 0);
  EXPECT_EQ(result.out, "") << "nothing but the protocol is written to stdout";
  EXPECT_EQ(result.err, "3") << "the run's output is on stderr";
}

TEST(TangDap, TheOutputOnStderrIsWhatThePlainRunPrintsOnStdoutWithTheSameStatus) {
  TempDir dir;
  std::string file = dir.write("loop.tang", kLoop);
  Result plain = run_plain({"--script", file});
  ASSERT_EQ(plain.status, 0);
  ASSERT_EQ(plain.out, "0,1,3,done");

  // 1. Attached, nothing set: it runs to the end.
  {
    Tang tang({"--script", "--dap", file});
    tang.configure(file, {});
    EXPECT_FALSE(tang.event("terminated").empty());
    tang.request("disconnect");
    Result r = tang.finish();
    EXPECT_EQ(r.err, plain.out);
    EXPECT_EQ(r.status, plain.status);
  }
  // 2. A breakpoint on a line that is never reached (inside a branch not taken).
  {
    Tang tang({"--script", "--dap", file});
    tang.configure(file, {7});
    EXPECT_FALSE(tang.event("terminated").empty()) << "the breakpoint is never hit, so the first thing the client hears is the end";
    tang.request("disconnect");
    Result r = tang.finish();
    EXPECT_EQ(r.err, plain.out);
    EXPECT_EQ(r.status, plain.status);
  }
  // 3. A breakpoint hit three times, each stop continued.
  {
    Tang tang({"--script", "--dap", file});
    tang.configure(file, {3});
    for (int hit = 0; hit < 3; ++hit) {
      EXPECT_FALSE(tang.event("stopped").empty()) << "hit " << hit;
      Tang::Where at = tang.where();
      EXPECT_EQ(at.line, 3);
      EXPECT_NE(tang.locals(at.id).find("\"name\":\"i\",\"value\":\"" + std::to_string(hit) + "\""), std::string::npos);
      tang.request("continue", "{\"threadId\":1}");
    }
    EXPECT_FALSE(tang.event("terminated").empty());
    tang.request("disconnect");
    Result r = tang.finish();
    EXPECT_EQ(r.err, plain.out);
    EXPECT_EQ(r.status, plain.status);
  }
  // 4. The breakpoint names a line in another file: the run is not stopped.
  {
    Tang tang({"--script", "--dap", file});
    tang.configure(dir.path + "/other.tang", {3});
    EXPECT_FALSE(tang.event("terminated").empty());
    tang.request("disconnect");
    Result r = tang.finish();
    EXPECT_EQ(r.err, plain.out);
    EXPECT_EQ(r.status, plain.status);
  }
  // 5. A template, not a script: the same equality for the other mode.
  {
    std::string page = dir.write("page.tang", "<p><%= 1 + 1 %></p>\n<% x = 3; %>\n<b><%= x %></b>\n");
    Result expected = run_plain({page});
    Tang tang({"--dap", page});
    tang.configure(page, {2});
    // A tag and the text after it on its line are two statements on one line,
    // so a breakpoint on that line stops at each (design.md, granularity):
    // continue until the run ends.
    int stops = 0;
    for (;;) {
      std::string next = tang.next_stop_or_end();
      if (next != "stopped") {
        EXPECT_EQ(next, "terminated");
        break;
      }
      EXPECT_EQ(tang.where().line, 2);
      ++stops;
      ASSERT_LT(stops, 10);
      tang.request("continue", "{\"threadId\":1}");
    }
    EXPECT_GE(stops, 1);
    tang.request("disconnect");
    Result r = tang.finish();
    EXPECT_EQ(r.err, expected.out);
    EXPECT_EQ(r.status, expected.status);
  }
}

TEST(TangDap, ABreakpointOnTheFirstLineOfAMultiLineStatementFiresThere) {
  TempDir dir;
  std::string file = dir.write("multi.tang", "x = 1;\ny = [\n  1,\n  2,\n  3];\nprint(y);\n");
  Tang tang({"--script", "--dap", file});
  tang.configure(file, {2, 4});  // line 4 is inside the statement: it never fires
  EXPECT_FALSE(tang.event("stopped").empty());
  EXPECT_EQ(tang.where().line, 2);
  tang.request("continue", "{\"threadId\":1}");
  EXPECT_FALSE(tang.event("terminated").empty()) << "the line inside the statement is never a stop";
  tang.request("disconnect");
  EXPECT_EQ(tang.finish().status, 0);
}

TEST(TangDap, ARunawayUnderAFuelBudgetStopsWithPauseNamingFuelAndTheLineAndContinueEndsIt) {
  TempDir dir;
  std::string file = dir.write("runaway.tang", "x = 1;\nwhile (true) {}\n");
  Tang tang({"--script", "--dap", "--fuel", "1000", file});
  tang.configure(file, {});
  std::string stopped = tang.event("stopped");
  EXPECT_NE(stopped.find("\"reason\":\"pause\""), std::string::npos) << stopped;
  EXPECT_NE(stopped.find("fuel"), std::string::npos) << stopped;
  std::string trace = tang.request("stackTrace", "{\"threadId\":1}");
  EXPECT_NE(trace.find(file), std::string::npos) << trace;
  EXPECT_EQ(number_after(trace, "\"line\":"), 2);
  // The command has no policy for raising a budget: the next continue unwinds.
  EXPECT_NE(tang.request("continue", "{\"threadId\":1}").find("\"success\":true"), std::string::npos);
  std::string exited = tang.event("exited");
  EXPECT_NE(exited.find("\"exitCode\":6"), std::string::npos) << exited;
  EXPECT_FALSE(tang.event("terminated").empty());
  tang.request("disconnect");
  Result result = tang.finish();
  EXPECT_EQ(result.status, 6);
  EXPECT_NE(result.err.find("stopped"), std::string::npos) << result.err;
}

TEST(TangDap, TerminateEndsTheRunWithStatusSixAndDisconnectWithoutTerminateRunsFree) {
  TempDir dir;
  std::string file = dir.write("loop.tang", kLoop);
  {
    Tang tang({"--script", "--dap", file});
    tang.configure(file, {3});
    EXPECT_FALSE(tang.event("stopped").empty());
    EXPECT_NE(tang.request("terminate").find("\"success\":true"), std::string::npos);
    EXPECT_FALSE(tang.event("terminated").empty());
    Result r = tang.finish();
    EXPECT_EQ(r.status, 6);
  }
  {
    // A client that disconnects at a stop: the debugger is disarmed and the
    // run finishes, with the output the plain run has.
    Tang tang({"--script", "--dap", file});
    tang.configure(file, {3});
    EXPECT_FALSE(tang.event("stopped").empty());
    EXPECT_NE(tang.request("disconnect", "{\"terminateDebuggee\":false}").find("\"success\":true"), std::string::npos);
    Result r = tang.finish();
    EXPECT_EQ(r.status, 0);
    EXPECT_EQ(r.err, "0,1,3,done");
  }
  {
    // A client that just goes away (stdin closed at a stop) is a detach too.
    Tang tang({"--script", "--dap", file});
    tang.configure(file, {3});
    EXPECT_FALSE(tang.event("stopped").empty());
    Result r = tang.finish();
    EXPECT_EQ(r.status, 0);
    EXPECT_EQ(r.err, "0,1,3,done");
  }
}

TEST(TangDap, TheSourceNameIsTheFileNameAsGivenOnTheCommandLine) {
  TempDir dir;
  std::string file = dir.write("named.tang", "x = 1;\ny = 2;\nprint(x + y);\n");
  // The breakpoint is set against the string passed to the command; a different
  // spelling of the same file (a `./` in front) is a different source.
  Tang tang({"--script", "--dap", file});
  tang.configure(dir.path + "/./named.tang", {2});
  EXPECT_FALSE(tang.event("terminated").empty()) << "a differently spelled path is not the same source";
  tang.request("disconnect");
  EXPECT_EQ(tang.finish().err, "3");
}

// The VS Code contribution (editors/vscode) cannot be started from a test. What
// can be: read the command lines its extension would hand VS Code as the debug
// adapter (one for a template, one for a script) and drive each against the real
// command, so that a command line the command does not accept fails here. The
// manifest's structure is `make check-vscode`'s, not this test's: all it reads
// from the manifest is that a tang debugger and its `script` attribute are named.
// The command name "tang" is the command under test (tang_path()), as it is on
// the PATH of a VS Code that has one.

/// Each `DebugAdapterExecutable("command", [arguments])` of the extension: the command and the text of the argument list.
/// Read by hand, not with std::regex: GCC 14 and 16 report libstdc++'s regex state copy as maybe-uninitialized
/// under ASan, which failed `make test-asan` at the baseline of story 9 as well.
static std::vector<std::pair<std::string, std::string>> adapter_calls(const std::string & text) {
  static const std::string kName = "DebugAdapterExecutable(";
  std::vector<std::pair<std::string, std::string>> calls;
  auto skip_space = [&](size_t at) {
    while (at < text.size() && std::isspace(static_cast<unsigned char>(text[at]))) {
      ++at;
    }
    return at;
  };
  for (size_t at = text.find(kName); at != std::string::npos; at = text.find(kName, at)) {
    size_t i = skip_space(at + kName.size());
    at += kName.size();
    if (i >= text.size() || text[i] != '"') {
      continue;
    }
    size_t close = text.find('"', i + 1);
    if (close == std::string::npos || close == i + 1) {
      continue;
    }
    const std::string command = text.substr(i + 1, close - i - 1);
    i = skip_space(close + 1);
    if (i >= text.size() || text[i] != ',') {
      continue;
    }
    i = skip_space(i + 1);
    if (i >= text.size() || text[i] != '[') {
      continue;
    }
    size_t end = text.find(']', i + 1);
    if (end == std::string::npos) {
      continue;
    }
    size_t after = skip_space(end + 1);
    if (after >= text.size() || text[after] != ')') {
      continue;
    }
    calls.emplace_back(command, text.substr(i + 1, end - i - 1));
    at = after;
  }
  return calls;
}

TEST(TangDap, TheVsCodeExtensionsCommandLinesStartTheRealCommandForATemplateAndAScript) {
#ifndef GLTANG_TEST_DATA
#error "GLTANG_TEST_DATA must name the tests/ directory; the Makefile defines it"
#endif
  const std::string dir = std::string(GLTANG_TEST_DATA) + "/../editors/vscode";
  auto read_file = [](const std::string & path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
  };
  const std::string manifest = read_file(dir + "/package.json");
  ASSERT_FALSE(manifest.empty()) << "no manifest at " << dir;
  EXPECT_NE(manifest.find("\"type\": \"tang\""), std::string::npos);
  EXPECT_NE(manifest.find("\"script\""), std::string::npos) << "the launch configuration has no script attribute";

  const std::string extension = read_file(dir + "/extension.js");
  const auto calls = adapter_calls(extension);
  struct Arm {
    bool script;
    std::vector<std::string> arguments;  // with `file` as the empty string
  };
  std::vector<Arm> arms;
  for (const auto & call : calls) {
    EXPECT_EQ(call.first, "tang") << "the adapter is the tang command";
    Arm arm{false, {}};
    std::stringstream list(call.second);
    std::string item;
    while (std::getline(list, item, ',')) {
      size_t first = item.find_first_not_of(" \t\r\n");
      size_t last = item.find_last_not_of(" \t\r\n");
      if (first == std::string::npos) {
        continue;
      }
      item = item.substr(first, last - first + 1);
      if (item == "file") {
        arm.arguments.push_back("");  // session.configuration.program, which VS Code fills with the file
      } else {
        ASSERT_GE(item.size(), 2u) << item;
        ASSERT_EQ(item.front(), '"') << "an argument that is neither a string literal nor `file`: " << item;
        ASSERT_EQ(item.back(), '"') << item;
        arm.arguments.push_back(item.substr(1, item.size() - 2));
        arm.script = arm.script || arm.arguments.back() == "--script";
      }
    }
    arms.push_back(arm);
  }
  ASSERT_EQ(arms.size(), 2u) << "extension.js has not two DebugAdapterExecutable calls, one for a template and one for a script";
  EXPECT_NE(arms[0].script, arms[1].script) << "one call is for a script and the other for a template";

  TempDir temp;
  for (const Arm & arm : arms) {
    // A script has a statement on line 1; a template has a tag on line 2, which
    // a breakpoint stops at (see the template session above).
    const std::string file = arm.script ? temp.write("vscode-script.tang", "a = 1;\nprint(a);\n")
                                        : temp.write("vscode-template.tang", "<p><%= 1 + 1 %></p>\n<% x = 3; %>\n<b><%= x %></b>\n");
    const int line = arm.script ? 1 : 2;
    std::vector<std::string> argv;
    for (const std::string & a : arm.arguments) {
      argv.push_back(a.empty() ? file : a);
    }
    EXPECT_NE(std::find(argv.begin(), argv.end(), "--dap"), argv.end());
    EXPECT_NE(std::find(argv.begin(), argv.end(), file), argv.end());

    // configure, stopped, disconnect: the session a client opens, a stop at the
    // breakpoint, and the client going away.
    Tang tang(argv);
    tang.configure(file, {line});
    std::string stopped = tang.event("stopped");
    EXPECT_NE(stopped.find("\"reason\":\"breakpoint\""), std::string::npos) << (arm.script ? "script: " : "template: ") << stopped;
    EXPECT_EQ(tang.where().line, line);
    EXPECT_NE(tang.request("disconnect").find("\"success\":true"), std::string::npos);
    Result result = tang.finish();
    EXPECT_EQ(result.out, "") << "nothing but the protocol is written to stdout";
    EXPECT_EQ(result.status, 0) << result.err;
  }
}

TEST(TangDap, MisuseIsAUsageError) {
  TempDir dir;
  std::string file = dir.write("x.tang", "print(1);\n");
  Result with_tree = run_plain({"--dap", "--tree", file});
  EXPECT_EQ(with_tree.status, 2);
  EXPECT_NE(with_tree.err.find("--dap"), std::string::npos) << with_tree.err;
  Result from_stdin = run_plain({"--dap"});
  EXPECT_EQ(from_stdin.status, 2);
  EXPECT_NE(from_stdin.err.find("--dap"), std::string::npos) << from_stdin.err;
  EXPECT_EQ(with_tree.out, "");
}

#ifdef GLTANG_WITH_JIT

namespace {

// A function that is hot and called twice, with a stop between the calls where
// the client sets a breakpoint inside it.
const char * const kHot =
    "function f(n) {\n"            // 1
    "  s = 0;\n"                   // 2
    "  i = 0;\n"                   // 3
    "  while (i < n) {\n"          // 4
    "    s = s + i;\n"             // 5
    "    i = i + 1;\n"             // 6
    "  }\n"                        // 7
    "  return s;\n"                // 8
    "}\n"                          // 9
    "print(f(3));\n"               // 10
    "print(\"mid\");\n"            // 11
    "print(f(4));\n"               // 12
    "print(\"end\");\n";           // 13

struct Played {
  std::string transcript;  // what the client saw, stop by stop
  std::string err;         // the run's output and the jit line
  int status = -1;
};

// One scripted session, the same on every tier:
//   1. Stop at the breakpoint on line 10, before the first call.
//   2. Step in: f's entry, line 1, one frame deeper.
//   3. Step over: line 2 of f.
//   4. Step out: back in the caller. Nothing else is set, so a stop here is the
//      step's alone (reason "step") and never a breakpoint's.
//   5. Set a breakpoint on line 5 inside f and continue: the second call stops
//      there (f already compiled when the JIT is on).
//   6. Step over twice and step out.
//   7. Continue until the end.
Played play_hot(const std::string & file, long jit_threshold) {
  Tang tang({"--script", "--dap", "--jit-threshold", std::to_string(jit_threshold), "--jit-stats", file});
  tang.configure(file, {10});
  Played out;
  auto stop = [&](const char * what) {
    std::string event = tang.event("stopped");
    EXPECT_FALSE(event.empty()) << what;
    Tang::Where at = tang.where();
    out.transcript += std::string(what) + ": " + (event.find("\"reason\":\"breakpoint\"") != std::string::npos ? "breakpoint" : "step") + " line " +
        std::to_string(at.line) + " frames " + std::to_string(at.frames) + " " + tang.locals(at.id) + "\n";
  };
  stop("before the first call");
  for (const char * step : {"stepIn", "next", "stepOut"}) {
    EXPECT_NE(tang.request(step, "{\"threadId\":1}").find("\"success\":true"), std::string::npos) << step;
    stop(step);
  }
  std::string reply = tang.request("setBreakpoints", "{\"source\":{\"path\":\"" + file + "\"},\"breakpoints\":[{\"line\":5}]}");
  EXPECT_NE(reply.find("\"success\":true"), std::string::npos) << reply;
  EXPECT_NE(tang.request("continue", "{\"threadId\":1}").find("\"success\":true"), std::string::npos);
  stop("inside f");
  for (const char * step : {"next", "next", "stepOut"}) {
    EXPECT_NE(tang.request(step, "{\"threadId\":1}").find("\"success\":true"), std::string::npos) << step;
    stop(step);
  }
  EXPECT_NE(tang.request("continue", "{\"threadId\":1}").find("\"success\":true"), std::string::npos);
  std::string next = tang.next_stop_or_end();
  out.transcript += "after stepping out, continue: " + next + "\n";
  if (next == "stopped") {
    Tang::Where at = tang.where();
    out.transcript += "  line " + std::to_string(at.line) + " frames " + std::to_string(at.frames) + "\n";
    reply = tang.request("setBreakpoints", "{\"source\":{\"path\":\"" + file + "\"},\"breakpoints\":[]}");
    EXPECT_NE(reply.find("\"success\":true"), std::string::npos) << reply;
    tang.request("continue", "{\"threadId\":1}");
    next = tang.next_stop_or_end();
    out.transcript += "then: " + next + "\n";
  }
  tang.request("disconnect");
  Result result = tang.finish();
  out.err = result.err;
  out.status = result.status;
  return out;
}

}  // namespace

TEST(TangDap, ABreakpointInCompiledCodeStopsAtTheRightLineWithTheRightLocalsAndStepsLikeTheInterpreter) {
  TempDir dir;
  std::string file = dir.write("hot.tang", kHot);
  Played interpreter = play_hot(file, 0);
  Played compiled = play_hot(file, 1);
  EXPECT_EQ(interpreter.status, 0);
  EXPECT_EQ(compiled.status, 0);
  // The same session, stop for stop: the lines, the frame counts and the locals.
  EXPECT_EQ(compiled.transcript, interpreter.transcript);
  // And it is the session it should be: the stop inside f is on line 5 with the
  // second call's arguments, and a step over goes to line 6.
  if (std::getenv("GLTANG_DAP_VERBOSE")) {
    std::printf("%s", compiled.transcript.c_str());
  }
  // Step in goes into f, at its entry and one frame deeper, interpreted and
  // compiled alike; a step over there is its next statement, and a step out
  // finishes the call and the print and stops, as a step (not a breakpoint), on
  // line 11 in the caller.
  EXPECT_NE(compiled.transcript.find("before the first call: breakpoint line 10 frames 1"), std::string::npos) << compiled.transcript;
  EXPECT_NE(compiled.transcript.find("stepIn: step line 1 frames 2"), std::string::npos) << compiled.transcript;
  EXPECT_NE(compiled.transcript.find("next: step line 2 frames 2"), std::string::npos) << compiled.transcript;
  EXPECT_NE(compiled.transcript.find("stepOut: step line 11 frames 1"), std::string::npos) << compiled.transcript;
  EXPECT_NE(compiled.transcript.find("inside f: breakpoint line 5 frames 2"), std::string::npos) << compiled.transcript;
  EXPECT_NE(compiled.transcript.find("\"name\":\"n\",\"value\":\"4\""), std::string::npos) << compiled.transcript;
  // The output is the plain run's, on stderr with the JIT line after it.
  EXPECT_EQ(compiled.err.substr(0, compiled.err.find("jit:")), interpreter.err.substr(0, interpreter.err.find("jit:")));
  EXPECT_EQ(interpreter.err.substr(0, interpreter.err.find("jit:")), "3mid6end");
  // The stats show compiled entries, and show them where the stops were: f was
  // entered in compiled code, and a poll inside it paused the run.
  size_t at = compiled.err.find("jit: compiled ");
  ASSERT_NE(at, std::string::npos) << compiled.err;
  EXPECT_EQ(compiled.err.find("entries 0,"), std::string::npos) << compiled.err;
  EXPECT_EQ(compiled.err.find("pauses 0,"), std::string::npos) << "a stop inside compiled code paused the run there: " << compiled.err;
  EXPECT_NE(interpreter.err.find("compiled 0, failed 0, discarded 0, entries 0,"), std::string::npos) << interpreter.err;
}

namespace {

// A chain of three calls, compiled at their first calls: the breakpoint is in the
// innermost, so a stop there has three compiled frames under it.
const char * const kChain =
    "function leaf(n) {\n"        // 1
    "  s = n * 2;\n"              // 2
    "  return s + 1;\n"           // 3
    "}\n"                         // 4
    "function mid(n) {\n"         // 5
    "  t = leaf(n);\n"            // 6
    "  return t + 1;\n"           // 7
    "}\n"                         // 8
    "function top(n) {\n"         // 9
    "  u = mid(n);\n"             // 10
    "  return u + 1;\n"           // 11
    "}\n"                         // 12
    "total = 0;\n"                // 13
    "for (k = 0; k < 6; k += 1) { total = total + top(k); }\n"  // 14
    "print(total);\n";            // 15

// The whole stack at a stop: every frame's name, line and local variables, as the
// client reads them.
std::string whole_stack(Tang & tang) {
  std::string trace = tang.request("stackTrace", "{\"threadId\":1}");
  std::string out = trace + "\n";
  size_t at = 0;
  while ((at = trace.find("\"id\":", at)) != std::string::npos) {
    long id = number_after(trace.substr(at), "\"id\":");
    out += "  frame " + std::to_string(id) + ": " + tang.locals(id) + "\n";
    at += 5;
  }
  return out;
}

std::string play_chain(const std::string & file, long jit_threshold, std::string * err) {
  Tang tang({"--script", "--dap", "--jit-threshold", std::to_string(jit_threshold), "--jit-stats", file});
  tang.configure(file, {2});
  std::string transcript;
  auto stop = [&](const char * what) {
    std::string event = tang.event("stopped");
    EXPECT_FALSE(event.empty()) << what;
    transcript += std::string(what) + ": " + (event.find("\"reason\":\"breakpoint\"") != std::string::npos ? "breakpoint" : "step") + "\n" + whole_stack(tang);
  };
  stop("inside leaf");
  // Out of leaf and out of mid: each stop is in the caller, which was a compiled
  // frame and is now an interpreter frame, on the line after its call.
  for (const char * step : {"stepOut", "stepOut"}) {
    EXPECT_NE(tang.request(step, "{\"threadId\":1}").find("\"success\":true"), std::string::npos) << step;
    stop(step);
  }
  // The next call of the chain stops at the breakpoint again, and the next one
  // finishes a step over inside the callee.
  EXPECT_NE(tang.request("continue", "{\"threadId\":1}").find("\"success\":true"), std::string::npos);
  stop("second call, inside leaf");
  EXPECT_NE(tang.request("next", "{\"threadId\":1}").find("\"success\":true"), std::string::npos);
  stop("next");
  std::string reply = tang.request("setBreakpoints", "{\"source\":{\"path\":\"" + file + "\"},\"breakpoints\":[]}");
  EXPECT_NE(reply.find("\"success\":true"), std::string::npos) << reply;
  tang.request("continue", "{\"threadId\":1}");
  transcript += "then: " + tang.next_stop_or_end() + "\n";
  tang.request("disconnect");
  Result result = tang.finish();
  EXPECT_EQ(result.status, 0) << result.err;
  *err = result.err;
  return transcript;
}

}  // namespace

TEST(TangDap, ABreakpointInACompiledCalleeShowsTheWholeChainFrameForFrameAndStepOutStopsInTheCompiledCaller) {
  TempDir dir;
  std::string file = dir.write("chain.tang", kChain);
  std::string interpreter_err, compiled_err;
  std::string interpreter = play_chain(file, 0, &interpreter_err);
  std::string compiled = play_chain(file, 1, &compiled_err);
  // The same session, stop for stop and frame for frame: names, lines and locals.
  EXPECT_EQ(compiled, interpreter);
  if (std::getenv("GLTANG_DAP_VERBOSE")) {
    std::printf("%s", compiled.c_str());
  }
  // It is the stack it should be: leaf, mid, top and the program, four frames
  // deep, with the locals of each; and a step out lands in the caller.
  EXPECT_NE(compiled.find("inside leaf: breakpoint"), std::string::npos) << compiled;
  EXPECT_NE(compiled.find("\"totalFrames\":4"), std::string::npos) << compiled;
  EXPECT_NE(compiled.find("\"name\":\"n\",\"value\":\"0\""), std::string::npos) << compiled;
  EXPECT_NE(compiled.find("stepOut: step"), std::string::npos) << compiled;
  EXPECT_NE(compiled.find("\"totalFrames\":3"), std::string::npos) << "a step out of leaf stops in mid, one frame less: " << compiled;
  EXPECT_NE(compiled.find("\"totalFrames\":2"), std::string::npos) << "and a second one in top: " << compiled;
  // The compiled run's chain was really compiled, and a stop inside it was a pause.
  EXPECT_EQ(compiled_err.find("calls 0,"), std::string::npos) << compiled_err;
  EXPECT_EQ(compiled_err.find("pauses 0,"), std::string::npos) << compiled_err;
  EXPECT_EQ(compiled_err.substr(0, compiled_err.find("jit:")), interpreter_err.substr(0, interpreter_err.find("jit:")));
}

#endif  // GLTANG_WITH_JIT

#endif  // _WIN32

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

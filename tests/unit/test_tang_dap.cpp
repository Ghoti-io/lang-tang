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

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
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

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

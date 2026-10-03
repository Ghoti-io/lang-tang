/**
 * @file
 *
 * The oracle differential's pieces, as plain C++ so that a planted case can
 * drive each one: a verdict and the comparison between two of them, the
 * child-process driver with its wall-clock kill, the divergence ledger and its
 * validator, and the judge that holds the comparison against the ledger.
 *
 * Nothing here includes ctang. The program that does is oracle_ctang.c, which
 * the driver runs as a child.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GLTANG_TESTS_ORACLE_H
#define GHOTI_IO_GLTANG_TESTS_ORACLE_H

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace oracle {

// -------------------------------------------------------------------------
// Verdicts
// -------------------------------------------------------------------------

// accept(n) and reject are what both sides can say. killed is ctang's alone
// (the harness killed it, or it crashed), paused is lang-tang's alone and is
// produced from the story that adds execution on; nothing makes one yet.
enum class Kind { Accept, Reject, Killed, Paused };

struct Verdict {
  Kind kind = Kind::Reject;
  size_t nodes = 0; // Accept only

  static Verdict accept(size_t n) { return {Kind::Accept, n}; }
  static Verdict reject() { return {Kind::Reject, 0}; }
  static Verdict killed() { return {Kind::Killed, 0}; }
  static Verdict paused() { return {Kind::Paused, 0}; }

  std::string str() const {
    switch (kind) {
      case Kind::Accept: return "accept(" + std::to_string(nodes) + ")";
      case Kind::Reject: return "reject";
      case Kind::Killed: return "killed";
      case Kind::Paused: return "paused";
    }
    return "?";
  }
};

// Equal verdicts agree. killed agrees only with paused: "lang-tang paused on
// its budget" and "ctang killed by the harness" are the same observation
// from two engines (AD-16). This is the whole rule, and a pure function so a
// planted case can drive it.
inline bool agree(const Verdict & lang_tang, const Verdict & ctang) {
  if (ctang.kind == Kind::Killed) {
    return lang_tang.kind == Kind::Paused;
  }
  if (lang_tang.kind == Kind::Paused) {
    return false;
  }
  if (lang_tang.kind != ctang.kind) {
    return false;
  }
  return lang_tang.kind != Kind::Accept || lang_tang.nodes == ctang.nodes;
}

// -------------------------------------------------------------------------
// The child-process driver
// -------------------------------------------------------------------------

struct ChildResult {
  bool timed_out = false;
  bool signaled = false;
  int signal_number = 0;
  int exit_code = 0;
  std::string output; // stdout
  double seconds = 0;
};

// Runs argv with stdout captured and stderr discarded. If the child has not
// finished within timeout_ms it is killed with SIGKILL and reaped. A child
// that crashes or hangs is a result, not a failure of the caller.
inline ChildResult run_child(const std::vector<std::string> & argv, int timeout_ms) {
  int pipefd[2];
  if (pipe(pipefd) != 0) {
    throw std::runtime_error("pipe failed");
  }
  auto start = std::chrono::steady_clock::now();
  pid_t pid = fork();
  if (pid < 0) {
    close(pipefd[0]);
    close(pipefd[1]);
    throw std::runtime_error("fork failed");
  }
  if (pid == 0) {
    dup2(pipefd[1], 1);
    close(pipefd[0]);
    close(pipefd[1]);
    int null_fd = open("/dev/null", O_WRONLY);
    if (null_fd >= 0) {
      dup2(null_fd, 2);
      close(null_fd);
    }
    // The sanitizer runtimes are preloaded for the test binaries that use
    // them, and a preloaded runtime would also instrument the child, turning
    // its crash into a sanitizer report and an ordinary exit. The child is the
    // reference or a stand-in for it, and must die the way it dies.
    unsetenv("LD_PRELOAD");
    std::vector<char *> args;
    for (const auto & a : argv) {
      args.push_back(const_cast<char *>(a.c_str()));
    }
    args.push_back(nullptr);
    execv(args[0], args.data());
    _exit(127);
  }
  close(pipefd[1]);

  ChildResult result;
  auto deadline = start + std::chrono::milliseconds(timeout_ms);
  bool open_pipe = true;
  while (open_pipe) {
    auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      result.timed_out = true;
      kill(pid, SIGKILL);
      break;
    }
    int wait_ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
    struct pollfd p = {pipefd[0], POLLIN, 0};
    int ready = poll(&p, 1, wait_ms);
    if (ready < 0 && errno == EINTR) {
      continue;
    }
    if (ready > 0) {
      char chunk[4096];
      ssize_t got = read(pipefd[0], chunk, sizeof(chunk));
      if (got > 0) {
        result.output.append(chunk, (size_t)got);
      }
      else if (got == 0) {
        open_pipe = false;
      }
    }
  }
  close(pipefd[0]);

  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  if (WIFSIGNALED(status)) {
    result.signaled = true;
    result.signal_number = WTERMSIG(status);
  }
  else if (WIFEXITED(status)) {
    result.exit_code = WEXITSTATUS(status);
  }
  result.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  return result;
}

// ctang's verdict for a file, through the runner. A signal or a timeout is
// `killed`; the runner's own usage and read failures (exit 2 and 3) are a
// broken harness and throw, so they cannot pass as a verdict.
inline Verdict ctang_verdict(const std::string & runner, const std::string & mode, const std::string & file, int timeout_ms) {
  ChildResult r = run_child({runner, mode, file}, timeout_ms);
  if (r.timed_out || r.signaled) {
    return Verdict::killed();
  }
  if (r.exit_code != 0) {
    throw std::runtime_error("the oracle runner failed on " + file + " (exit " + std::to_string(r.exit_code) + ")");
  }
  if (r.output == "error\n") {
    return Verdict::reject();
  }
  unsigned long long n = 0;
  if (std::sscanf(r.output.c_str(), "ok %llu", &n) == 1) {
    return Verdict::accept((size_t)n);
  }
  throw std::runtime_error("the oracle runner printed something unreadable for " + file + ": " + r.output);
}

// -------------------------------------------------------------------------
// The divergence ledger
// -------------------------------------------------------------------------

struct Row {
  std::string id, category, status, ref, resolved_by, summary;
  std::vector<std::string> corpus; // relative to the corpus root; empty for "-"
  int line = 0;
};

struct Ledger {
  std::vector<Row> rows;
  std::vector<std::string> errors; // why the table is malformed; empty if valid

  bool valid() const { return errors.empty(); }
  // Closed: nothing is left open. ctang retires when this is true (AD-3).
  bool closed() const {
    return std::none_of(rows.begin(), rows.end(), [](const Row & r) { return r.status == "open"; });
  }
  const Row * recorded_row_for(const std::string & file) const {
    for (const auto & r : rows) {
      if (r.status == "recorded" && std::find(r.corpus.begin(), r.corpus.end(), file) != r.corpus.end()) {
        return &r;
      }
    }
    return nullptr;
  }
};

inline std::string trim(const std::string & s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) {
    return "";
  }
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

inline bool file_exists(const std::string & path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

// Parses the ledger's table out of the markdown text. Every line that starts
// with `|` belongs to the table; the header row and the separator row are
// recognised and skipped. A malformed row is an error naming its line, and
// does not stop the others being read, so one run reports every defect.
inline Ledger parse_ledger(const std::string & text, const std::string & corpus_root) {
  static const std::set<std::string> categories = {"ctang-defect", "limit", "error-reporting", "rng"};
  static const std::set<std::string> statuses = {"open", "recorded", "fixed"};
  Ledger ledger;
  std::istringstream in(text);
  std::string line;
  int number = 0;
  bool seen_header = false;
  std::set<std::string> ids;
  while (std::getline(in, line)) {
    number++;
    std::string t = trim(line);
    if (t.empty() || t[0] != '|') {
      continue;
    }
    std::vector<std::string> cells;
    std::string cell;
    std::string body = t.substr(1);
    if (!body.empty() && body.back() == '|') {
      body.pop_back();
    }
    std::istringstream cs(body);
    while (std::getline(cs, cell, '|')) {
      cells.push_back(trim(cell));
    }
    auto err = [&](const std::string & m) {
      ledger.errors.push_back("line " + std::to_string(number) + ": " + m);
    };
    if (!seen_header) {
      if (cells.size() >= 1 && cells[0] == "id") {
        seen_header = true;
        const std::vector<std::string> want = {"id", "category", "status", "ref", "corpus", "resolved-by", "summary"};
        if (cells != want) {
          err("the header row is not id | category | status | ref | corpus | resolved-by | summary");
        }
      }
      continue;
    }
    if (!cells.empty() && cells[0].find_first_not_of("-: ") == std::string::npos) {
      continue; // the separator row
    }
    if (cells.size() != 7) {
      err("a row has " + std::to_string(cells.size()) + " cells, not 7");
      continue;
    }
    Row r;
    r.line = number;
    r.id = cells[0];
    r.category = cells[1];
    r.status = cells[2];
    r.ref = cells[3];
    r.resolved_by = cells[5];
    r.summary = cells[6];
    bool id_ok = r.id.size() == 5 && r.id.compare(0, 2, "D-") == 0 &&
        std::all_of(r.id.begin() + 2, r.id.end(), [](unsigned char c) { return std::isdigit(c); });
    if (!id_ok) {
      err("id '" + r.id + "' is not D-nnn");
    }
    else if (!ids.insert(r.id).second) {
      err("id " + r.id + " is not unique");
    }
    if (!categories.count(r.category)) {
      err(r.id + ": category '" + r.category + "' is not one of ctang-defect, limit, error-reporting, rng");
    }
    if (!statuses.count(r.status)) {
      err(r.id + ": status '" + r.status + "' is not one of open, recorded, fixed");
    }
    if (r.ref != "-" && (r.ref.empty() || !std::all_of(r.ref.begin(), r.ref.end(), [](unsigned char c) { return std::isdigit(c); }))) {
      err(r.id + ": ref '" + r.ref + "' is neither a section 13 number nor -");
    }
    bool resolved_ok = r.resolved_by == "-" || (!r.resolved_by.empty() &&
        std::all_of(r.resolved_by.begin(), r.resolved_by.end(), [](unsigned char c) { return std::isdigit(c) || c == ',' || c == ' '; }));
    if (!resolved_ok && !r.resolved_by.empty()) {
      err(r.id + ": resolved-by '" + r.resolved_by + "' is not a story number");
    }
    if (r.resolved_by.empty()) {
      err(r.id + ": resolved-by is empty");
    }
    if (r.summary.empty()) {
      err(r.id + ": the summary is empty");
    }
    if (cells[4].empty()) {
      err(r.id + ": corpus is empty (use - for none)");
    }
    else if (cells[4] != "-") {
      std::istringstream fs(cells[4]);
      std::string f;
      while (std::getline(fs, f, ',')) {
        f = trim(f);
        if (f.empty() || !file_exists(corpus_root + "/" + f)) {
          err(r.id + ": corpus file '" + f + "' does not exist under " + corpus_root);
        }
        else {
          r.corpus.push_back(f);
        }
      }
    }
    ledger.rows.push_back(r);
  }
  if (!seen_header) {
    ledger.errors.push_back("no table: the ledger has no header row starting with | id");
  }
  else if (ledger.rows.empty()) {
    ledger.errors.push_back("the table has no rows; a ledger of zero rows is a ledger that was not read");
  }
  return ledger;
}

inline std::string read_text_file(const std::string & path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    throw std::runtime_error("cannot read " + path);
  }
  std::ostringstream s;
  s << f.rdbuf();
  return s.str();
}

// -------------------------------------------------------------------------
// The judge
// -------------------------------------------------------------------------

struct Entry {
  std::string file; // relative to the corpus root, e.g. script/if.tang
  Verdict lang_tang;
  Verdict ctang;
};

struct Judgement {
  std::vector<std::string> failures;   // what makes the differential fail
  std::vector<std::string> recorded;   // divergences a ledger row accepts
  size_t agreed = 0;
  bool ok() const { return failures.empty(); }
};

// A divergence is accepted only if a `recorded` row names the file. A
// `recorded` row naming a file that agrees is stale, and fails: otherwise a
// ledger would keep excusing a divergence long after it was fixed, and the
// ledger could never close.
inline Judgement judge(const std::vector<Entry> & entries, const Ledger & ledger) {
  Judgement j;
  for (const auto & e : entries) {
    const Row * row = ledger.recorded_row_for(e.file);
    if (agree(e.lang_tang, e.ctang)) {
      j.agreed++;
      if (row) {
        j.failures.push_back("stale ledger row " + row->id + ": " + e.file + " no longer diverges (both say " + e.ctang.str() + ")");
      }
    }
    else if (row) {
      j.recorded.push_back(e.file + " (" + row->id + "): lang-tang " + e.lang_tang.str() + ", ctang " + e.ctang.str());
    }
    else {
      j.failures.push_back("unrecorded divergence: " + e.file + ": lang-tang " + e.lang_tang.str() + ", ctang " + e.ctang.str());
    }
  }
  return j;
}

// Every .tang file under <root>/script and <root>/template, as "dir/name".
inline std::vector<std::string> list_corpus(const std::string & root) {
  std::vector<std::string> files;
  for (const char * dir : {"script", "template"}) {
    std::string path = root + "/" + dir;
    std::string cmd = "ls " + path + " 2>/dev/null";
    FILE * p = popen(cmd.c_str(), "r");
    if (!p) {
      continue;
    }
    char name[1024];
    while (fgets(name, sizeof(name), p)) {
      std::string n = trim(name);
      if (n.size() > 5 && n.compare(n.size() - 5, 5, ".tang") == 0) {
        files.push_back(std::string(dir) + "/" + n);
      }
    }
    pclose(p);
  }
  std::sort(files.begin(), files.end());
  return files;
}

} // namespace oracle

#endif

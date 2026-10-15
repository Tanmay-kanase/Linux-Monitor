#pragma once

#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "monitor/event.hpp"
#include "monitor/event_queue.hpp"

namespace testutil {

namespace fs = std::filesystem;
using monitor::Event;
using monitor::EventQueue;

// Creates a unique directory under /tmp and removes it (recursively) on destruction.
class TempDir {
public:
    TempDir() {
        char tmpl[] = "/tmp/monitor_test_XXXXXX";
        if (!::mkdtemp(tmpl)) throw std::runtime_error("mkdtemp failed");
        path_ = tmpl;
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const fs::path& path() const { return path_; }
    std::string str() const { return path_.string(); }

private:
    fs::path path_;
};

inline void write_file(const fs::path& path, const std::string& content) {
    std::ofstream out(path, std::ios::trunc);
    out << content;
}

inline void append_file(const fs::path& path, const std::string& content) {
    std::ofstream out(path, std::ios::app);
    out << content;
}

// Pops events until one satisfies `pred`, or the timeout expires.
inline std::optional<Event> wait_for_event(EventQueue<Event>& queue,
                                           const std::function<bool(const Event&)>& pred,
                                           std::chrono::milliseconds timeout = std::chrono::milliseconds(3000)) {
    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + timeout;
    while (clock::now() < deadline && !queue.closed()) {
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock::now());
        auto event = queue.pop_for(std::min(remaining, std::chrono::milliseconds(100)));
        if (event && pred(*event)) return event;
    }
    return std::nullopt;
}

// Removes and returns everything currently in the queue (non-blocking).
inline std::vector<Event> drain(EventQueue<Event>& queue) {
    std::vector<Event> events;
    while (auto e = queue.pop_for(std::chrono::milliseconds(0))) events.push_back(std::move(*e));
    return events;
}

inline pid_t spawn_sleeper() {
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::execlp("sleep", "sleep", "30", static_cast<char*>(nullptr));
        ::_exit(127);
    }
    return pid;
}

inline void kill_and_reap(pid_t pid) {
    ::kill(pid, SIGKILL);
    ::waitpid(pid, nullptr, 0);
}

}  // namespace testutil

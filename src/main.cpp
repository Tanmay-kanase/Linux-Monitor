#include <pthread.h>
#include <signal.h>

#include <charconv>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "monitor/dir_watcher.hpp"
#include "monitor/event_queue.hpp"
#include "monitor/logger.hpp"
#include "monitor/proc_monitor.hpp"

namespace {

struct Config {
    std::vector<std::string> dirs;
    bool recursive = false;
    bool watch_processes = true;
    bool report_existing = false;
    long interval_ms = 500;
    long memory_threshold_kb = 0;
    std::string log_path;
};

void print_usage(const char* prog) {
    std::cout <<
        "Usage: " << prog << " [options]\n"
        "\n"
        "Monitors directories (inotify) and processes (/proc) and logs events.\n"
        "\n"
        "Options:\n"
        "  --dir PATH             Watch a directory (repeatable)\n"
        "  --recursive            Also watch sub-directories\n"
        "  --no-proc              Disable process monitoring\n"
        "  --existing             Report processes that are already running at startup\n"
        "  --interval-ms N        Process poll interval in ms (default 500)\n"
        "  --mem-threshold-kb N   Report processes whose RSS reaches N kB (default off)\n"
        "  --log FILE             Append events to FILE instead of stdout\n"
        "  -h, --help             Show this help\n"
        "\n"
        "Stop with Ctrl+C.\n";
}

std::optional<long> parse_long(const std::string& s) {
    long value = 0;
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
    if (ec != std::errc() || ptr != s.data() + s.size()) return std::nullopt;
    return value;
}

// Returns false if the program should exit immediately (exit_code says how).
bool parse_args(int argc, char** argv, Config& cfg, int& exit_code) {
    auto fail = [&](const std::string& msg) {
        std::cerr << "error: " << msg << "\n\n";
        print_usage(argv[0]);
        exit_code = 2;
        return false;
    };

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : nullptr; };

        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            exit_code = 0;
            return false;
        } else if (arg == "--dir") {
            const char* v = next();
            if (!v) return fail("--dir needs a path");
            cfg.dirs.emplace_back(v);
        } else if (arg == "--recursive") {
            cfg.recursive = true;
        } else if (arg == "--no-proc") {
            cfg.watch_processes = false;
        } else if (arg == "--existing") {
            cfg.report_existing = true;
        } else if (arg == "--interval-ms" || arg == "--mem-threshold-kb") {
            const char* v = next();
            const auto n = v ? parse_long(v) : std::nullopt;
            if (!n || *n < 0 || (arg == "--interval-ms" && *n == 0)) {
                return fail(arg + " needs a positive number");
            }
            (arg == "--interval-ms" ? cfg.interval_ms : cfg.memory_threshold_kb) = *n;
        } else if (arg == "--log") {
            const char* v = next();
            if (!v) return fail("--log needs a file name");
            cfg.log_path = v;
        } else {
            return fail("unknown option: " + arg);
        }
    }

    if (cfg.dirs.empty() && !cfg.watch_processes) {
        return fail("nothing to monitor: pass --dir and/or leave process monitoring enabled");
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Config cfg;
    int exit_code = 0;
    if (!parse_args(argc, argv, cfg, exit_code)) return exit_code;

    // Block SIGINT/SIGTERM in every thread (threads inherit the mask), then wait for
    // them synchronously on the main thread. No async-signal-safety pitfalls.
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);

    try {
        std::ofstream log_file;
        std::ostream* out = &std::cout;
        if (!cfg.log_path.empty()) {
            log_file.open(cfg.log_path, std::ios::app);
            if (!log_file) throw std::runtime_error("cannot open log file: " + cfg.log_path);
            out = &log_file;
        }
        monitor::EventQueue<monitor::Event> queue;
        monitor::EventLogger logger(queue, *out);

        std::vector<std::unique_ptr<monitor::DirWatcher>> watchers;
        for (const auto& dir : cfg.dirs) {
            monitor::DirWatcherOptions opts;
            opts.recursive = cfg.recursive;
            watchers.push_back(std::make_unique<monitor::DirWatcher>(dir, queue, opts));
        }

        std::unique_ptr<monitor::ProcessMonitor> proc_monitor;
        if (cfg.watch_processes) {
            monitor::ProcessMonitorOptions opts;
            opts.poll_interval = std::chrono::milliseconds(cfg.interval_ms);
            opts.memory_threshold_kb = cfg.memory_threshold_kb;
            opts.report_existing = cfg.report_existing;
            proc_monitor = std::make_unique<monitor::ProcessMonitor>(queue, opts);
        }

        logger.start();
        for (auto& w : watchers) w->start();
        if (proc_monitor) proc_monitor->start();

        std::cerr << "monitoring... press Ctrl+C to stop\n";
        int sig = 0;
        sigwait(&signals, &sig);
        std::cerr << "\nstopping (" << strsignal(sig) << ")\n";

        for (auto& w : watchers) w->stop();
        if (proc_monitor) proc_monitor->stop();
        logger.stop();  // drains remaining events

        std::cerr << logger.events_logged() << " events logged\n";
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << '\n';
        return 1;
    }
    return 0;
}

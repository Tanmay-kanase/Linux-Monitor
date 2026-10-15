#include <algorithm>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "monitor/proc_monitor.hpp"
#include "test_util.hpp"

using namespace monitor;
using namespace testutil;
using namespace std::chrono_literals;

namespace {

// Builds a fake /proc/<pid>/{stat,status} so tests are deterministic.
void make_proc(const fs::path& root, pid_t pid, const std::string& name,
               unsigned long long start_time, long rss_kb) {
    const auto dir = root / std::to_string(pid);
    fs::create_directories(dir);
    write_file(dir / "stat",
               std::to_string(pid) + " (" + name + ") S 1 1 1 0 -1 4194304 0 0 0 0 0 0 0 0 20 0 1 0 " +
                   std::to_string(start_time) + " 1000 100 18446744073709551615\n");
    write_file(dir / "status", "Name:\t" + name + "\nVmRSS:\t" + std::to_string(rss_kb) + " kB\n");
}

void remove_proc(const fs::path& root, pid_t pid) { fs::remove_all(root / std::to_string(pid)); }

ProcessMonitorOptions fake_options(const TempDir& root) {
    ProcessMonitorOptions o;
    o.proc_root = root.str();
    return o;
}

std::vector<Event> of_type(const std::vector<Event>& events, EventType type) {
    std::vector<Event> out;
    std::copy_if(events.begin(), events.end(), std::back_inserter(out),
                 [&](const Event& e) { return e.type == type; });
    return out;
}

}  // namespace

TEST(ProcessMonitor, ExistingProcessesAreSilentByDefault) {
    TempDir root;
    make_proc(root.path(), 100, "init", 1, 1000);
    EventQueue<Event> q;
    ProcessMonitor m(q, fake_options(root));

    m.poll_once();
    EXPECT_TRUE(drain(q).empty());
}

TEST(ProcessMonitor, ReportExistingAnnouncesRunningProcessesOnFirstPoll) {
    TempDir root;
    make_proc(root.path(), 100, "init", 1, 1000);
    EventQueue<Event> q;
    auto opts = fake_options(root);
    opts.report_existing = true;
    ProcessMonitor m(q, opts);

    m.poll_once();
    const auto events = drain(q);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].type, EventType::ProcessStarted);
    EXPECT_EQ(events[0].pid, 100);
}

TEST(ProcessMonitor, DetectsStartWithNameAndMemory) {
    TempDir root;
    EventQueue<Event> q;
    ProcessMonitor m(q, fake_options(root));
    m.poll_once();  // baseline: nothing running

    make_proc(root.path(), 200, "my app", 50, 4096);
    m.poll_once();

    const auto events = drain(q);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].type, EventType::ProcessStarted);
    EXPECT_EQ(events[0].pid, 200);
    EXPECT_EQ(events[0].subject, "my app");
    EXPECT_EQ(events[0].rss_kb, 4096);
}

TEST(ProcessMonitor, DetectsExitWithLastKnownMemory) {
    TempDir root;
    EventQueue<Event> q;
    ProcessMonitor m(q, fake_options(root));
    m.poll_once();
    make_proc(root.path(), 200, "app", 50, 4096);
    m.poll_once();
    drain(q);

    make_proc(root.path(), 200, "app", 50, 8192);  // memory grew...
    m.poll_once();
    EXPECT_TRUE(drain(q).empty());                 // ...but no event without a threshold

    remove_proc(root.path(), 200);
    m.poll_once();
    const auto events = drain(q);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].type, EventType::ProcessExited);
    EXPECT_EQ(events[0].pid, 200);
    EXPECT_EQ(events[0].rss_kb, 8192);
}

TEST(ProcessMonitor, NoDuplicateEventsWhileProcessKeepsRunning) {
    TempDir root;
    EventQueue<Event> q;
    ProcessMonitor m(q, fake_options(root));
    m.poll_once();
    make_proc(root.path(), 300, "svc", 7, 100);
    m.poll_once();
    drain(q);

    m.poll_once();
    m.poll_once();
    EXPECT_TRUE(drain(q).empty());
}

TEST(ProcessMonitor, RecycledPidIsExitThenStart) {
    TempDir root;
    EventQueue<Event> q;
    ProcessMonitor m(q, fake_options(root));
    m.poll_once();
    make_proc(root.path(), 400, "first", 10, 100);
    m.poll_once();
    drain(q);

    make_proc(root.path(), 400, "second", 99, 200);  // same pid, different start time
    m.poll_once();

    const auto events = drain(q);
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0].type, EventType::ProcessExited);
    EXPECT_EQ(events[0].subject, "first");
    EXPECT_EQ(events[1].type, EventType::ProcessStarted);
    EXPECT_EQ(events[1].subject, "second");
}

TEST(ProcessMonitor, MemoryThresholdFiresOncePerCrossing) {
    TempDir root;
    EventQueue<Event> q;
    auto opts = fake_options(root);
    opts.memory_threshold_kb = 1000;
    ProcessMonitor m(q, opts);
    m.poll_once();

    make_proc(root.path(), 500, "leaky", 1, 100);
    m.poll_once();
    EXPECT_TRUE(of_type(drain(q), EventType::ProcessMemoryHigh).empty());

    make_proc(root.path(), 500, "leaky", 1, 5000);  // crosses the threshold
    m.poll_once();
    auto events = of_type(drain(q), EventType::ProcessMemoryHigh);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].rss_kb, 5000);

    make_proc(root.path(), 500, "leaky", 1, 6000);  // still high: no repeat
    m.poll_once();
    EXPECT_TRUE(of_type(drain(q), EventType::ProcessMemoryHigh).empty());

    make_proc(root.path(), 500, "leaky", 1, 100);   // drops back: re-arms
    m.poll_once();
    drain(q);
    make_proc(root.path(), 500, "leaky", 1, 9000);  // crosses again
    m.poll_once();
    EXPECT_EQ(of_type(drain(q), EventType::ProcessMemoryHigh).size(), 1u);
}

TEST(ProcessMonitor, DetectsRealChildProcessViaPolling) {
    EventQueue<Event> q;
    ProcessMonitor m(q);  // real /proc
    m.poll_once();        // baseline
    drain(q);

    const pid_t child = spawn_sleeper();
    ASSERT_TRUE(child > 0);
    m.poll_once();
    auto started = of_type(drain(q), EventType::ProcessStarted);
    const bool saw_start = std::any_of(started.begin(), started.end(),
                                       [&](const Event& e) { return e.pid == child; });

    kill_and_reap(child);
    m.poll_once();
    auto exited = of_type(drain(q), EventType::ProcessExited);
    const bool saw_exit = std::any_of(exited.begin(), exited.end(),
                                      [&](const Event& e) { return e.pid == child; });

    EXPECT_TRUE(saw_start);
    EXPECT_TRUE(saw_exit);
}

TEST(ProcessMonitor, BackgroundThreadReportsStartAndExit) {
    EventQueue<Event> q;
    ProcessMonitorOptions opts;
    opts.poll_interval = 20ms;
    ProcessMonitor m(q, opts);
    m.start();
    std::this_thread::sleep_for(150ms);  // let the baseline scan finish

    const pid_t child = spawn_sleeper();
    ASSERT_TRUE(child > 0);
    const auto started = wait_for_event(q, [&](const Event& e) {
        return e.type == EventType::ProcessStarted && e.pid == child;
    });

    kill_and_reap(child);
    const auto exited = wait_for_event(q, [&](const Event& e) {
        return e.type == EventType::ProcessExited && e.pid == child;
    });

    m.stop();
    EXPECT_TRUE(started.has_value());
    EXPECT_TRUE(exited.has_value());
}

TEST(ProcessMonitor, StopIsPromptEvenWithLongPollInterval) {
    EventQueue<Event> q;
    ProcessMonitorOptions opts;
    opts.poll_interval = 60s;
    ProcessMonitor m(q, opts);
    m.start();
    std::this_thread::sleep_for(100ms);

    const auto start = std::chrono::steady_clock::now();
    m.stop();
    m.stop();  // idempotent
    EXPECT_TRUE(std::chrono::steady_clock::now() - start < 2s);
}

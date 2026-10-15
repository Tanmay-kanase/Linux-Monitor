#include <filesystem>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

#include "monitor/dir_watcher.hpp"
#include "test_util.hpp"

using namespace monitor;
using namespace testutil;

namespace {

auto is_event(EventType type, const std::string& path) {
    return [=](const Event& e) { return e.type == type && e.subject == path; };
}

}  // namespace

TEST(DirWatcher, ThrowsIfPathIsNotADirectory) {
    EventQueue<Event> q;
    EXPECT_THROW(DirWatcher("/nonexistent/dir/for/test", q), std::runtime_error);

    TempDir dir;
    write_file(dir.path() / "file.txt", "x");
    EXPECT_THROW(DirWatcher((dir.path() / "file.txt").string(), q), std::runtime_error);
}

TEST(DirWatcher, ReportsFileCreated) {
    TempDir dir;
    EventQueue<Event> q;
    DirWatcher watcher(dir.str(), q);
    watcher.start();

    const auto file = (dir.path() / "a.txt").string();
    write_file(file, "hello");

    EXPECT_TRUE(wait_for_event(q, is_event(EventType::FileCreated, file)).has_value());
}

TEST(DirWatcher, ReportsFileModified) {
    TempDir dir;
    const auto file = (dir.path() / "a.txt").string();
    write_file(file, "v1");

    EventQueue<Event> q;
    DirWatcher watcher(dir.str(), q);
    watcher.start();

    append_file(file, "more");
    EXPECT_TRUE(wait_for_event(q, is_event(EventType::FileModified, file)).has_value());
}

TEST(DirWatcher, ReportsFileDeleted) {
    TempDir dir;
    const auto file = (dir.path() / "a.txt").string();
    write_file(file, "v1");

    EventQueue<Event> q;
    DirWatcher watcher(dir.str(), q);
    watcher.start();

    std::filesystem::remove(file);
    EXPECT_TRUE(wait_for_event(q, is_event(EventType::FileDeleted, file)).has_value());
}

TEST(DirWatcher, ReportsRenameAsMovedFromAndMovedTo) {
    TempDir dir;
    const auto from = (dir.path() / "old.txt").string();
    const auto to = (dir.path() / "new.txt").string();
    write_file(from, "x");

    EventQueue<Event> q;
    DirWatcher watcher(dir.str(), q);
    watcher.start();

    std::filesystem::rename(from, to);
    EXPECT_TRUE(wait_for_event(q, is_event(EventType::FileMovedFrom, from)).has_value());
    EXPECT_TRUE(wait_for_event(q, is_event(EventType::FileMovedTo, to)).has_value());
}

TEST(DirWatcher, TrailingSlashInRootDoesNotProduceDoubleSlash) {
    TempDir dir;
    EventQueue<Event> q;
    DirWatcher watcher(dir.str() + "/", q);
    watcher.start();

    const auto file = (dir.path() / "a.txt").string();
    write_file(file, "x");
    EXPECT_TRUE(wait_for_event(q, is_event(EventType::FileCreated, file)).has_value());
}

TEST(DirWatcher, NonRecursiveIgnoresSubdirectoryContents) {
    TempDir dir;
    EventQueue<Event> q;
    DirWatcher watcher(dir.str(), q);  // recursive = false
    watcher.start();

    const auto sub = (dir.path() / "sub").string();
    std::filesystem::create_directory(sub);
    ASSERT_TRUE(wait_for_event(q, is_event(EventType::FileCreated, sub)).has_value());

    const auto nested = sub + "/nested.txt";
    write_file(nested, "x");
    const auto sentinel = (dir.path() / "sentinel.txt").string();
    write_file(sentinel, "x");

    // inotify delivers in order: once the sentinel is seen, a nested event would
    // already have arrived if the subdirectory were being watched.
    std::vector<Event> seen;
    const auto got = wait_for_event(q, [&](const Event& e) {
        seen.push_back(e);
        return e.type == EventType::FileCreated && e.subject == sentinel;
    });
    ASSERT_TRUE(got.has_value());
    for (const auto& e : seen) EXPECT_NE(e.subject, nested);
}

TEST(DirWatcher, RecursiveWatchesExistingAndNewSubdirectories) {
    TempDir dir;
    const auto existing = dir.path() / "existing";
    std::filesystem::create_directory(existing);

    EventQueue<Event> q;
    DirWatcherOptions opts;
    opts.recursive = true;
    DirWatcher watcher(dir.str(), q, opts);
    watcher.start();

    // 1) directory that already existed when the watcher was created
    const auto in_existing = (existing / "a.txt").string();
    write_file(in_existing, "x");
    EXPECT_TRUE(wait_for_event(q, is_event(EventType::FileCreated, in_existing)).has_value());

    // 2) directory created after the watcher started
    const auto fresh = (dir.path() / "fresh").string();
    std::filesystem::create_directory(fresh);
    ASSERT_TRUE(wait_for_event(q, is_event(EventType::FileCreated, fresh)).has_value());

    const auto in_fresh = fresh + "/b.txt";
    write_file(in_fresh, "x");
    EXPECT_TRUE(wait_for_event(q, is_event(EventType::FileCreated, in_fresh)).has_value());
}

TEST(DirWatcher, StopIsPromptAndIdempotent) {
    TempDir dir;
    EventQueue<Event> q;
    DirWatcher watcher(dir.str(), q);
    watcher.start();

    const auto start = std::chrono::steady_clock::now();
    watcher.stop();
    watcher.stop();  // second call is a no-op
    const auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_TRUE(elapsed < std::chrono::seconds(1));  // woken via eventfd, not a poll timeout
}

TEST(DirWatcher, DestructorWithoutStartIsSafe) {
    TempDir dir;
    EventQueue<Event> q;
    { DirWatcher watcher(dir.str(), q); }
    SUCCEED();
}

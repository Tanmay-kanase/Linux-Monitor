#pragma once

#include <string>
#include <thread>
#include <unordered_map>

#include "monitor/event.hpp"
#include "monitor/event_queue.hpp"
#include "monitor/unique_fd.hpp"

struct inotify_event;

namespace monitor {

struct DirWatcherOptions {
    bool recursive = false;  // also watch sub-directories (including ones created later)
};

// Watches a directory with inotify and pushes file events onto an EventQueue.
//
// One background thread blocks in epoll_wait() on two descriptors:
//   * the inotify fd  -> file system events
//   * an eventfd      -> wake-up signal used by stop()
// so stop() is immediate instead of waiting for a poll timeout.
//
// The constructor sets everything up and throws (std::runtime_error /
// std::system_error) on failure; start() launches the thread.
class DirWatcher {
public:
    DirWatcher(std::string root, EventQueue<Event>& queue, DirWatcherOptions options = {});
    ~DirWatcher();

    DirWatcher(const DirWatcher&) = delete;
    DirWatcher& operator=(const DirWatcher&) = delete;

    void start();
    void stop();  // idempotent; blocks until the thread has exited

private:
    void run();
    void drain_inotify();
    void handle(const inotify_event& ev);
    bool add_watch(const std::string& dir);
    void add_subdirectories(const std::string& dir);
    void add_to_epoll(int fd);

    std::string root_;
    EventQueue<Event>& queue_;
    DirWatcherOptions options_;

    UniqueFd inotify_fd_;
    UniqueFd epoll_fd_;
    UniqueFd stop_fd_;

    // Only touched by the constructor and the watcher thread.
    std::unordered_map<int, std::string> watches_;  // watch descriptor -> directory path

    std::thread thread_;
};

}  // namespace monitor

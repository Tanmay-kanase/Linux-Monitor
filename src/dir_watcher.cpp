#include "monitor/dir_watcher.hpp"

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <system_error>

namespace fs = std::filesystem;

namespace monitor {
namespace {

constexpr uint32_t kWatchMask = IN_CREATE | IN_MODIFY | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO;

UniqueFd checked(int fd, const char* what) {
    if (fd < 0) {
        const int err = errno;
        throw std::system_error(err, std::generic_category(), what);
    }
    return UniqueFd(fd);
}

std::optional<EventType> classify(uint32_t mask) {
    if (mask & IN_CREATE) return EventType::FileCreated;
    if (mask & IN_MODIFY) return EventType::FileModified;
    if (mask & IN_DELETE) return EventType::FileDeleted;
    if (mask & IN_MOVED_FROM) return EventType::FileMovedFrom;
    if (mask & IN_MOVED_TO) return EventType::FileMovedTo;
    return std::nullopt;
}

}  // namespace

DirWatcher::DirWatcher(std::string root, EventQueue<Event>& queue, DirWatcherOptions options)
    : root_(std::move(root)), queue_(queue), options_(options) {
    std::error_code ec;
    if (!fs::is_directory(root_, ec)) {
        throw std::runtime_error("not a directory: " + root_);
    }
    while (root_.size() > 1 && root_.back() == '/') root_.pop_back();

    inotify_fd_ = checked(::inotify_init1(IN_NONBLOCK | IN_CLOEXEC), "inotify_init1");
    epoll_fd_ = checked(::epoll_create1(EPOLL_CLOEXEC), "epoll_create1");
    stop_fd_ = checked(::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK), "eventfd");

    add_to_epoll(inotify_fd_.get());
    add_to_epoll(stop_fd_.get());

    if (!add_watch(root_)) {
        const int err = errno;
        throw std::system_error(err, std::generic_category(), "inotify_add_watch " + root_);
    }
    if (options_.recursive) add_subdirectories(root_);
}

DirWatcher::~DirWatcher() { stop(); }

void DirWatcher::add_to_epoll(int fd) {
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = fd;
    if (::epoll_ctl(epoll_fd_.get(), EPOLL_CTL_ADD, fd, &ev) < 0) {
        const int err = errno;
        throw std::system_error(err, std::generic_category(), "epoll_ctl");
    }
}

bool DirWatcher::add_watch(const std::string& dir) {
    const int wd = ::inotify_add_watch(inotify_fd_.get(), dir.c_str(), kWatchMask);
    if (wd < 0) return false;
    watches_[wd] = dir;
    return true;
}

// Adds a watch for every existing sub-directory below `dir`. Failures (e.g.
// permission denied) are skipped: one unreadable folder shouldn't stop the rest.
void DirWatcher::add_subdirectories(const std::string& dir) {
    std::error_code ec;
    fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
    const fs::recursive_directory_iterator end;
    for (; !ec && it != end; it.increment(ec)) {
        std::error_code entry_ec;
        if (it->is_symlink(entry_ec) || !it->is_directory(entry_ec)) continue;
        add_watch(it->path().string());
    }
}

void DirWatcher::start() {
    if (thread_.joinable()) return;
    thread_ = std::thread(&DirWatcher::run, this);
}

void DirWatcher::stop() {
    if (!thread_.joinable()) return;
    const uint64_t one = 1;
    if (::write(stop_fd_.get(), &one, sizeof one) < 0) {
        // The eventfd is non-blocking and only ever written once; nothing sensible to do.
    }
    thread_.join();
}

void DirWatcher::run() {
    constexpr int kMaxEvents = 8;
    epoll_event events[kMaxEvents];

    for (;;) {
        const int n = ::epoll_wait(epoll_fd_.get(), events, kMaxEvents, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            return;
        }
        for (int i = 0; i < n; ++i) {
            const int fd = events[i].data.fd;
            if (fd == stop_fd_.get()) return;
            if (fd == inotify_fd_.get()) drain_inotify();
        }
    }
}

// The inotify fd is non-blocking: read until EAGAIN so we never leave events behind.
void DirWatcher::drain_inotify() {
    alignas(inotify_event) char buffer[4096];

    for (;;) {
        const ssize_t len = ::read(inotify_fd_.get(), buffer, sizeof buffer);
        if (len < 0) {
            if (errno == EINTR) continue;
            return;  // EAGAIN: drained
        }
        if (len == 0) return;

        for (char* p = buffer; p < buffer + len;) {
            const auto* ev = reinterpret_cast<const inotify_event*>(p);
            handle(*ev);
            p += sizeof(inotify_event) + ev->len;
        }
    }
}

void DirWatcher::handle(const inotify_event& ev) {
    if (ev.mask & IN_IGNORED) {  // the kernel removed this watch (directory deleted/unmounted)
        watches_.erase(ev.wd);
        return;
    }
    if (ev.mask & IN_Q_OVERFLOW) return;  // kernel queue overflowed: events were lost

    const auto it = watches_.find(ev.wd);
    if (it == watches_.end()) return;

    std::string path = it->second;  // copy: watches_ may be modified below
    if (ev.len > 0) {
        path += '/';
        path += ev.name;
    }

    const auto type = classify(ev.mask);
    if (!type) return;

    // Add the watch *before* publishing the event, so anything created inside the
    // new directory after the consumer sees this event is guaranteed to be caught.
    if (options_.recursive && (ev.mask & IN_ISDIR) &&
        (*type == EventType::FileCreated || *type == EventType::FileMovedTo)) {
        add_watch(path);
        add_subdirectories(path);
    }

    Event out;
    out.type = *type;
    out.subject = std::move(path);
    queue_.push(std::move(out));
}

}  // namespace monitor

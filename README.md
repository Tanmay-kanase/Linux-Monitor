# Linux File & Process Monitor

A small C++17 command-line tool that watches directories and running processes on Linux
and logs what happens: files created / modified / deleted / moved, and processes that
start, exit or use a lot of memory.

It is built from the Linux primitives that real monitoring agents use - `inotify`,
`epoll`, `/proc` - plus a producer-consumer pipeline so that slow logging never blocks
event capture.

```
$ ./build/monitor --dir ~/Downloads --recursive --mem-threshold-kb 500000
monitoring... press Ctrl+C to stop
2026-10-15T10:21:04.512Z FILE_CREATED /home/me/Downloads/report.pdf
2026-10-15T10:21:04.530Z FILE_MODIFIED /home/me/Downloads/report.pdf
2026-10-15T10:21:09.101Z PROCESS_STARTED pid=48213 name=firefox rss_kb=212340
2026-10-15T10:21:19.611Z PROCESS_MEMORY_HIGH pid=48213 name=firefox rss_kb=512004
2026-10-15T10:22:41.007Z PROCESS_EXITED pid=48213 name=firefox rss_kb=598112
```

## Architecture

```
  DirWatcher thread  ──┐
  (inotify + epoll)    │          ┌──────────────┐          ┌───────────────┐
                       ├─ push ─► │  EventQueue  │ ─ pop ─► │  EventLogger  │ ─► stdout / file
  ProcessMonitor thread┘          │ (mutex + CV) │          │    thread     │
  (polls /proc)                   └──────────────┘          └───────────────┘
```

| Component | File | What it does |
|-----------|------|--------------|
| `UniqueFd` | `unique_fd.hpp` | Move-only RAII wrapper that owns a file descriptor and closes it on scope exit. |
| `DirWatcher` | `dir_watcher.cpp` | One thread blocks in `epoll_wait` on the inotify fd **and** an `eventfd`, so `stop()` wakes it instantly. Optional recursive mode adds watches for new sub-directories on the fly. |
| `ProcessMonitor` | `proc_monitor.cpp` | Scans `/proc` every N ms and diffs against the previous snapshot. Processes are keyed by `(pid, start_time)` so a recycled PID is reported correctly. |
| `proc_parser` | `proc_parser.cpp` | Pure functions that parse `/proc/<pid>/stat` and `/proc/<pid>/status`. Handles process names containing spaces and `)`. |
| `EventQueue<T>` | `event_queue.hpp` | Thread-safe queue (`std::mutex` + `std::condition_variable`) with a `close()` protocol so the consumer drains everything before exiting. |
| `EventLogger` | `logger.cpp` | Consumer thread: formats events and writes them out. |

### Design decisions

* **Producer-consumer split.** Capturing events must never wait on disk or terminal I/O -
  otherwise the kernel's inotify queue can overflow and events are lost.
* **`eventfd` for shutdown.** Instead of `epoll_wait` with a timeout and a flag, `stop()`
  writes to an `eventfd` that is registered in the same epoll set. Shutdown latency is
  effectively zero and there is no polling.
* **Watch is added before the event is published.** For a new sub-directory the watcher
  calls `inotify_add_watch` first and only then pushes the "created" event, so a consumer
  that reacts to the event can safely create files inside it.
* **Testable by construction.** The `/proc` root is injectable, so process-monitor tests
  run against a fake `/proc` directory and are fully deterministic. Parsers are pure
  functions.
* **Signals handled synchronously.** `SIGINT`/`SIGTERM` are blocked in all threads and
  consumed with `sigwait` on the main thread - no async-signal-safety pitfalls.

## Build

Requires Linux, a C++17 compiler and CMake >= 3.14.

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

GoogleTest is used from the system if installed (`sudo apt install libgtest-dev`),
otherwise CMake downloads it automatically. Useful options:

```bash
cmake -S . -B build -DMONITOR_BUILD_TESTS=OFF          # tool only
cmake -S . -B build -DMONITOR_ENABLE_SANITIZERS=ON     # ASan + UBSan
```

## Usage

```
monitor [options]
  --dir PATH             Watch a directory (repeatable)
  --recursive            Also watch sub-directories
  --no-proc              Disable process monitoring
  --existing             Report processes that are already running at startup
  --interval-ms N        Process poll interval in ms (default 500)
  --mem-threshold-kb N   Report processes whose RSS reaches N kB (default off)
  --log FILE             Append events to FILE instead of stdout
```

Examples:

```bash
./build/monitor --dir /tmp/demo --recursive                  # files + processes
./build/monitor --dir /etc --no-proc --log /tmp/etc-watch.log  # files only, to a log file
./build/monitor --mem-threshold-kb 1000000                   # processes using >= ~1 GB
```

## Tests

GoogleTest suite covering: RAII fd semantics, queue ordering / shutdown / multi-producer
multi-consumer stress, `/proc` parsing edge cases, inotify events (create, modify,
delete, rename, recursive and non-recursive), process start/exit/PID-reuse/memory
threshold against a fake `/proc`, real child-process detection, and the logger.

## Known limitations

* **Polling `/proc`** can miss a process that starts and exits within one poll interval.
  Netlink's process connector would catch every event but requires root.
* **inotify limits:** watches are per-directory, bounded by
  `fs.inotify.max_user_watches`, and a kernel queue overflow drops events (it is detected
  but not yet reported).
* A directory that is *moved* inside a recursive watch keeps its old path in the
  watch table.
* Memory is reported as resident set size (`VmRSS`) in kB.

## Possible extensions

Bounded queue with back-pressure, JSON output, `fanotify` for mount-wide watching,
config file, per-process CPU usage from `utime`/`stime`.

## License

MIT

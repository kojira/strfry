#pragma once

#include <string.h>
#include <errno.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/inotify.h>
#include <sys/eventfd.h>
#include <poll.h>
#else
#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>
#include <fcntl.h>
#endif

#include <string>
#include <thread>
#include <functional>

#include "hoytech/error.h"
#include "hoytech/time.h"



namespace hoytech {


#if defined(__linux__)

// ---- Linux implementation (inotify) ----

class file_change_monitor {
  private:
    std::thread t;
    uint64_t debounce_us = 50 * 1000;
    int inotify_fd = -1;
    int inotify_wd = -1;
    int eventfd_fd = -1;
    bool shutdown = false;

    void cleanup() {
        if (eventfd_fd != -1) {
            ::close(eventfd_fd);
            eventfd_fd = -1;
        }

        if (inotify_wd != -1) {
            ::inotify_rm_watch(inotify_fd, inotify_wd);
            inotify_wd = -1;
        }

        if (inotify_fd != -1) {
            ::close(inotify_fd);
            inotify_fd = -1;
        }
    }

  public:
    file_change_monitor(std::string path) {
       inotify_fd = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (inotify_fd < 0) throw hoytech::error("unable to create inotify descriptor: ", ::strerror(errno));

        inotify_wd = ::inotify_add_watch(inotify_fd, path.c_str(), IN_MODIFY);
        if (inotify_fd < 0) throw hoytech::error("unable to add watch to inotify descriptor: ", ::strerror(errno));

        eventfd_fd = ::eventfd(0, EFD_CLOEXEC);
        if (eventfd_fd < 0) throw hoytech::error("unable to create eventfd descriptor: ", ::strerror(errno));
    }

    void setDebounce(uint64_t ms) {
        debounce_us = ms * 1000;
    }

    void run(std::function<void()> cb) {
        if (shutdown) throw hoytech::error("inotify watcher already shutdown");

        t = std::thread([cb, this]() {
            struct pollfd pollfd_array[2] = {
                { inotify_fd, POLLIN, 0 },
                { eventfd_fd, POLLIN, 0 }
            };

            uint64_t trigger_time = 0;

            while (1) {
                int timeout_ms = -1;

                if (trigger_time) {
                    uint64_t now = hoytech::curr_time_us();

                    if (now < trigger_time) {
                        timeout_ms = (trigger_time - now) / 1000;
                    } else {
                        timeout_ms = 0;
                    }

                    if (timeout_ms == 0) {
                        trigger_time = 0;
                        cb();
                        continue;
                    }
                }

                int rv = ::poll(pollfd_array, 2, timeout_ms);
                if (shutdown) return;

                if (rv == -1 && errno == EINTR) continue;
                if (rv == -1) return;
                if (rv == 0) continue;
                struct inotify_event event;
                rv = ::read(inotify_fd, (char*)&event, sizeof(event));
                if (shutdown) return;

                if (rv == -1 && (errno == EINTR || errno == EAGAIN)) continue;
                if (rv == -1) return;
                if (rv != sizeof(event)) return;

                if (trigger_time == 0) trigger_time = hoytech::curr_time_us() + debounce_us;
            }
        });
    }

    ~file_change_monitor() {
        shutdown = true;

        if (t.joinable()) {
            if (eventfd_fd != -1) {
                uint64_t event_msg = 1;
                int rv = write(eventfd_fd, &event_msg, sizeof(event_msg));
                (void)rv;
            }

            t.join();
        }

        cleanup();
    }
};

#else

// ---- macOS / BSD implementation (kqueue) ----
// Same instant-notification semantics as the Linux inotify version. This matters because
// strfry's real-time event delivery relies on watching data.mdb for changes.

class file_change_monitor {
  private:
    std::string path;
    std::thread t;
    uint64_t debounce_us = 50 * 1000;
    int kq = -1;
    int file_fd = -1;
    bool shutdown = false;

    static const uintptr_t USER_IDENT = 1;

#if defined(__APPLE__)
    static const int OPEN_FLAGS = O_EVTONLY;
#else
    static const int OPEN_FLAGS = O_RDONLY;
#endif

    void registerVnode() {
        struct kevent ev;
        EV_SET(&ev, file_fd, EVFILT_VNODE, EV_ADD | EV_CLEAR,
               NOTE_WRITE | NOTE_EXTEND | NOTE_DELETE | NOTE_RENAME, 0, nullptr);
        ::kevent(kq, &ev, 1, nullptr, 0, nullptr);
    }

  public:
    file_change_monitor(std::string path_) : path(std::move(path_)) {
        kq = ::kqueue();
        if (kq < 0) throw hoytech::error("unable to create kqueue descriptor: ", ::strerror(errno));

        file_fd = ::open(path.c_str(), OPEN_FLAGS);
        if (file_fd < 0) throw hoytech::error("unable to open file for monitoring: ", path, ": ", ::strerror(errno));
    }

    void setDebounce(uint64_t ms) {
        debounce_us = ms * 1000;
    }

    void run(std::function<void()> cb) {
        if (shutdown) throw hoytech::error("file watcher already shutdown");

        t = std::thread([cb, this]() {
            registerVnode();

            // EVFILT_USER channel used to wake the kevent() wait on shutdown
            {
                struct kevent uev;
                EV_SET(&uev, USER_IDENT, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, nullptr);
                ::kevent(kq, &uev, 1, nullptr, 0, nullptr);
            }

            uint64_t trigger_time = 0;

            while (1) {
                struct timespec ts;
                struct timespec *tsp = nullptr;

                if (trigger_time) {
                    uint64_t now = hoytech::curr_time_us();

                    if (now >= trigger_time) {
                        trigger_time = 0;
                        cb();
                        continue;
                    }

                    uint64_t delta = trigger_time - now;
                    ts.tv_sec = delta / 1000000;
                    ts.tv_nsec = (delta % 1000000) * 1000;
                    tsp = &ts;
                }

                struct kevent out;
                int rv = ::kevent(kq, nullptr, 0, &out, 1, tsp);
                if (shutdown) return;

                if (rv == -1 && errno == EINTR) continue;
                if (rv == -1) return;
                if (rv == 0) continue; // timeout: loop re-checks trigger_time and fires cb

                if (out.filter == EVFILT_USER) return; // shutdown signalled

                if (out.filter == EVFILT_VNODE) {
                    // File replaced (e.g. config rewritten via rename) -> reopen and re-watch
                    if (out.fflags & (NOTE_DELETE | NOTE_RENAME)) {
                        ::close(file_fd);
                        file_fd = ::open(path.c_str(), OPEN_FLAGS);
                        if (file_fd >= 0) registerVnode();
                    }

                    if (trigger_time == 0) trigger_time = hoytech::curr_time_us() + debounce_us;
                }
            }
        });
    }

    ~file_change_monitor() {
        shutdown = true;

        if (t.joinable()) {
            struct kevent trig;
            EV_SET(&trig, USER_IDENT, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
            ::kevent(kq, &trig, 1, nullptr, 0, nullptr);
            t.join();
        }

        if (file_fd != -1) { ::close(file_fd); file_fd = -1; }
        if (kq != -1) { ::close(kq); kq = -1; }
    }
};

#endif


}

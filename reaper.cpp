/*
 *  Copyright 2021 Google, Inc
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#define LOG_TAG "lowmemorykiller"

#include <errno.h>
#include <fcntl.h>
#include <log/log.h>
#include <string.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/pidfd.h>
#include <sys/sysinfo.h>
#include <time.h>
#include <unistd.h>

#include <processgroup/processgroup.h>

#include "reaper.h"

#define NS_PER_MS (NS_PER_SEC / MS_PER_SEC)

#ifndef __NR_process_mrelease
#define __NR_process_mrelease 448
#endif

static int process_mrelease(int pidfd, unsigned int flags) {
    return syscall(__NR_process_mrelease, pidfd, flags);
}

static inline long get_time_diff_ms(struct timespec *from,
                                    struct timespec *to) {
    return (to->tv_sec - from->tv_sec) * (long)MS_PER_SEC +
           (to->tv_nsec - from->tv_nsec) / (long)NS_PER_MS;
}

static void* reaper_main(void* param) {
    Reaper *reaper = (Reaper *)param;
    struct timespec start_tm, end_tm;
    struct Reaper::queued_proc proc;
    pid_t tid = gettid();

    // Ensure the thread does not use little cores
    if (!SetTaskProfiles(tid, {"CPUSET_SP_FOREGROUND"}, true)) {
        ALOGE("Failed to assign cpuset to the reaper thread");
    }

    for (;;) {
        proc = reaper->dequeue_request();

        if (reaper->debug_logs_enabled()) {
            clock_gettime(CLOCK_MONOTONIC_COARSE, &start_tm);
        }

        if (pidfd_send_signal(proc.pidfd, SIGKILL, NULL, 0)) {
            // Inform the main thread about failure to kill
            reaper->notify_kill_failure(proc.pid);
            close(proc.pidfd);
            continue;
        }
        if (process_mrelease(proc.pidfd, 0)) {
            ALOGE("process_mrelease %d failed: %s", proc.pidfd, strerror(errno));
            close(proc.pidfd);
            continue;
        }
        if (reaper->debug_logs_enabled()) {
            clock_gettime(CLOCK_MONOTONIC_COARSE, &end_tm);
            ALOGI("Process %d was reaped in %ldms", proc.pid,
                  get_time_diff_ms(&start_tm, &end_tm));
        }

        close(proc.pidfd);
        reaper->request_complete();
    }

    return NULL;
}

bool Reaper::is_reaping_supported() {
    static enum {
        UNKNOWN,
        SUPPORTED,
        UNSUPPORTED
    } reap_support = UNKNOWN;

    if (reap_support == UNKNOWN) {
        if (process_mrelease(-1, 0) && errno == ENOSYS) {
            reap_support = UNSUPPORTED;
        } else {
            reap_support = SUPPORTED;
        }
    }
    return reap_support == SUPPORTED;
}

int Reaper::setup_thread_comm() {
    if (pipe(comm_fd_)) {
        ALOGE("pipe failed: %s", strerror(errno));
        return -1;
    }

    // Ensure main thread never blocks on read
    int flags = fcntl(comm_fd_[0], F_GETFL);
    if (fcntl(comm_fd_[0], F_SETFL, flags | O_NONBLOCK)) {
        ALOGE("fcntl failed: %s", strerror(errno));
        drop_thread_comm();
        return -1;
    }

    return comm_fd_[0];
}

void Reaper::drop_thread_comm() {
    close(comm_fd_[0]);
    close(comm_fd_[1]);
}

bool Reaper::create_thread_pool() {
    int cpu_count = get_nprocs();

    thread_pool_ = new pthread_t[cpu_count];
    reaper_thread_cnt_ = 0;
    for (int i = 0; i < cpu_count; i++) {
        if (pthread_create(&thread_pool_[reaper_thread_cnt_], NULL,
                           reaper_main, this)) {
            ALOGE("pthread_create failed: %s", strerror(errno));
            continue;
        }
        reaper_thread_cnt_++;
    }

    if (!reaper_thread_cnt_) {
        delete[] thread_pool_;
        return false;
    }

    queue_.reserve(reaper_thread_cnt_);
    return true;
}

bool Reaper::request_kill(int pidfd, int pid) {
    struct queued_proc proc;

    if (pidfd == -1) {
        return false;
    }

    if (!reaper_thread_cnt_) {
        return false;
    }

    mutex_.lock();
    if (active_requests_ >= reaper_thread_cnt_) {
        mutex_.unlock();
        return false;
    }
    active_requests_++;

    // Duplicate pidfd because the original one is used for waiting
    proc.pidfd = dup(pidfd);
    proc.pid = pid;
    queue_.push_back(proc);
    // Wake up a reaper thread
    cond_.notify_one();
    mutex_.unlock();

    return true;
}

int Reaper::get_failed_kill_pid() {
    int pid;

    if (TEMP_FAILURE_RETRY(read(comm_fd_[0], &pid, sizeof(pid))) != sizeof(pid)) {
        ALOGE("thread communication read failed: %s", strerror(errno));
        return -1;
    }
    return pid;
}

Reaper::queued_proc Reaper::dequeue_request() {
    struct queued_proc proc;
    std::unique_lock<std::mutex> lock(mutex_);

    while (queue_.empty()) {
        cond_.wait(lock);
    }
    proc = queue_.back();
    queue_.pop_back();

    return proc;
}

void Reaper::request_complete() {
    std::scoped_lock<std::mutex> lock(mutex_);
    active_requests_--;
}

void Reaper::notify_kill_failure(int pid) {
    std::scoped_lock<std::mutex> lock(mutex_);
    if (TEMP_FAILURE_RETRY(write(comm_fd_[1], &pid, sizeof(pid))) != sizeof(pid)) {
        ALOGE("thread communication write failed: %s", strerror(errno));
    }
}


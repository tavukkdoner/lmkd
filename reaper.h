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

#pragma once

#include <condition_variable>
#include <mutex>
#include <vector>

class Reaper {
public:
    struct queued_proc {
        int pidfd;
        int pid;
    };
private:
    // mutex_ and cond_ are used to wakeup the reaper thread.
    std::mutex mutex_;
    std::condition_variable cond_;
    // mutex_ protects queue_ and active_requests_ access.
    std::vector<struct queued_proc> queue_;
    int active_requests_;
    // pipe fds to communicate kill failures with the main thread
    int comm_fd_[2];
    int reaper_thread_cnt_;
    pthread_t* thread_pool_;
    bool debug_logs_enabled_;
public:
    Reaper();

    static bool is_reaping_supported();

    int setup_thread_comm();
    void drop_thread_comm();
    bool create_thread_pool();
    int reaper_thread_cnt() const { return reaper_thread_cnt_; }
    void enable_debug_logs(bool enable) { debug_logs_enabled_ = enable; }
    bool debug_logs_enabled() const { return debug_logs_enabled_; }

    bool request_kill(int pidfd, int pid);
    int get_failed_kill_pid();
    // below members are used only by reaper_main
    queued_proc dequeue();
    void notify_kill_failure(int pid);
};


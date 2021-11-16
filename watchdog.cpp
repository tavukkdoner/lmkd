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

#include <log/log.h>

#include "watchdog.h"

static void* watchdog_main(void* param) {
    Watchdog *watchdog = (Watchdog*)param;
    int reset_cnt;

    for (;;) {
        // Wait for watchdog to be set
        reset_cnt = watchdog->wait_for_set();

        while (true) {
            // Wait for watchdog to either be reset or to timeout
            if (watchdog->wait_for_reset(reset_cnt)) {
                // Watchdog was reset
                break;
            }
            // Call watchdog callback and keep waiting for the reset for another timeout period
            watchdog->bite();
        }
    }

    return NULL;
}

bool Watchdog::start() {
    pthread_t thread;

    if (pthread_create(&thread, NULL, watchdog_main, this)) {
        ALOGE("pthread_create failed: %s", strerror(errno));
        return false;
    }
    return true;
}

void Watchdog::set() {
    std::scoped_lock<std::mutex> lock(mutex_);
    set_cnt_++;
    cond_.notify_one();
}

void Watchdog::reset() {
    std::scoped_lock<std::mutex> lock(mutex_);
    reset_cnt_++;
    cond_.notify_one();
}

int Watchdog::wait_for_set() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (set_cnt_ == reset_cnt_) {
        cond_.wait(lock);
    }
    return reset_cnt_;
}

bool Watchdog::wait_for_reset(int reset_cnt) {
    std::chrono::duration timeout = std::chrono::seconds(timeout_);
    std::unique_lock<std::mutex> lock(mutex_);
    while (reset_cnt_ == reset_cnt) {
        if (cond_.wait_for(lock, timeout) == std::cv_status::timeout) {
            // Wait timed out
            return false;
        }
    }
    return true;
}

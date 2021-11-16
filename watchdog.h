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

class Watchdog {
private:
    std::mutex mutex_;
    std::condition_variable cond_;
    int set_cnt_;
    int reset_cnt_;
    int timeout_;
    void (*callback_)();
public:
    Watchdog(int timeout, void (*callback)()) : set_cnt_(0), reset_cnt_(0),
        timeout_(timeout), callback_(callback) {}

    bool start();
    void set();
    void reset();
    // used by the watchdog_main
    int wait_for_set();
    bool wait_for_reset(int reset_cnt);
    void bite() const { if (callback_) callback_(); }
};

/*MT*
    
    MediaTomb - http://www.mediatomb.cc/
    
    timer.cc - this file is part of MediaTomb.
    
    Copyright (C) 2005 Gena Batyan <bgeradz@mediatomb.cc>,
                       Sergey 'Jin' Bostandzhyan <jin@mediatomb.cc>
    
    Copyright (C) 2006-2010 Gena Batyan <bgeradz@mediatomb.cc>,
                            Sergey 'Jin' Bostandzhyan <jin@mediatomb.cc>,
                            Leonhard Wimmer <leo@mediatomb.cc>
    
    MediaTomb is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License version 2
    as published by the Free Software Foundation.
    
    MediaTomb is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
    
    You should have received a copy of the GNU General Public License
    version 2 along with MediaTomb; if not, write to the Free Software
    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.
    
    $Id$
*/

/// \file timer.cc

#include "timer.h" // API

#include <cassert>

Timer::Timer(std::shared_ptr<Config> config)
    : shutdownFlag(false)
    , config(std::move(config))
{
}

void Timer::run()
{
    log_debug("Starting Timer thread...");
    threadRunner = std::make_unique<StdThreadRunner>("TimerThread", Timer::staticThreadProc, this, config);

    if (!threadRunner->isAlive())
        throw_std_runtime_error("Failed to start timer thread");
}

void* Timer::staticThreadProc(void* arg)
{
    log_debug("Started Timer thread.");
    auto inst = static_cast<Timer*>(arg);
    inst->threadProc();
    log_debug("Exiting Timer thread...");
    return nullptr;
}

void Timer::threadProc()
{
    triggerWait();
}

void Timer::addTimerSubscriber(Subscriber* timerSubscriber, unsigned int notifyInterval, std::shared_ptr<Parameter> parameter, bool once)
{
    log_debug("Adding subscriber... interval: {} once: {} ", notifyInterval, once);
    if (notifyInterval == 0)
        throw_std_runtime_error("Tried to add timer with illegal notifyInterval: {}", notifyInterval);

    auto lock = threadRunner->lockGuard();
    TimerSubscriberElement element(timerSubscriber, notifyInterval, std::move(parameter), once);

    if (!subscribers.empty()) {
        bool err = std::any_of(subscribers.begin(), subscribers.end(), [&](const auto& subscriber) { return subscriber == element; });
        if (err) {
            throw_std_runtime_error("Tried to add same timer twice");
        }
    }

    subscribers.push_back(element);
    threadRunner->notify();
}

void Timer::removeTimerSubscriber(Subscriber* timerSubscriber, std::shared_ptr<Parameter> parameter, bool dontFail)
{
    log_debug("Removing subscriber...");
    auto lock = threadRunner->lockGuard();
    if (!subscribers.empty()) {
        TimerSubscriberElement element(timerSubscriber, 0, std::move(parameter));
        auto it = std::find(subscribers.begin(), subscribers.end(), element);
        if (it != subscribers.end()) {
            subscribers.erase(it);
            threadRunner->notify();
            log_debug("Removed subscriber...");
            return;
        }
    }
    if (!dontFail) {
        throw_std_runtime_error("Tried to remove nonexistent timer");
    }
}

void Timer::triggerWait()
{
    std::unique_lock<std::mutex> lock(waitMutex);

    while (!shutdownFlag) {
        log_debug("triggerWait. - {} subscriber(s)", subscribers.size());

        if (subscribers.empty()) {
            log_debug("Nothing to do, sleeping...");
            threadRunner->wait(lock);
            continue;
        }

        struct timespec* timeout = getNextNotifyTime();
        struct timespec now;
        getTimespecNow(&now);

        long wait = getDeltaMillis(&now, timeout);
        if (wait > 0) {
            auto ret = threadRunner->waitFor(lock, wait);
            if (ret != std::cv_status::timeout) {
                /*
                 * Some rude thread woke us!
                 * Now we have to wait all over again...
                 */
                continue;
            }
        }
        notify();
    }
}

void Timer::notify()
{
    auto lock = threadRunner->uniqueLock();
    assert(lock.owns_lock());

    std::list<TimerSubscriberElement> toNotify;

    if (!subscribers.empty()) {
        for (auto it = subscribers.begin(); it != subscribers.end(); /*++it*/) {
            TimerSubscriberElement& element = *it;

            struct timespec now;
            getTimespecNow(&now);
            long wait = getDeltaMillis(&now, element.getNextNotify());

            if (wait <= 0) {
                toNotify.push_back(element);
                if (element.isOnce()) {
                    it = subscribers.erase(it);
                } else {
                    element.updateNextNotify();
                    ++it;
                }
            } else {
                ++it;
            }
        }
    }

    // Unlock before we notify so that other threads can modify the subscribers
    lock.unlock();
    for (auto& element : toNotify) {
        element.notify();
    }
}

struct timespec* Timer::getNextNotifyTime()
{
    auto lock = threadRunner->lockGuard();
    struct timespec* nextTime = nullptr;
    if (!subscribers.empty()) {
        for (auto& subscriber : subscribers) {
            struct timespec* nextNotify = subscriber.getNextNotify();
            if (nextTime == nullptr || getDeltaMillis(nextTime, nextNotify) < 0) {
                nextTime = nextNotify;
            }
        }
    }
    return nextTime;
}

void Timer::shutdown()
{
    shutdownFlag = true;
    threadRunner->notifyAll();
    threadRunner->join();
}

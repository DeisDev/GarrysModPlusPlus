#pragma once

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <functional>
#include <span>
#include <thread>
#include <vector>

namespace gmodpp {

// Runs the jobs on this thread and on worker threads, as many in all as the logical cores less one (at least one), and
// returns the first failure. Creating pipeline states this way divides the wait, since the driver compiles each one's
// shaders as it is created and a device creates them on any thread. `done` counts the jobs finished.
inline HRESULT RunInParallel(std::span<const std::function<HRESULT()>> jobs, std::atomic<std::size_t>* done = nullptr)
{
    std::atomic<std::size_t> next = 0;
    std::atomic<HRESULT> result = S_OK;
    auto work = [&] {
        for (std::size_t i = next++; i < jobs.size(); i = next++) {
            const HRESULT hr = jobs[i]();
            HRESULT expected = S_OK;
            if (FAILED(hr))
                result.compare_exchange_strong(expected, hr);
            if (done != nullptr)
                done->fetch_add(1, std::memory_order_relaxed);
        }
    };
    const std::size_t threads = std::min<std::size_t>(std::max(2u, std::thread::hardware_concurrency()) - 1, jobs.size());
    {
        std::vector<std::jthread> workers;
        for (std::size_t i = 1; i < threads; ++i)
            workers.emplace_back([&] {
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
                work();
            });
        work();
    }
    return result.load();
}

}

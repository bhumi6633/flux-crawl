#pragma once
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
namespace fluxcrawl::detail {
// Declared after the state it samples. Destructor wakes immediately and joins.
class ObservabilitySampler {
public:
    ObservabilitySampler(std::chrono::milliseconds interval, std::function<void()> capture)
        : thread_{[this, interval, capture=std::move(capture)] {
            std::unique_lock lock{mutex_};
            while (!wake_.wait_for(lock, interval, [this] { return stopped_; })) { capture(); }
        }} {}
    ~ObservabilitySampler() {
        { std::lock_guard lock{mutex_}; stopped_=true; }
        wake_.notify_all(); thread_.join();
    }
private:
    std::mutex mutex_;
    std::condition_variable wake_;
    bool stopped_=false;
    std::thread thread_;
};
}

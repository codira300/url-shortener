#pragma once

#include <chrono>
#include <condition_variable>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

namespace urlshort {

template <typename T>
struct SingleFlightResult {
  std::optional<T> value;
  bool leader{false};
  bool timed_out{false};
};

// Coalesces concurrent loads for the same key inside one process. This is the
// first line of defence against a cache stampede; the bounded database pool is
// the second. It does not coordinate across processes.
template <typename T>
class SingleFlight {
 public:
  SingleFlightResult<T> execute(const std::string& key, std::function<T()> load,
                                std::chrono::milliseconds wait_timeout) {
    std::shared_ptr<Entry> entry;
    bool leader = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto [it, inserted] = in_flight_.try_emplace(key);
      if (inserted) {
        entry = std::make_shared<Entry>();
        it->second = entry;
        leader = true;
      } else {
        entry = it->second;
      }
    }

    if (!leader) {
      std::unique_lock<std::mutex> lock(entry->mutex);
      if (!entry->cv.wait_for(lock, wait_timeout, [&entry] { return entry->done; })) {
        return SingleFlightResult<T>{std::nullopt, false, true};
      }
      if (entry->error) {
        std::rethrow_exception(entry->error);
      }
      return SingleFlightResult<T>{entry->value, false, false};
    }

    std::optional<T> value;
    std::exception_ptr error;
    try {
      value = load();
    } catch (...) {
      error = std::current_exception();
    }

    {
      std::lock_guard<std::mutex> lock(entry->mutex);
      entry->value = value;
      entry->error = error;
      entry->done = true;
    }
    entry->cv.notify_all();

    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto it = in_flight_.find(key);
      if (it != in_flight_.end() && it->second == entry) {
        in_flight_.erase(it);
      }
    }

    if (error) {
      std::rethrow_exception(error);
    }
    return SingleFlightResult<T>{value, true, false};
  }

 private:
  struct Entry {
    std::mutex mutex;
    std::condition_variable cv;
    bool done{false};
    std::optional<T> value;
    std::exception_ptr error;
  };

  std::mutex mutex_;
  std::unordered_map<std::string, std::shared_ptr<Entry>> in_flight_;
};

}  // namespace urlshort

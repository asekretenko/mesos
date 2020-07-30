// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef __STOUT_WEAK_CACHE_HPP__
#define __STOUT_WEAK_CACHE_HPP__

#include <glog/logging.h>

#include <atomic>
#include <memory>
#include <unordered_map>

template <class Key, class Value>
class WeakCache
{
  // Mapping from keys to weak_ptrs to Values that are potentially stored by
  // elsewhere.
  std::unordered_map<Key, std::weak_ptr<Value>> cache;

  // The number of expired weak_ptrs in `cache`.
  //
  // NOTE: The pointed-to atomic_size_t is potentially modified concurrently
  // from multiple threads (by deleters of the handed out shared_ptrs and also
  // by `get()`).
  std::shared_ptr<std::atomic_size_t> expiredCounter;

public:
  WeakCache() : expiredCounter(std::make_shared<std::atomic_size_t>(0)) {}

  WeakCache(const WeakCache&) = delete;
  WeakCache& operator=(const WeakCache&) = delete;


  // Tries to find a weak_ptr to the Value corresponding to the Key
  // in the hashtable and returns a shared_ptr to a "cached" Value if the Value
  // still exists.
  // Otherwise, creates a Value by calling a Factory, keeps a weak_ptr to the
  // created Value and stores the pair (key, weak_ptr) in the hashtable.
  //
  // Hashtable entries corresponding to expired weak_ptr are cleaned up
  // in this method on creating a new value by sweeping the hashtable
  // when the count of expired weak_ptrs exceeds half of the hashtable size.
  //
  // NOTE: This method is by no means threadsafe, but the handed out
  // shared_ptrs have the same thread safety properties as "normal" ones:
  // counting of expired entries that occurs on destruction is threadsafe.
  template <class Factory>
  std::shared_ptr<Value> get(const Key& key, Factory&& factory)
  {
    static_assert(
        std::is_same<
            decltype(std::forward<Factory>(factory)()),
            std::unique_ptr<Value>>::value,
        "Factory::operator() must return unique_ptr<Value>");

    const auto weakCached = cache.find(key);
    if (weakCached != cache.end()) {
      const std::shared_ptr<Value> cached = weakCached->second.lock();
      if (cached) {
        return cached;
      }
    }

    const std::shared_ptr<std::atomic_size_t> expiredCounter_ = expiredCounter;

    std::shared_ptr<Value> shared(
        CHECK_NOTNULL(std::forward<Factory>(factory)().release()),
        [expiredCounter_](Value* p) {
          delete p;
          expiredCounter_->fetch_add(1);
        });

    if (expiredCounter->load() * 2 > cache.size()) {
      gcExpired();
    }

    cache.insert({key, shared});

    return shared;
  }

  // Exposed for tests only.
  size_t size() const { return cache.size(); }

private:
  void gcExpired()
  {
    size_t erasedCount = 0;

    for (auto it = cache.begin(); it != cache.end();) {
      if (it->second.expired()) {
        it = cache.erase(it);
        ++erasedCount;
      } else {
        ++it;
      }
    }

    CHECK_LE(erasedCount, expiredCounter->load());
    expiredCounter->fetch_sub(erasedCount);
  }
};

#endif // __STOUT_WEAK_CACHE_HPP__

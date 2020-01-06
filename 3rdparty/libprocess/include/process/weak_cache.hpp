// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License

#ifndef __PROCESS_WEAK_CACHE_HPP__
#define __PROCESS_WEAK_CACHE_HPP__

#include <memory>
#include <string>
#include <unordered_map>

#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/id.hpp>
#include <process/process.hpp>


namespace process {


template<class Key, class Item, class Hash, class Equal>
class WeakCacheProcess
  : public Process<WeakCacheProcess<Key, Item, Hash, Equal>>
{
public:
  WeakCacheProcess(const std::string& id) : ProcessBase(ID::generate(id)){};

  template <typename... CreationArgs>
  std::pair<std::shared_ptr<Item>, bool> get(
      const Key& key,
      CreationArgs&&... creationArgs)
  {
    const auto it = items.find(key);
    if (it != items.end()) {
      std::shared_ptr<Item> result = it->second.lock();
      if (result) {
        return {std::move(result), false};
      }
    }

    // NOTE: std::make_shared is not used here due to two reasons:
    //  - need for the custom deleter
    //  - potential for the control block to outlive the Item for a
    //    significant length of time
    PID<WeakCacheProcess> pid = this->self();
    std::shared_ptr<Item> result(
        new Item(key, std::forward<CreationArgs>(creationArgs)...),
        [pid, key](Item* ptr) {
          delete ptr;
          ::process::dispatch(pid, &WeakCacheProcess::gc, key);
        });

    items[key] = std::weak_ptr<Item>(result);
    return {std::move(result), true};
  }

  size_t keyCount() { return items.size(); }

private:

  // Removes garbage hashtable entries.
  void gc(const Key& key)
  {
    auto it = items.find(key);
    // NOTE: Between deletion of the Item that scheduled gc() and execution
    // of gc() a new Item might have been created for the same key,
    // hence the need to check `expired()`.
    if (it != items.end() && it->second.expired()) {
      items.erase(it);
    }
  }

  std::unordered_map<Key, std::weak_ptr<Item>, Hash, Equal> items;
};


// A key-value cache that hands out shared pointers to cached items,
// but does not own the items by its own.
template <
    class Key,
    class Item,
    class Hash = std::hash<Key>,
    class Equal = std::equal_to<Key>>
class WeakCache {
  using Process_ = WeakCacheProcess<Key, Item, Hash, Equal>;

public:
  // For a given Key, either gets a pointer to Item from cache, or constructs a
  // new Item and caches a weak_ptr to it.
  //
  // Returns a shared_ptr to the Item and a flag that is set to true if a new
  // Item was created.
  template <typename... CreationArgs>
  Future<std::pair<std::shared_ptr<Item>, bool>> get(
      const Key& key,
      CreationArgs&&... creationArgs)
  {
    static_assert(
        std::is_constructible<Item, Key, CreationArgs&&...>::value,
        "Item cannot be constructed from WeakCache::get(...) arguments");

    return ::process::dispatch(
        pid,
        &Process_::template get<CreationArgs...>,
        key,
        std::forward<CreationArgs>(creationArgs)...);
  }

  // Returns amount of keys in cache, including expired ones.
  // Used primarily for tests.
  Future<size_t> keyCount() {
    return ::process::dispatch(pid, &Process_::keyCount);
  }

  WeakCache(const std::string& id) : pid(::process::spawn(new Process_(id))){};

  ~WeakCache() {
    ::process::terminate(pid);
  }

private:
  PID<Process_> pid;

};


} // namespace process {

#endif // __PROCESS_WEAK_CACHE_HPP__

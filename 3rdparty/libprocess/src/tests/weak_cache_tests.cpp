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

#include <string>
#include <memory>

#include <process/gtest.hpp>
#include <process/time.hpp>

#include <process/weak_cache.hpp>

using std::string;

using process::Clock;
using process::Future;

struct Item
{
  Item(const string& key, string data_) : data(std::move(data_)){};
  string data;
};


TEST(WeakCacheTest, twoKeys)
{
  process::WeakCache<string, Item> cache("testing_weak_cache");

  const string KEY1 {"k1"};
  const string KEY2 {"k2"};
 
  const string INITIALIZER1 {"foo"};
  const string INITIALIZER2 {"bar"};

  {
    //
    // First, get two items for the same key and test for caching behavior.

    Future<std::pair<std::shared_ptr<Item>, bool>> item1 =
      cache.get(KEY1, INITIALIZER1);

    AWAIT_READY(item1);
    EXPECT_EQ(INITIALIZER1, item1.get().first->data);

    // Creation flag should be set.
    EXPECT_TRUE(item1.get().second);

    Future<std::pair<std::shared_ptr<Item>, bool>> item2 =
      cache.get(KEY1, INITIALIZER2);

    AWAIT_READY(item2);

    // shared_ptr for the same object should be returned.
    EXPECT_EQ(item2.get().first, item1.get().first);

    // The stored object should have the same value.
    EXPECT_EQ(INITIALIZER1, item2.get().first->data);

    // Creation flag should be not set.
    EXPECT_FALSE(item2.get().second);

    // There should be exactly one item in the cache.
    AWAIT_EXPECT_EQ(1ul, cache.keyCount());

    //
    // Now, get an item for a different key and perform the same tests.

    Future<std::pair<std::shared_ptr<Item>, bool>> itemForKey2 =
      cache.get(KEY2, INITIALIZER2);

    AWAIT_READY(itemForKey2);

    EXPECT_EQ(INITIALIZER2, itemForKey2.get().first->data);
    EXPECT_NE(item1.get().first, itemForKey2.get().first);
    EXPECT_TRUE(itemForKey2.get().second);
    AWAIT_EXPECT_EQ(2ul, cache.keyCount());
  }

  // All shared_ptrs to data are destroyed by now, check garbage collection.
  Clock::pause();
  Clock::settle();

  // Check that cache is now empty
  AWAIT_EXPECT_EQ(0ul, cache.keyCount());
}

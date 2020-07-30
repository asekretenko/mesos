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

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include <stout/weak_cache.hpp>

using std::shared_ptr;
using std::string;
using std::unique_ptr;
using std::vector;
using std::weak_ptr;


TEST(WeakCacheTest, WeakCaching)
{
  WeakCache<string, int> cache;

  auto createFactory = [](int value) {
    auto factory = [value]() { return std::unique_ptr<int>(new int(value)); };
    return factory;
  };

  weak_ptr<int> weak_foo;
  weak_ptr<int> weak_bar;
  {
    shared_ptr<int> foo = cache.get("foo", createFactory(100));
    shared_ptr<int> bar = cache.get("bar", createFactory(500));
    weak_foo = foo;
    weak_bar = bar;

    EXPECT_EQ(100, *foo);
    EXPECT_EQ(500, *bar);

    shared_ptr<int> cachedFoo = cache.get("foo", createFactory(100500));

    // `foo` and `cachedFoo` should POINT to the same object
    int* fooPtr = foo.get();
    int* cachedFooPtr = cachedFoo.get();
    EXPECT_EQ(fooPtr, cachedFooPtr);
  }

  // Weak cache stores no strong references.
  EXPECT_TRUE(weak_foo.expired());
  EXPECT_TRUE(weak_bar.expired());

  // Now that the old values corresponding to "foo" have been destroyed,
  // the value should be created anew.
  shared_ptr<int> newFoo = cache.get("foo", createFactory(100500));
  EXPECT_EQ(100500, *newFoo);
}


TEST(WeakCacheTest, Cleanup)
{
  WeakCache<int, string> cache;

  auto factory = []() { return std::unique_ptr<string>(new string("foo")); };

  vector<shared_ptr<string>> bunch1;
  for (int i = 0; i < 10; ++i) {
    bunch1.push_back(cache.get(i, factory));
  }

  ASSERT_EQ(10, cache.size());

  {
    vector<shared_ptr<string>> bunch2;
    for (int i = 10; i < 30; ++i) {
      bunch2.push_back(cache.get(i, factory));
    }

    ASSERT_EQ(30, cache.size());
  }

  // Create one more new value to trigger cleanup of expired cache entries.
  cache.get(100500, factory);

  EXPECT_EQ(11, cache.size());
}

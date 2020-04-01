// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <stdlib.h> // For atof.

#include <string>

#include <stout/none.hpp>
#include <stout/option.hpp>

#include "common/build.hpp"

// NOTE: On CMake, instead of defining `BUILD_DATE|TIME|USER` as
// compiler flags, we instead emit a header file with the definitions.
// This facilitates incremental builds as the compiler flags will
// no longer change with every invocation of the build.
// TODO(josephw): After deprecating autotools, remove this guard.
#ifdef USE_CMAKE_BUILD_CONFIG
#include "common/build_config.hpp"
#endif // USE_CMAKE_BUILD_CONFIG

#include "common/git_version.hpp"

using std::string;

namespace mesos {
namespace internal {
namespace build {

string date() { return BUILD_DATE; };
double time() { return atof(BUILD_TIME); };

string user()
{
#ifdef BUILD_USER
  return BUILD_USER;
#else
  return "";
#endif
}

string flags() { return BUILD_FLAGS; };

string java_jvm_library()
{
#ifdef BUILD_JAVA_JVM_LIBRARY
  return BUILD_JAVA_JVM_LIBRARY;
#else
  return "";
#endif
}

Option<string> git_sha()
{
#ifdef BUILD_GIT_SHA
  return string(BUILD_GIT_SHA);
#else
  return None();
#endif
}

Option<string> git_branch()
{
#ifdef BUILD_GIT_BRANCH
  return string(BUILD_GIT_BRANCH);
#else
  return None();
#endif
}

Option<string> git_tag()
{
#ifdef BUILD_GIT_TAG
  return string(BUILD_GIT_TAG);
#else
  return None();
#endif
}

} // namespace build {
} // namespace internal {
} // namespace mesos {

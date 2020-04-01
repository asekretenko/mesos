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

#ifndef __VERSION_HPP__
#define __VERSION_HPP__

#include <string>

#include <process/future.hpp>
#include <process/http.hpp>
#include <process/process.hpp>

#include "common/build.hpp"

namespace mesos {
namespace internal {

// Helper function to return Mesos version.
inline JSON::Object version()
{
  JSON::Object object;
  object.values["version"] = MESOS_VERSION;

  if (build::git_sha().isSome()) {
    object.values["git_sha"] = build::git_sha().get();
  }

  if (build::git_branch().isSome()) {
    object.values["git_branch"] = build::git_branch().get();
  }

  if (build::git_tag().isSome()) {
    object.values["git_tag"] = build::git_tag().get();
  }

  object.values["build_date"] = build::date();
  object.values["build_time"] = build::time();
  object.values["build_user"] = build::user();

  return object;
}


class VersionProcess : public process::Process<VersionProcess>
{
public:
  VersionProcess();

protected:
  void initialize() override;

private:
  static process::Future<process::http::Response> version(
      const process::http::Request& request);
};

}  // namespace internal {
}  // namespace mesos {

#endif // __VERSION_HPP__

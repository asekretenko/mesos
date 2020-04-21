# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.


# This function tries to split the list of source files (`SOURCES`) into object
# libraries so that each source directory corresponds to a single library.
# This is used to work around the issue that cmake+msbuild are incapable of
# building files with the same filename in parallel (see MESOS-10122).
#
# Each of the resulting intermediate libraries is compiled using specified
# `INCLUDE_DIRECTORIES` and `COMPILE DEFINITIONS`, and also interface
# definitions and include directories of `LINK_LIBRARIES`, and has
# `LINK_LIBRARIES` added as dependencies.
# The list of objects in all intermediate libraries is placed into `OUTPUT`.
function(
  INTERMEDIATE_OBJECT_LIBRARIES
  OUTPUT
  SOURCES
  LINK_LIBRARIES
  INCLUDE_DIRECTORIES
  COMPILE_DEFINITIONS)

  # TODO(asekretenko): After we drop support for pre-3.12 cmake versions
  # (that don't support `target_link_libraries()` for OBJECT libraries),
  # DEPENDENCIES_LIB should be dismantled.
  set(DEPENDENCIES_LIB ${OUTPUT}_intermediate_object_dependencies)
  add_library(${DEPENDENCIES_LIB} INTERFACE)
  target_include_directories(${DEPENDENCIES_LIB} INTERFACE ${INCLUDE_DIRECTORIES})
  target_compile_definitions(${DEPENDENCIES_LIB} INTERFACE ${COMPILE_DEFINITIONS})
  target_link_libraries(${DEPENDENCIES_LIB} INTERFACE ${LINK_LIBRARIES})

  # TODO(asekretenko): Remove explicit dependencies if and when
  # https://gitlab.kitware.com/cmake/cmake/issues/18203 is fixed.
  add_dependencies(${DEPENDENCIES_LIB} ${LINK_LIBRARIES})

  # Generate a source list for each library.
  set(SOURCE_LIST_NAMES)
  foreach(SOURCE ${SOURCES})
    get_filename_component(DIR ${SOURCE} DIRECTORY)
    string(REPLACE "/" "_" LIB_SUFFIX "${DIR}")
    string(REPLACE ":" "_" LIB_SUFFIX "${LIB_SUFFIX}")
    # NOTE: Due to accidental collisions (`foo/bar/baz.cpp` vs
    # `foo_bar/baz.cpp`) sources with the same filename might end up
    # in the same object library.
    #
    # NOTE: To minimize risk of library names collision of generated
    # by different calls to INTERMEDIATE_OBJECT_LIBRARIES,
    # the name of the output variable is added to the library name.
    set(SOURCE_LIST_NAME LIB_${OUTPUT}_${LIB_SUFFIX})
    list(APPEND SOURCE_LIST_NAMES ${SOURCE_LIST_NAME})
    list(APPEND ${SOURCE_LIST_NAME} ${SOURCE})
  endforeach()

  list(REMOVE_DUPLICATES SOURCE_LIST_NAMES)

  set(OBJECTS)
  foreach(LIST_NAME ${SOURCE_LIST_NAMES})
    string(TOLOWER ${LIST_NAME} LIB)
    set(SOURCE_LIST "${${LIST_NAME}}")
    add_library(${LIB} OBJECT ${SOURCE_LIST})
    add_dependencies(${LIB} ${DEPENDENCIES_LIB})
    target_compile_definitions(${LIB} PRIVATE
      $<TARGET_PROPERTY:${DEPENDENCIES_LIB},INTERFACE_COMPILE_DEFINITIONS>)

    target_include_directories(${LIB} PRIVATE
      $<TARGET_PROPERTY:${DEPENDENCIES_LIB},INTERFACE_INCLUDE_DIRECTORIES>)

    list(APPEND OBJECTS $<TARGET_OBJECTS:${LIB}>)
  endforeach()

  set(${OUTPUT} ${OBJECTS} PARENT_SCOPE)
endfunction()

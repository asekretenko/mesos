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

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <stout/cache.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>
#include <stout/variant.hpp>
#include <stout/weak_cache.hpp>

#include <re2/re2.h>

#include <mesos/allocator/allocator.hpp>
#include <mesos/attributes.hpp>

using std::shared_ptr;
using std::string;
using std::vector;
using std::unique_ptr;
using std::unordered_map;

using ::mesos::scheduler::AttributeConstraint;
using ::mesos::scheduler::OfferConstraints;

namespace mesos {
namespace allocator {

namespace internal {

constexpr static size_t RE2_LRU_CACHE_SIZE = 1024;
constexpr static int64_t RE2_MAX_MEM = 4096;
constexpr static int RE2_MAX_PROGRAM_SIZE = 100;

class RE2Factory
{
  // Caching factory for RE2 regexps.
  //
  // RE2s are cached by storing:
  // 1. Strong references to RE2s recently handed out, regardless of whether
  //    they are referenced elsewhere or not.
  Cache<string, shared_ptr<const RE2>> lruCache{RE2_LRU_CACHE_SIZE};

  // 2. Weak references to all RE2s that have ever been handed out by get()
  //    and are still strongly referenced somewhere (including `lruCache`).
  WeakCache<string, const RE2> weakCache;

public:
  shared_ptr<const RE2> operator()(const string& regex)
  {
    Option<shared_ptr<const RE2>> recent = lruCache.get(regex);
    if (recent.isSome()) {
      return *std::move(recent);
    }

    const shared_ptr<const RE2> re2 = weakCache.get(regex, [&regex]() {
      RE2::Options options {RE2::CannedOptions::Quiet};
      options.set_max_mem(RE2_MAX_MEM);
      return std::unique_ptr<const RE2>(new RE2(regex, options));
    });

    // NOTE: Invalid re2s are cached as well; in many cases they may be no less
    // expensive to construct than the valid ones.
    lruCache.put(regex, re2);
    return re2;
  }
};


using Selector = AttributeConstraint::Selector;


static Option<Error> validate(const Selector& selector)
{
  switch(selector.selector_case()) {
    case Selector::kPseudoattributeType:
      switch (selector.pseudoattribute_type()) {
        case Selector::HOSTNAME:
        case Selector::REGION:
        case Selector::ZONE:
          return None();
        case Selector::UNKNOWN:
          break;
      }

      return Error("Unknown pseudoattribute type");

    case Selector::kAttributeName:
      return None();

    case Selector::SELECTOR_NOT_SET:
      return Error(
        "Exactly one of 'AttributeConstraint::Selector::name' or"
        " 'AttributeConstraint::Selector::pseudoattribute_type' must be set");
  }

  UNREACHABLE();
}


static Option<Error> validateRE2(const RE2& re2){
  if (!re2.ok()) {
    return Error(
        "Failed to construct regex from pattern '" + re2.pattern() +
        "': " + re2.error());
  }

  if (re2.ProgramSize() > RE2_MAX_PROGRAM_SIZE) {
    return Error("Regex '" + re2.pattern() + "' is too complex.");
  }

  return None();
}


// An equivalent of RE2::fullMatch(), which, as of RE2 2020-07-06, is slightly
// faster with gcc -O2 due to the fact that the latter unconditionally zeroes
// internal arrays for returning submatches, about which we do not care.
static bool fullMatch(const string& str, const RE2& re2)
{
  return re2.Match(str, 0, str.size(), RE2::ANCHOR_BOTH, nullptr, 0);
}


class AttributeConstraintPredicate
{
public:
  bool apply(const Nothing& _) const { return apply_(_); }
  bool apply(const string& str) const { return apply_(str); }
  bool apply(const Attribute& attr) const { return apply_(attr); }

  static Try<AttributeConstraintPredicate> create(
      AttributeConstraint::Predicate&& predicate,
      RE2Factory* re2Factory)
  {
    using Self = AttributeConstraintPredicate;

    switch (predicate.predicate_case()) {
      case AttributeConstraint::Predicate::kExists:
        return Self(Exists{});
      case AttributeConstraint::Predicate::kNotExists:
        return Self(NotExists{});

      case AttributeConstraint::Predicate::kNonStringOrEqualsString:
        return Self(NonStringOrEqualsString{
          std::move(*predicate.mutable_non_string_or_equals_string()
                       ->mutable_value())});

      case AttributeConstraint::Predicate::kNotEqualsString:
        return Self(NotEqualsString{
          std::move(*predicate.mutable_not_equals_string()->mutable_value())});

      case AttributeConstraint::Predicate::kNonStringOrMatchesRegex: {
        shared_ptr<const RE2> re2 =
          (*re2Factory)(predicate.non_string_or_matches_regex().regex());

        Option<Error> error = validateRE2(*re2);
        if (error.isSome()) {
          return *error;
        }

        return Self(NonStringOrMatchesRegex{std::move(re2)});
      }

      case AttributeConstraint::Predicate::kNotMatchesRegex: {
        shared_ptr<const RE2> re2 =
          (*re2Factory)(predicate.not_matches_regex().regex());

        Option<Error> error = validateRE2(*re2);
        if (error.isSome()) {
          return *error;
        }

        return Self(NotMatchesRegex{std::move(re2)});
      }

      case AttributeConstraint::Predicate::PREDICATE_NOT_SET:
        return Error("Unknown predicate type");
    }

    UNREACHABLE();
  }

private:
  // The following helper structs apply the predicate using
  // overloads for the three cases:
  //   (1) Nothing   -> non existent (pseudo) attribute
  //   (2) string    -> pseudo attribute value
  //   (3) Attribute -> named attribute value
  struct Exists
  {
    bool apply(const Nothing&) const { return false; }
    bool apply(const string&) const { return true; }
    bool apply(const Attribute&) const { return true; }
  };

  struct NotExists
  {
    bool apply(const Nothing&) const { return true; }
    bool apply(const string&) const { return false; }
    bool apply(const Attribute&) const { return false; }
  };

  struct NonStringOrEqualsString
  {
    string value;

    bool apply(const Nothing&) const { return false; }
    bool apply(const string& str) const { return str == value; }
    bool apply(const Attribute& attr) const
    {
      return attr.type() != Value::TEXT || attr.text().value() == value;
    }
  };

  struct NotEqualsString
  {
    string value;

    bool apply(const Nothing&) const { return true; }
    bool apply(const string& str) const { return str != value; }
    bool apply(const Attribute& attr) const
    {
      // NOTE: For non-TEXT attributes, this predicate returns `true`,
      // like its counterpart `NonStringOrEqualsString`.
      return attr.type() != Value::TEXT || attr.text().value() != value;
    }
  };

  struct NonStringOrMatchesRegex
  {
    shared_ptr<const RE2> re2;

    bool apply(const Nothing&) const { return false; }
    bool apply(const string& str) const { return fullMatch(str, *re2); }

    bool apply(const Attribute& attr) const
    {
      return attr.type() != Value::TEXT || fullMatch(attr.text().value(), *re2);
    }
  };

  struct NotMatchesRegex
  {
    shared_ptr<const RE2> re2;

    bool apply(const Nothing&) const { return true; }
    bool apply(const string& str) const { return !fullMatch(str, *re2); }

    bool apply(const Attribute& attr) const
    {
      // NOTE: For non-TEXT attributes, this predicate returns `true`,
      // like its counterpart `NonStringOrMatchesRegex`.
      return attr.type() != Value::TEXT ||
             !fullMatch(attr.text().value(), *re2);
    }
  };


  using Predicate = Variant<
      Nothing,
      Exists,
      NotExists,
      NonStringOrEqualsString,
      NotEqualsString,
      NonStringOrMatchesRegex,
      NotMatchesRegex>;

  Predicate predicate;

  AttributeConstraintPredicate(Predicate&& p) : predicate(std::move(p)){};

  template <class T>
  bool apply_(const T& attribute) const
  {
    return predicate.visit(
        [](const Nothing& p) -> bool {
          LOG(FATAL) << "Predciate not initialized properly";
          UNREACHABLE();
        },
        [&](const Exists& p) { return p.apply(attribute); },
        [&](const NotExists& p) { return p.apply(attribute); },
        [&](const NonStringOrEqualsString& p) { return p.apply(attribute); },
        [&](const NotEqualsString& p) { return p.apply(attribute); },
        [&](const NonStringOrMatchesRegex& p) { return p.apply(attribute); },
        [&](const NotMatchesRegex& p) { return p.apply(attribute); });
  }
};


class AttributeConstraintEvaluator
{
public:
  bool evaluate(const SlaveInfo& info) const
  {
    switch (selector.selector_case()) {
      case Selector::kAttributeName: {
        const string& name = selector.attribute_name();
        const auto attr = std::find_if(
            info.attributes().cbegin(),
            info.attributes().cend(),
            [&name](const Attribute& a) { return a.name() == name; });

        return attr == info.attributes().cend() ? predicate.apply(Nothing())
                                                : predicate.apply(*attr);
      }

      case Selector::kPseudoattributeType:
        switch (selector.pseudoattribute_type()) {
          case Selector::HOSTNAME:
            return predicate.apply(info.hostname());

          case Selector::REGION:
            return info.has_domain() && info.domain().has_fault_domain()
                ? predicate.apply(info.domain().fault_domain().region().name())
                : predicate.apply(Nothing());

          case Selector::ZONE:
            return info.has_domain() && info.domain().has_fault_domain()
                ? predicate.apply(info.domain().fault_domain().zone().name())
                : predicate.apply(Nothing());

          case Selector::UNKNOWN:
            LOG(FATAL) << "Unknown pseudoattribute value passed validation";
        }

        UNREACHABLE();

      case Selector::SELECTOR_NOT_SET:
        LOG(FATAL) << "'AttributeConstraint::Selector::selector' oneof that"
                      " has no known value set passed validation";
    }

    UNREACHABLE();
  }

  static Try<AttributeConstraintEvaluator> create(
      AttributeConstraint&& constraint,
      RE2Factory* re2Factory)
  {
    Option<Error> error = validate(constraint.selector());
    if (error.isSome()) {
      return *error;
    }

    Try<AttributeConstraintPredicate> predicate =
      AttributeConstraintPredicate::create(
          std::move(*constraint.mutable_predicate()), re2Factory);

    if (predicate.isError()) {
      return Error(predicate.error());
    }

    return AttributeConstraintEvaluator{
      std::move(*constraint.mutable_selector()), std::move(*predicate)};
  }

private:
  Selector selector;
  AttributeConstraintPredicate predicate;

  AttributeConstraintEvaluator(
      Selector&& selector_,
      AttributeConstraintPredicate&& predicate_)
    : selector(std::move(selector_)),
      predicate(std::move(predicate_))
  {}
};


class OfferConstraintsFilterImpl
{
  using Group = vector<AttributeConstraintEvaluator>;

public:
  OfferConstraintsFilterImpl(
      unordered_map<string, vector<Group>>&& expressions_)
    : expressions(std::move(expressions_))
  {}

  bool isAgentExcluded(const std::string& role, const SlaveInfo& info) const
  {
    auto roleConstraintsExpression = expressions.find(role);
    if (roleConstraintsExpression == expressions.end()) {
      return false;
    }

    // TODO(asekretenko): This method evaluates the constraints in the order in
    // which they have been passed by the scheduler. Given that agents are
    // seldom added or have their (pseudo)attributes changed, tracking
    // match/mismatch frequency and runtime cost of the constraints, and
    // reordering the expression accordingly (so that the cheapest groups that
    // usually match come first, and the most expensive that usually don't come
    // last) could potentially help speed up this method.

    return !std::any_of(
        roleConstraintsExpression->second.cbegin(),
        roleConstraintsExpression->second.cend(),
        [&info](const Group& group) {
          return std::all_of(
              group.cbegin(),
              group.cend(),
              [&info](const AttributeConstraintEvaluator& e) {
                return e.evaluate(info);
              });
        });
  }

  static Try<OfferConstraintsFilterImpl> create(
    OfferConstraints&& constraints,
    RE2Factory* re2Factory)
  {
    // TODO(asekretenko): This method performs a dumb 1:1 translation of
    // `AttributeConstraint`s without any reordering; this leaves room for
    // a number of potential optimizations such as:
    //  - deactivating a framework with no constraint groups
    //  - constructing no filter if there is a single empty group
    //  - deduplicating constraints and groups
    //  - reordering constraints and groups so that the potentially cheaper ones
    //    come first
    using Group = vector<AttributeConstraintEvaluator>;
    unordered_map<string, vector<Group>> expressions;

    for (auto& pair : *constraints.mutable_role_constraints()) {
      const string& role = pair.first;
      OfferConstraints::RoleConstraints& roleConstraints = pair.second;

      if (roleConstraints.groups().empty()) {
        return Error(
            "'OfferConstraints::role_constraints' has an empty "
            "'RoleConstraints::groups' for role " +
            role);
      }

      vector<Group>& groups = expressions[role];

      for (OfferConstraints::RoleConstraints::Group& group_ :
           *roleConstraints.mutable_groups()) {
        if (group_.attribute_constraints().empty()) {
          return Error(
              "'OfferConstraints::RoleConstraints::groups' for role " + role +
              "contains an empty RoleConstraints::Group");
        }

        groups.emplace_back();
        Group& group = groups.back();

        for (AttributeConstraint& constraint :
             *group_.mutable_attribute_constraints()) {
          Try<AttributeConstraintEvaluator> evaluator =
            AttributeConstraintEvaluator::create(
                std::move(constraint), re2Factory);

          if (evaluator.isError()) {
            return Error(
                "A role " + role +
                " has an invalid 'AttributeConstraint': " + evaluator.error());
          }

          group.emplace_back(std::move(*evaluator));
        }
      }
    }

    return OfferConstraintsFilterImpl(std::move(expressions));
  }

private:
  unordered_map<string, vector<Group>> expressions;
};

} // namespace internal {


using internal::OfferConstraintsFilterImpl;


class OfferConstraintsFilter::FactoryImpl
{
public:
  Try<OfferConstraintsFilter> operator()(OfferConstraints&& constraints)
  {
    Try<OfferConstraintsFilterImpl> impl =
      OfferConstraintsFilterImpl::create(std::move(constraints), &re2Factory);

    if (impl.isError()) {
      return Error(impl.error());
    }

    return OfferConstraintsFilter(std::move(*impl));
  }
private:
  internal::RE2Factory re2Factory;
};


OfferConstraintsFilterFactory::OfferConstraintsFilterFactory()
  : impl(new OfferConstraintsFilter::FactoryImpl())
{}


OfferConstraintsFilterFactory::~OfferConstraintsFilterFactory() = default;


Try<OfferConstraintsFilter> OfferConstraintsFilterFactory::operator()(
    OfferConstraints&& constraints)
{
  return (*CHECK_NOTNULL(impl))(std::move(constraints));
}


OfferConstraintsFilter::OfferConstraintsFilter(
    OfferConstraintsFilterImpl&& impl_)
  : impl(new OfferConstraintsFilterImpl(std::move(impl_)))
{}


OfferConstraintsFilter::OfferConstraintsFilter(OfferConstraintsFilter&&) =
  default;


OfferConstraintsFilter& OfferConstraintsFilter::operator=(
    OfferConstraintsFilter&&) = default;


OfferConstraintsFilter::~OfferConstraintsFilter() = default;


bool OfferConstraintsFilter::isAgentExcluded(
    const std::string& role,
    const SlaveInfo& info) const
{
  return CHECK_NOTNULL(impl)->isAgentExcluded(role, info);
}

} // namespace allocator {
} // namespace mesos {

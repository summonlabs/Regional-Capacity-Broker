// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Stable identifier types.
//
// Every authoritative object in this boundary has a stable identity that
// survives restart and is part of the canonical encoding. Identifiers are
// validated at the edge: an identifier that reaches the broker is already
// known to be well formed, bounded and free of path-like content.

#ifndef RCB_IDENTIFIER_HPP
#define RCB_IDENTIFIER_HPP

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

#include "rcb/status.hpp"

namespace rcb {

/// A validated identifier: 1..64 ASCII characters, starting with a letter or a
/// digit, and otherwise drawn from [A-Za-z0-9._:-]. Path separators, control
/// characters, whitespace, non-ASCII bytes and the names "." and ".." are
/// rejected, as are the Windows reserved device names: this runtime never turns
/// an identifier into a path, but a consumer may, and the edge is where that is
/// cheap to refuse.
class Identifier {
 public:
  static constexpr std::size_t kMaxLength = 64;

  Identifier() = default;

  static Result<Identifier> Make(std::string_view text);

  [[nodiscard]] const std::string& value() const noexcept { return value_; }
  [[nodiscard]] bool empty() const noexcept { return value_.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return value_.size(); }
  [[nodiscard]] std::string_view view() const noexcept { return value_; }

  friend bool operator==(const Identifier& a, const Identifier& b) noexcept {
    return a.value_ == b.value_;
  }
  friend bool operator!=(const Identifier& a, const Identifier& b) noexcept { return !(a == b); }
  friend bool operator<(const Identifier& a, const Identifier& b) noexcept {
    return a.value_ < b.value_;
  }

 private:
  std::string value_;
};

/// A strong typedef over Identifier. The tag type gives each identity domain
/// its own C++ type, so a site identity cannot be passed where a service class
/// identity is required.
template <class Tag>
class TaggedId {
 public:
  TaggedId() = default;

  static Result<TaggedId> Make(std::string_view text) {
    Result<Identifier> identifier = Identifier::Make(text);
    if (!identifier.ok()) {
      return Result<TaggedId>(identifier.status());
    }
    TaggedId result;
    result.value_ = identifier.value();
    return result;
  }

  [[nodiscard]] bool empty() const noexcept { return value_.empty(); }
  [[nodiscard]] const std::string& value() const noexcept { return value_.value(); }
  [[nodiscard]] std::string_view view() const noexcept { return value_.view(); }

  friend bool operator==(const TaggedId& a, const TaggedId& b) noexcept { return a.value_ == b.value_; }
  friend bool operator!=(const TaggedId& a, const TaggedId& b) noexcept { return !(a == b); }
  friend bool operator<(const TaggedId& a, const TaggedId& b) noexcept { return a.value_ < b.value_; }

 private:
  Identifier value_;
};

struct SiteIdTag {};
struct ServiceClassIdTag {};
struct RegionIdTag {};
struct JurisdictionIdTag {};
struct FailureDomainIdTag {};
struct SnapshotIdTag {};
struct PolicyIdTag {};
struct EvidenceIdTag {};
struct RequesterIdTag {};
struct AskKeyTag {};
struct DecisionIdTag {};
struct CommitmentIdTag {};

using SiteId = TaggedId<SiteIdTag>;
using ServiceClassId = TaggedId<ServiceClassIdTag>;
using RegionId = TaggedId<RegionIdTag>;
using JurisdictionId = TaggedId<JurisdictionIdTag>;
using FailureDomainId = TaggedId<FailureDomainIdTag>;
using SnapshotId = TaggedId<SnapshotIdTag>;
using PolicyId = TaggedId<PolicyIdTag>;
using EvidenceId = TaggedId<EvidenceIdTag>;
using RequesterId = TaggedId<RequesterIdTag>;
using AskKey = TaggedId<AskKeyTag>;
using DecisionId = TaggedId<DecisionIdTag>;
using CommitmentId = TaggedId<CommitmentIdTag>;

}  // namespace rcb

#endif  // RCB_IDENTIFIER_HPP

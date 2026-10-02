// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Regional Capacity Broker -- version identity.

#ifndef RCB_VERSION_HPP
#define RCB_VERSION_HPP

#include <cstdint>
#include <string>
#include <string_view>

namespace rcb {

/// Semantic version of the runtime, as compiled into the library.
inline constexpr std::uint32_t kVersionMajor = 1;
inline constexpr std::uint32_t kVersionMinor = 0;
inline constexpr std::uint32_t kVersionPatch = 0;

/// Version of the on-disk persistence format. Bumped only for a change that an
/// older reader could not interpret; see docs/persistence-format.md.
inline constexpr std::uint32_t kPersistenceFormatVersion = 1;

/// Version of the canonical request document schema accepted on the CLI and by
/// the JSON front end.
inline constexpr std::uint32_t kRequestSchemaVersion = 1;

/// "major.minor.patch".
std::string VersionString();

/// Version of the library as a compile-time string, for consumers that embed it.
std::string_view VersionStringView();

}  // namespace rcb

#endif  // RCB_VERSION_HPP

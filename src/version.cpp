// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "rcb/version.hpp"

namespace rcb {
namespace {

// The literal is kept here so that a consumer reading the binary (or a test
// comparing the header constants against the compiled string) sees one source
// of truth: this file.
constexpr std::string_view kVersionLiteral = "1.0.0";

}  // namespace

std::string VersionString() { return std::string(kVersionLiteral); }

std::string_view VersionStringView() { return kVersionLiteral; }

}  // namespace rcb

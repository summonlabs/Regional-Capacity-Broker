// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "rcb/status.hpp"

namespace rcb {

std::string Status::ToString() const {
  if (ok()) {
    return "ok";
  }
  std::string text(ErrorToken(code_));
  if (!detail_.empty()) {
    text += ": ";
    text += detail_;
  }
  return text;
}

}  // namespace rcb

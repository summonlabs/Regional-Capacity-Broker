// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Foundation suite: checked arithmetic, exact fixed point, digests, identifiers,
// capacity vectors and the strict JSON front end.

#include <limits>
#include <string>

#include "rcb/rcb.hpp"
#include "test_framework.hpp"
#include "test_main.hpp"

namespace {

using rcb::ErrorCode;
using rcb::i64;
using rcb::u64;

}  // namespace

RCB_TEST(checked_addition_detects_overflow) {
  i64 out = 0;
  RCB_CHECK(rcb::TryAdd<i64>(1, 2, &out));
  RCB_CHECK_EQ(out, i64{3});
  RCB_CHECK(!rcb::TryAdd<i64>(std::numeric_limits<i64>::max(), 1, &out));
  RCB_CHECK(!rcb::TryAdd<i64>(std::numeric_limits<i64>::min(), -1, &out));
  RCB_CHECK(rcb::TryAdd<i64>(std::numeric_limits<i64>::max(), i64{0}, &out));
  RCB_CHECK_EQ(out, std::numeric_limits<i64>::max());
  RCB_CHECK(rcb::TryAdd<i64>(std::numeric_limits<i64>::min(), i64{1}, &out));
  RCB_CHECK_EQ(out, std::numeric_limits<i64>::min() + 1);

  u64 unsigned_out = 0;
  RCB_CHECK(rcb::TryAdd<u64>(1, 1, &unsigned_out));
  RCB_CHECK_EQ(unsigned_out, u64{2});
  RCB_CHECK(!rcb::TryAdd<u64>(std::numeric_limits<u64>::max(), 1, &unsigned_out));

  RCB_CHECK_ERROR(rcb::AddChecked(std::numeric_limits<i64>::max(), 1), ErrorCode::Overflow);
  RCB_CHECK_ERROR(rcb::AddCheckedU64(std::numeric_limits<u64>::max(), 1), ErrorCode::Overflow);
}

RCB_TEST(checked_subtraction_and_multiplication_detect_overflow) {
  i64 out = 0;
  RCB_CHECK(rcb::TrySub<i64>(5, 3, &out));
  RCB_CHECK_EQ(out, i64{2});
  RCB_CHECK(!rcb::TrySub<i64>(std::numeric_limits<i64>::min(), 1, &out));
  RCB_CHECK(rcb::TrySub<i64>(0, 1, &out));
  RCB_CHECK_EQ(out, i64{-1});

  u64 unsigned_out = 0;
  RCB_CHECK(!rcb::TrySub<u64>(0, 1, &unsigned_out));
  RCB_CHECK(rcb::TrySub<u64>(3, 1, &unsigned_out));
  RCB_CHECK_EQ(unsigned_out, u64{2});

  RCB_CHECK(rcb::TryMul<i64>(6, 7, &out));
  RCB_CHECK_EQ(out, i64{42});
  RCB_CHECK(rcb::TryMul<i64>(-6, 7, &out));
  RCB_CHECK_EQ(out, i64{-42});
  RCB_CHECK(rcb::TryMul<i64>(-6, -7, &out));
  RCB_CHECK_EQ(out, i64{42});
  RCB_CHECK(rcb::TryMul<i64>(0, std::numeric_limits<i64>::max(), &out));
  RCB_CHECK_EQ(out, i64{0});
  RCB_CHECK(!rcb::TryMul<i64>(std::numeric_limits<i64>::max(), 2, &out));
  RCB_CHECK(!rcb::TryMul<i64>(std::numeric_limits<i64>::min(), -1, &out));
  RCB_CHECK(rcb::TryMul<i64>(std::numeric_limits<i64>::max(), 1, &out));
  RCB_CHECK_EQ(out, std::numeric_limits<i64>::max());
  RCB_CHECK(rcb::TryMul<i64>(std::numeric_limits<i64>::min(), 1, &out));
  RCB_CHECK_EQ(out, std::numeric_limits<i64>::min());

  RCB_CHECK_ERROR(rcb::MulChecked(std::numeric_limits<i64>::max(), 3), ErrorCode::Overflow);
}

RCB_TEST(mul_div_is_exact_and_overflow_checked) {
  RCB_CHECK_EQ(rcb::MulDivI64(10, 10, 4).ValueOr(-1), i64{25});
  u64 remainder = 0;
  RCB_CHECK_EQ(rcb::MulDivU64(10, 10, 3, &remainder).ValueOr(u64{0}), u64{33});
  RCB_CHECK_EQ(remainder, u64{1});
  RCB_CHECK_ERROR(rcb::MulDivU64(1, 1, 0), ErrorCode::OutOfRange);
  RCB_CHECK_ERROR(rcb::MulDivI64(-1, 5, 2), ErrorCode::InvalidArgument);
  RCB_CHECK_ERROR(rcb::MulDivI64(1, 5, 0), ErrorCode::InvalidArgument);
  RCB_CHECK_EQ(rcb::MulDivU64(0, 5, 3).ValueOr(u64{7}), u64{0});

  // A product that only a 128-bit intermediate holds exactly.
  const u64 big = 1ULL << 40U;
  RCB_CHECK_EQ(rcb::MulDivU64(big, big, big).ValueOr(u64{0}), big);

  // A quotient that does not fit must be refused, never wrapped.
  RCB_CHECK_ERROR(rcb::MulDivU64(std::numeric_limits<u64>::max(), 2, 1), ErrorCode::Overflow);

#if defined(__SIZEOF_INT128__)
  // Reference model: the same computation through a 128-bit integer. As in
  // src/checked.cpp this is the one place where the compiler extension has to be
  // spelled, so the pedantic warning about the spelling is suppressed locally.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wpedantic"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
  for (u64 a = 1; a < 4096; a += 211) {
    for (u64 b = 1; b < 4096; b += 307) {
      const unsigned __int128 product = static_cast<unsigned __int128>(a) * b;
      const u64 divisor = ((a + b) % 977U) + 1U;
      const u64 expected = static_cast<u64>(product / divisor);
      u64 actual_remainder = 0;
      const rcb::Result<u64> actual = rcb::MulDivU64(a, b, divisor, &actual_remainder);
      RCB_REQUIRE(actual.ok());
      RCB_CHECK_EQ(actual.value(), expected);
      RCB_CHECK_EQ(actual_remainder, static_cast<u64>(product % divisor));
    }
  }
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#endif
}

RCB_TEST(sum_checked_enforces_the_bound) {
  const i64 values[4] = {1, 2, 3, 4};
  RCB_CHECK_EQ(rcb::SumChecked(values, 4, 100).ValueOr(-1), i64{10});
  RCB_CHECK_ERROR(rcb::SumChecked(values, 4, 9), ErrorCode::LimitExceeded);
  const i64 negative[1] = {-1};
  RCB_CHECK_ERROR(rcb::SumChecked(negative, 1, 100), ErrorCode::InvalidArgument);
}

RCB_TEST(checked_cast_refuses_to_truncate) {
  RCB_CHECK_EQ(rcb::CheckedCast<int>(static_cast<i64>(7)).ValueOr(-1), 7);
  RCB_CHECK_ERROR(rcb::CheckedCast<int>(static_cast<i64>(1) << 40), ErrorCode::OutOfRange);
  RCB_CHECK_ERROR(rcb::CheckedCast<unsigned>(static_cast<i64>(-1)), ErrorCode::OutOfRange);
  RCB_CHECK_EQ(rcb::CheckedCast<u64>(static_cast<i64>(9)).ValueOr(u64{0}), u64{9});
}

RCB_TEST(fixed_point_parses_exactly) {
  RCB_CHECK_EQ(rcb::ScaledAmount::Parse("12.5").ValueOr(rcb::ScaledAmount()).micros(),
               i64{12500000});
  RCB_CHECK_EQ(rcb::ScaledAmount::Parse("0").ValueOr(rcb::ScaledAmount()).micros(), i64{0});
  RCB_CHECK_EQ(rcb::ScaledAmount::Parse("-0.000001").ValueOr(rcb::ScaledAmount()).micros(),
               i64{-1});
  RCB_CHECK_EQ(rcb::ScaledAmount::Parse("+3").ValueOr(rcb::ScaledAmount()).micros(), i64{3000000});
  RCB_CHECK_EQ(rcb::ScaledAmount::Parse("0.0000001").status().code(), ErrorCode::OutOfRange);
  RCB_CHECK_EQ(rcb::ScaledAmount::Parse("1e5").status().code(), ErrorCode::MalformedInput);
  RCB_CHECK_EQ(rcb::ScaledAmount::Parse("1.2.3").status().code(), ErrorCode::MalformedInput);
  RCB_CHECK_EQ(rcb::ScaledAmount::Parse("").status().code(), ErrorCode::MalformedInput);
  RCB_CHECK_EQ(rcb::ScaledAmount::Parse(".").status().code(), ErrorCode::MalformedInput);
  RCB_CHECK_EQ(rcb::ScaledAmount::Parse("1.").status().code(), ErrorCode::MalformedInput);
  RCB_CHECK_EQ(rcb::ScaledAmount::Parse(" 1").status().code(), ErrorCode::MalformedInput);
  // Sixteen whole digits is out of range; fifteen digits that overflow when
  // scaled to micro-units is an arithmetic overflow.
  RCB_CHECK_EQ(rcb::ScaledAmount::Parse("9999999999999999").status().code(),
               ErrorCode::OutOfRange);
  RCB_CHECK_EQ(rcb::ScaledAmount::Parse("999999999999999").status().code(), ErrorCode::Overflow);

  RCB_CHECK_EQ(rcb::ScaledAmount::Parse("12.5").ValueOr(rcb::ScaledAmount()).ToString(),
               std::string("12.5"));
  RCB_CHECK_EQ(rcb::ScaledAmount::Parse("-0.000001").ValueOr(rcb::ScaledAmount()).ToString(),
               std::string("-0.000001"));
  RCB_CHECK_EQ(rcb::ScaledAmount::FromUnits(2, 250000).ValueOr(rcb::ScaledAmount()).ToString(),
               std::string("2.25"));
  RCB_CHECK_EQ(rcb::ScaledAmount::FromMicros(0).ValueOr(rcb::ScaledAmount()).ToString(),
               std::string("0"));

  const rcb::ScaledAmount price = rcb::ScaledAmount::Parse("0.0004").ValueOr(rcb::ScaledAmount());
  RCB_CHECK_EQ(price.ScaleBy(1000).ValueOr(rcb::ScaledAmount()).micros(), i64{400000});
  RCB_CHECK_EQ(price.ScaleBy(1000).ValueOr(rcb::ScaledAmount()).ToString(), std::string("0.4"));
  RCB_CHECK_ERROR(price.ScaleBy(-1), ErrorCode::InvalidArgument);
  const rcb::ScaledAmount huge = rcb::ScaledAmount::Parse("1000000").ValueOr(rcb::ScaledAmount());
  RCB_CHECK_ERROR(huge.ScaleBy(1LL << 40), ErrorCode::Overflow);
  RCB_CHECK_ERROR(rcb::ScaledAmount::FromMicros(rcb::ScaledAmount::kMaxMicros + 1),
                  ErrorCode::OutOfRange);
}

RCB_TEST(sha256_matches_published_vectors) {
  RCB_CHECK_EQ(rcb::Sha256Of(std::string_view("")).Hex(),
               std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
  RCB_CHECK_EQ(rcb::Sha256Of(std::string_view("abc")).Hex(),
               std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  RCB_CHECK_EQ(
      rcb::Sha256Of(std::string_view("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))
          .Hex(),
      std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));

  const std::string million(1000000, 'a');
  RCB_CHECK_EQ(rcb::Sha256Of(std::string_view(million)).Hex(),
               std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));

  rcb::Sha256 incremental;
  for (int i = 0; i < 1000; ++i) {
    incremental.Update(std::string_view("a"));
  }
  RCB_CHECK_EQ(incremental.Finalize().Hex(),
               std::string("41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3"));

  const rcb::Digest digest = rcb::Sha256Of(std::string_view("abc"));
  RCB_CHECK_EQ(rcb::Digest::FromHex(digest.Hex()).ValueOr(rcb::Digest()), digest);
  RCB_CHECK_EQ(digest.ShortHex(8), std::string("ba7816bf"));
  RCB_CHECK_EQ(rcb::Digest::FromHex("abc").status().code(), ErrorCode::InvalidArgument);
  RCB_CHECK_EQ(rcb::Digest::FromHex(std::string(64, 'z')).status().code(),
               ErrorCode::InvalidArgument);
}

RCB_TEST(crc32c_matches_published_vectors) {
  const auto crc_of = [](const std::string& text) {
    return rcb::Crc32c(
        std::span<const rcb::u8>(reinterpret_cast<const rcb::u8*>(text.data()), text.size()));
  };
  RCB_CHECK_EQ(crc_of(""), rcb::u32{0});
  RCB_CHECK_EQ(crc_of("123456789"), rcb::u32{0xE3069283U});
  RCB_CHECK_EQ(crc_of("The quick brown fox jumps over the lazy dog"), rcb::u32{0x22620404U});

  rcb::Crc32cStream stream;
  const std::string part = "123456789";
  stream.Update(std::span<const rcb::u8>(reinterpret_cast<const rcb::u8*>(part.data()), 4));
  stream.Update(std::span<const rcb::u8>(reinterpret_cast<const rcb::u8*>(part.data()) + 4, 5));
  RCB_CHECK_EQ(stream.value(), rcb::u32{0xE3069283U});
}

RCB_TEST(identifiers_are_validated_at_the_edge) {
  RCB_CHECK(rcb::SiteId::Make("site-a").ok());
  RCB_CHECK(rcb::SiteId::Make("S1").ok());
  RCB_CHECK(rcb::SiteId::Make("a.b_c:d-e").ok());
  RCB_CHECK_EQ(rcb::SiteId::Make("").status().code(), ErrorCode::InvalidIdentifier);
  RCB_CHECK_EQ(rcb::SiteId::Make("-leading").status().code(), ErrorCode::InvalidIdentifier);
  RCB_CHECK_EQ(rcb::SiteId::Make("has space").status().code(), ErrorCode::InvalidIdentifier);
  RCB_CHECK_EQ(rcb::SiteId::Make("path/sep").status().code(), ErrorCode::InvalidIdentifier);
  RCB_CHECK_EQ(rcb::SiteId::Make("back\\slash").status().code(), ErrorCode::InvalidIdentifier);
  RCB_CHECK_EQ(rcb::SiteId::Make("..").status().code(), ErrorCode::InvalidIdentifier);
  RCB_CHECK_EQ(rcb::SiteId::Make(".").status().code(), ErrorCode::InvalidIdentifier);
  RCB_CHECK_EQ(rcb::SiteId::Make("CON").status().code(), ErrorCode::InvalidIdentifier);
  RCB_CHECK_EQ(rcb::SiteId::Make("com1.txt").status().code(), ErrorCode::InvalidIdentifier);
  RCB_CHECK_EQ(rcb::SiteId::Make(std::string(65, 'a')).status().code(),
               ErrorCode::InvalidIdentifier);
  RCB_CHECK(rcb::SiteId::Make(std::string(64, 'a')).ok());
  RCB_CHECK_EQ(rcb::SiteId::Make("nul\u00e9").status().code(), ErrorCode::InvalidIdentifier);
  RCB_CHECK_EQ(rcb::SiteId::Make("tab\there").status().code(), ErrorCode::InvalidIdentifier);
}

RCB_TEST(capacity_vectors_are_bounded_and_exact) {
  RCB_CHECK(rcb::CapacityVector::Make({1, 2, 3, 4}).ok());
  RCB_CHECK_ERROR(rcb::CapacityVector::Make({-1, 0, 0, 0}), ErrorCode::InvalidArgument);
  RCB_CHECK_ERROR(rcb::CapacityVector::Make({rcb::Limits::kMaxDimensionValue + 1, 0, 0, 0}),
                  ErrorCode::OutOfRange);
  const rcb::CapacityVector a =
      rcb::CapacityVector::Make({10, 20, 30, 40}).ValueOr(rcb::CapacityVector());
  const rcb::CapacityVector b =
      rcb::CapacityVector::Make({1, 2, 3, 4}).ValueOr(rcb::CapacityVector());
  RCB_CHECK_EQ(rcb::CapacityVector::Add(a, b).ValueOr(rcb::CapacityVector()).Get(
                   rcb::Dimension::Power),
               i64{11});
  RCB_CHECK_EQ(rcb::CapacityVector::Sub(a, b).ValueOr(rcb::CapacityVector()).Get(
                   rcb::Dimension::Cooling),
               i64{18});
  RCB_CHECK_EQ(rcb::CapacityVector::Sub(b, a).ValueOr(rcb::CapacityVector()).Get(
                   rcb::Dimension::Power),
               i64{-9});
  RCB_CHECK_ERROR(rcb::CapacityVector::SubNonNegative(b, a), ErrorCode::InvariantViolation);
  RCB_CHECK(a.Covers(b));
  RCB_CHECK(!b.Covers(a));
  RCB_CHECK_EQ(a.Min(b).Get(rcb::Dimension::RackSpace), i64{3});
  RCB_CHECK_EQ(a.Max(b).Get(rcb::Dimension::RackSpace), i64{30});
  RCB_CHECK_EQ(a.ScalarTotal().ValueOr(-1), i64{100});
  RCB_CHECK_EQ(a.ToString(),
               std::string("power=10,cooling=20,rack_space=30,service_capacity=40"));
  RCB_CHECK_EQ(rcb::CapacityVector::FromDimension(rcb::Dimension::Power, 5)
                   .ValueOr(rcb::CapacityVector())
                   .Get(rcb::Dimension::Power),
               i64{5});
  RCB_CHECK(!a.IsZero());
  RCB_CHECK(rcb::CapacityVector().IsZero());
  RCB_CHECK_ERROR(rcb::CapacityVector::Add(a, a, 10), ErrorCode::LimitExceeded);
}

RCB_TEST(json_parses_the_accepted_subset) {
  const rcb::Result<rcb::JsonValue> object = rcb::ParseJson(R"({"b":1,"a":[true,null,"x"]})");
  RCB_REQUIRE(object.ok());
  RCB_CHECK(object.value().IsObject());
  RCB_CHECK_EQ(object.value().members().size(), std::size_t{2});
  // Canonical rendering sorts keys, so input order never reaches a digest.
  RCB_CHECK_EQ(object.value().ToText(false), std::string(R"({"a":[true,null,"x"],"b":1})"));

  const rcb::Result<rcb::JsonValue> escaped = rcb::ParseJson(R"("a\"b\\c\n\u0041\u00e9")");
  RCB_REQUIRE(escaped.ok());
  RCB_CHECK_EQ(escaped.value().AsString(), std::string("a\"b\\c\nA\u00e9"));

  const rcb::Result<rcb::JsonValue> surrogate = rcb::ParseJson(R"("\ud83d\ude00")");
  RCB_REQUIRE(surrogate.ok());
  RCB_CHECK_EQ(surrogate.value().AsString(), std::string("\xf0\x9f\x98\x80"));

  const rcb::Result<rcb::JsonValue> negative = rcb::ParseJson("-9223372036854775808");
  RCB_REQUIRE(negative.ok());
  RCB_CHECK_EQ(negative.value().AsInt(), std::numeric_limits<i64>::min());

  const rcb::Result<rcb::JsonValue> big = rcb::ParseJson("18446744073709551615");
  RCB_REQUIRE(big.ok());
  RCB_CHECK(big.value().IsUint());
}

RCB_TEST(json_refuses_what_it_should) {
  const auto code_of = [](const std::string& text) {
    return rcb::ParseJson(text).status().code();
  };
  RCB_CHECK_EQ(code_of("1.5"), ErrorCode::MalformedInput);
  RCB_CHECK_EQ(code_of("1e5"), ErrorCode::MalformedInput);
  RCB_CHECK_EQ(code_of("01"), ErrorCode::MalformedInput);
  RCB_CHECK_EQ(code_of("+1"), ErrorCode::MalformedInput);
  RCB_CHECK_EQ(code_of("{\"a\":1,\"a\":2}"), ErrorCode::DuplicateField);
  RCB_CHECK_EQ(code_of("{\"a\":1} trailing"), ErrorCode::MalformedInput);
  RCB_CHECK_EQ(code_of(""), ErrorCode::MalformedInput);
  RCB_CHECK_EQ(code_of("["), ErrorCode::MalformedInput);
  RCB_CHECK_EQ(code_of("[1,]"), ErrorCode::MalformedInput);
  RCB_CHECK_EQ(code_of("\"unterminated"), ErrorCode::MalformedInput);
  RCB_CHECK_EQ(code_of("\"raw\x01control\""), ErrorCode::MalformedInput);
  RCB_CHECK_EQ(code_of("\"\\ud800\""), ErrorCode::InvalidUnicode);
  RCB_CHECK_EQ(code_of("\"\\udc00\""), ErrorCode::InvalidUnicode);
  RCB_CHECK_EQ(code_of("\"\\ud800\\u0041\""), ErrorCode::InvalidUnicode);
  RCB_CHECK_EQ(code_of("\"\\q\""), ErrorCode::MalformedInput);
  RCB_CHECK_EQ(code_of("18446744073709551616"), ErrorCode::OutOfRange);
  RCB_CHECK_EQ(code_of("nul"), ErrorCode::MalformedInput);
  RCB_CHECK_EQ(code_of("\"\xff\xfe\""), ErrorCode::InvalidUnicode);
  RCB_CHECK_EQ(code_of("\"\xc0\x80\""), ErrorCode::InvalidUnicode);

  // Depth and size limits are applied before anything is allocated.
  std::string deep(200, '[');
  deep.append(200, ']');
  RCB_CHECK_EQ(code_of(deep), ErrorCode::LimitExceeded);
  std::string wide = "[";
  for (int i = 0; i < 50; ++i) {
    wide += "1,";
  }
  wide += "1]";
  rcb::JsonLimits tiny;
  tiny.max_elements = 4;
  RCB_CHECK_EQ(rcb::ParseJson(wide, tiny).status().code(), ErrorCode::LimitExceeded);
  rcb::JsonLimits small_bytes;
  small_bytes.max_bytes = 3;
  RCB_CHECK_EQ(rcb::ParseJson("{\"a\":1}", small_bytes).status().code(),
               ErrorCode::LimitExceeded);
}

RCB_TEST(utf8_validation_is_strict) {
  RCB_CHECK(rcb::IsValidUtf8("plain ascii"));
  RCB_CHECK(rcb::IsValidUtf8("\xc3\xa9"));
  RCB_CHECK(rcb::IsValidUtf8("\xf0\x9f\x98\x80"));
  RCB_CHECK(!rcb::IsValidUtf8("\xc0\x80"));
  RCB_CHECK(!rcb::IsValidUtf8("\xed\xa0\x80"));
  RCB_CHECK(!rcb::IsValidUtf8("\xf5\x80\x80\x80"));
  RCB_CHECK(!rcb::IsValidUtf8("\x80"));
  RCB_CHECK(!rcb::IsValidUtf8("\xc3"));
}

RCB_TEST(error_taxonomy_is_stable) {
  RCB_CHECK_EQ(rcb::ErrorToken(ErrorCode::StaleGeneration), std::string_view("stale_generation"));
  RCB_CHECK_EQ(rcb::ErrorToken(ErrorCode::InsufficientCapacity),
               std::string_view("insufficient_capacity"));
  RCB_CHECK_EQ(rcb::ErrorCategoryOf(ErrorCode::InsufficientCapacity), rcb::ErrorCategory::Unmet);
  RCB_CHECK_EQ(rcb::ErrorCategoryOf(ErrorCode::PersistenceInteriorCorruption),
               rcb::ErrorCategory::Persistence);
  RCB_CHECK(rcb::IsConstraintFailure(ErrorCode::CostCeilingExceeded));
  RCB_CHECK(!rcb::IsConstraintFailure(ErrorCode::MalformedInput));
  RCB_CHECK(rcb::IsRetryable(ErrorCode::QueueFull));
  RCB_CHECK(!rcb::IsRetryable(ErrorCode::InvalidArgument));
  RCB_CHECK_EQ(std::string(rcb::Status::Error(ErrorCode::Cancelled, "x").ToString()),
               std::string("cancelled: x"));
  RCB_CHECK(rcb::Status().ok());
  RCB_CHECK_EQ(std::string(rcb::VersionString()), std::string("1.0.0"));
}

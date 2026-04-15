#include <gtest/gtest.h>
#include <picojson.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

#include <xgrammar/compiler.h>
#include <xgrammar/grammar.h>
#include <xgrammar/tokenizer_info.h>

#include "fsm.h"
#include "grammar_impl.h"
#include "support/compact_2d_array.h"
#include "support/dynamic_bitset.h"
#include "support/json_serializer.h"
#include "support/memory_size.h"
#include "tokenizer_info_impl.h"

namespace xgrammar {

bool operator==(const Compact2DArray<int>& lhs, const Compact2DArray<int>& rhs) {
  if (lhs.size() != rhs.size()) return false;
  const std::size_t indptr_size = lhs.size() + 1;
  const auto* lhs_indptr = lhs.indptr();
  const auto* rhs_indptr = rhs.indptr();
  for (std::size_t i = 0; i < indptr_size; ++i) {
    if (lhs_indptr[i] != rhs_indptr[i]) return false;
  }
  const auto data_size = *std::max_element(lhs_indptr, lhs_indptr + indptr_size);
  const auto* lhs_data = lhs.data();
  const auto* rhs_data = rhs.data();
  for (int i = 0; i < int(data_size); ++i) {
    if (lhs_data[i] != rhs_data[i]) return false;
  }
  return true;
}

bool operator==(const Compact2DArray<FSMEdge>& lhs, const Compact2DArray<FSMEdge>& rhs) {
  if (lhs.size() != rhs.size()) return false;
  const std::size_t indptr_size = lhs.size() + 1;
  const auto* lhs_indptr = lhs.indptr();
  const auto* rhs_indptr = rhs.indptr();
  for (std::size_t i = 0; i < indptr_size; ++i) {
    if (lhs_indptr[i] != rhs_indptr[i]) return false;
  }
  const auto data_size = *std::max_element(lhs_indptr, lhs_indptr + indptr_size);
  const auto* lhs_data = lhs.data();
  const auto* rhs_data = rhs.data();
  for (int i = 0; i < int(data_size); ++i) {
    if (!(lhs_data[i] == rhs_data[i])) return false;
  }
  return true;
}

}  // namespace xgrammar

TEST(XGrammarSerializationTest, TestSTLAndBuiltinTypes) {
  using namespace xgrammar;

  // Test basic types
  {
    bool value = true;
    auto json_value = AutoSerializeJSONValue(value);
    ASSERT_TRUE(json_value.is<bool>());
    ASSERT_EQ(json_value.get<bool>(), true);

    // Test literal string comparison
    std::string expected = "true";
    ASSERT_EQ(json_value.serialize(), expected);

    bool deserialized = false;
    auto error = AutoDeserializeJSONValue(&deserialized, json_value);
    ASSERT_FALSE(error.has_value());
    ASSERT_EQ(deserialized, value);

    // Test roundtrip
    auto json_value2 = AutoSerializeJSONValue(deserialized);
    ASSERT_EQ(json_value.serialize(), json_value2.serialize());
  }

  {
    int value = 42;
    auto json_value = AutoSerializeJSONValue(value);
    ASSERT_TRUE(json_value.is<int64_t>());
    ASSERT_EQ(json_value.get<int64_t>(), 42);

    // Test literal string comparison
    std::string expected = "42";
    ASSERT_EQ(json_value.serialize(), expected);

    int deserialized = 0;
    auto error = AutoDeserializeJSONValue(&deserialized, json_value);
    ASSERT_FALSE(error.has_value());
    ASSERT_EQ(deserialized, value);

    // Test roundtrip
    auto json_value2 = AutoSerializeJSONValue(deserialized);
    ASSERT_EQ(json_value.serialize(), json_value2.serialize());
  }

  {
    double value = 3.14;
    auto json_value = AutoSerializeJSONValue(value);
    ASSERT_TRUE(json_value.is<double>());
    ASSERT_EQ(json_value.get<double>(), 3.14);

    // Test literal string comparison
    // due to precision, we can't compare strings directly
    // because it might serialize as "3.1400000000000001" or similar
    // so we compare the numeric value instead
    std::string expected = "3.14";
    ASSERT_EQ(std::stod(json_value.serialize()), std::stod(expected));

    double deserialized = 0.0;
    auto error = AutoDeserializeJSONValue(&deserialized, json_value);
    ASSERT_FALSE(error.has_value());
    ASSERT_EQ(deserialized, value);

    // Test roundtrip
    auto json_value2 = AutoSerializeJSONValue(deserialized);
    ASSERT_EQ(json_value.serialize(), json_value2.serialize());
  }

  {
    std::string value = "hello";
    auto json_value = AutoSerializeJSONValue(value);
    ASSERT_TRUE(json_value.is<std::string>());
    ASSERT_EQ(json_value.get<std::string>(), "hello");

    // Test literal string comparison
    std::string expected = "\"hello\"";
    ASSERT_EQ(json_value.serialize(), expected);

    std::string deserialized;
    auto error = AutoDeserializeJSONValue(&deserialized, json_value);
    ASSERT_FALSE(error.has_value());
    ASSERT_EQ(deserialized, value);

    // Test roundtrip
    auto json_value2 = AutoSerializeJSONValue(deserialized);
    ASSERT_EQ(json_value.serialize(), json_value2.serialize());
  }

  {
    std::string value("\0A\xff", 3);
    auto json_value = AutoSerializeJSONValue(value);
    ASSERT_TRUE(json_value.is<std::string>());

    std::string deserialized;
    auto error = AutoDeserializeJSONValue(&deserialized, json_value);
    ASSERT_FALSE(error.has_value());
    ASSERT_EQ(deserialized.size(), value.size());
    ASSERT_EQ(deserialized, value);
  }

  // Test containers
  {
    std::vector<int> value = {1, 2, 3};
    auto json_value = AutoSerializeJSONValue(value);
    ASSERT_TRUE(json_value.is<picojson::array>());

    // Test literal string comparison
    std::string expected = "[1,2,3]";
    ASSERT_EQ(json_value.serialize(), expected);

    std::vector<int> deserialized;
    auto error = AutoDeserializeJSONValue(&deserialized, json_value);
    ASSERT_FALSE(error.has_value());
    ASSERT_EQ(deserialized, value);

    // Test roundtrip
    auto json_value2 = AutoSerializeJSONValue(deserialized);
    ASSERT_EQ(json_value.serialize(), json_value2.serialize());
  }

  {
    std::unordered_set<int> value = {1, 2, 3};
    auto json_value = AutoSerializeJSONValue(value);
    ASSERT_TRUE(json_value.is<picojson::array>());

    // Test literal string comparison (sorted due to unordered_set)
    std::string expected = "[1,2,3]";
    ASSERT_EQ(json_value.serialize(), expected);

    std::unordered_set<int> deserialized;
    auto error = AutoDeserializeJSONValue(&deserialized, json_value);
    ASSERT_FALSE(error.has_value());
    ASSERT_EQ(deserialized, value);

    // Test roundtrip
    auto json_value2 = AutoSerializeJSONValue(deserialized);
    ASSERT_EQ(json_value.serialize(), json_value2.serialize());
  }

  {
    std::pair<int, std::string> value = {42, "hello"};
    auto json_value = AutoSerializeJSONValue(value);
    ASSERT_TRUE(json_value.is<picojson::array>());

    // Test literal string comparison
    std::string expected = "[42,\"hello\"]";
    ASSERT_EQ(json_value.serialize(), expected);

    std::pair<int, std::string> deserialized;
    auto error = AutoDeserializeJSONValue(&deserialized, json_value);
    ASSERT_FALSE(error.has_value());
    ASSERT_EQ(deserialized, value);

    // Test roundtrip
    auto json_value2 = AutoSerializeJSONValue(deserialized);
    ASSERT_EQ(json_value.serialize(), json_value2.serialize());
  }

  // Test optional
  {
    std::optional<int> value = 42;
    auto json_value = AutoSerializeJSONValue(value);
    ASSERT_TRUE(json_value.is<int64_t>());
    ASSERT_EQ(json_value.get<int64_t>(), 42);

    // Test literal string comparison
    std::string expected = "42";
    ASSERT_EQ(json_value.serialize(), expected);

    std::optional<int> deserialized;
    auto error = AutoDeserializeJSONValue(&deserialized, json_value);
    ASSERT_FALSE(error.has_value());
    ASSERT_TRUE(deserialized.has_value());
    ASSERT_EQ(*deserialized, 42);

    // Test roundtrip
    auto json_value2 = AutoSerializeJSONValue(deserialized);
    ASSERT_EQ(json_value.serialize(), json_value2.serialize());
  }

  {
    std::optional<int> value;
    auto json_value = AutoSerializeJSONValue(value);
    ASSERT_TRUE(json_value.is<picojson::null>());

    // Test literal string comparison
    std::string expected = "null";
    ASSERT_EQ(json_value.serialize(), expected);

    std::optional<int> deserialized = 999;
    auto error = AutoDeserializeJSONValue(&deserialized, json_value);
    ASSERT_FALSE(error.has_value());
    ASSERT_FALSE(deserialized.has_value());

    // Test roundtrip
    auto json_value2 = AutoSerializeJSONValue(deserialized);
    ASSERT_EQ(json_value.serialize(), json_value2.serialize());
  }
}

TEST(XGrammarSerializationTest, TestCompiledGrammarMemorySizeAvoidsSharedFSMDoubleCount) {
  using namespace xgrammar;

  auto compiled =
      GrammarCompiler(TokenizerInfo(std::vector<std::string>{}), 1, false).CompileBuiltinJSONGrammar();
  const auto* grammar_impl = compiled.GetGrammar().ImplPtr();

  const int per_rule_fsm_count = static_cast<int>(std::count_if(
      grammar_impl->per_rule_fsms.begin(),
      grammar_impl->per_rule_fsms.end(),
      [](const auto& fsm) { return fsm.has_value(); }
  ));
  ASSERT_GT(per_rule_fsm_count, 10);
  EXPECT_TRUE(grammar_impl->per_rule_fsm_hashes.empty());
  EXPECT_TRUE(grammar_impl->per_rule_fsm_new_state_ids.empty());

  const std::size_t complete_fsm_bytes = MemorySize(grammar_impl->complete_fsm);
  ASSERT_GT(complete_fsm_bytes, 0U);
  EXPECT_LT(
      compiled.MemorySizeBytes(),
      complete_fsm_bytes * static_cast<std::size_t>(per_rule_fsm_count) * 3 / 2
  );
}

TEST(XGrammarSerializationTest, TestString) {
  using namespace xgrammar;
  {
    std::string value = "hello\nworld";
    auto json_value = AutoSerializeJSONValue(value);
    ASSERT_EQ(json_value.serialize(), "\"hello\\nworld\"");

    std::string deserialized;
    auto error = AutoDeserializeJSONValue(&deserialized, json_value);
    ASSERT_FALSE(error.has_value());
    ASSERT_EQ(deserialized, value);
  }

  {
    std::string value = "\xC3\x28";
    auto json_value = AutoSerializeJSON(value);
    ASSERT_EQ(json_value, u8"\"\u00c3(\"");

    std::string deserialized;
    auto error = AutoDeserializeJSON(&deserialized, json_value);
    ASSERT_FALSE(error.has_value());
    ASSERT_EQ(deserialized, value);
  }
  {
    std::string value = "我";
    auto json_value = AutoSerializeJSON(value);
    std::cout << json_value << std::endl;
    ASSERT_EQ(json_value, u8"\"\u00e6\u0088\u0091\"");

    std::string deserialized;
    auto error = AutoDeserializeJSON(&deserialized, json_value);
    ASSERT_FALSE(error.has_value());
    ASSERT_EQ(deserialized, value);
  }
}

TEST(XGrammarSerializationTest, TestFSMEdge) {
  using namespace xgrammar;

  // Test basic FSMEdge
  {
    FSMEdge edge{1, 2, 3};
    auto json_value = AutoSerializeJSONValue(edge);
    ASSERT_TRUE(json_value.is<picojson::array>());

    // Test literal string comparison
    std::string expected = "[1,2,3]";
    ASSERT_EQ(json_value.serialize(), expected);

    FSMEdge deserialized;
    auto error = AutoDeserializeJSONValue(&deserialized, json_value);
    ASSERT_FALSE(error.has_value());
    ASSERT_EQ(deserialized, edge);

    // Test roundtrip
    auto json_value2 = AutoSerializeJSONValue(deserialized);
    ASSERT_EQ(json_value.serialize(), json_value2.serialize());
  }

  // Test special edge types
  {
    FSMEdge epsilon_edge{FSMEdge::EdgeType::kEpsilon, 0, 5};
    auto json_value = AutoSerializeJSONValue(epsilon_edge);

    // Test literal string comparison
    std::string expected = "[-1,0,5]";
    ASSERT_EQ(json_value.serialize(), expected);

    FSMEdge deserialized;
    auto error = AutoDeserializeJSONValue(&deserialized, json_value);
    ASSERT_FALSE(error.has_value());
    ASSERT_EQ(deserialized, epsilon_edge);
    ASSERT_TRUE(deserialized.IsEpsilon());

    // Test roundtrip
    auto json_value2 = AutoSerializeJSONValue(deserialized);
    ASSERT_EQ(json_value.serialize(), json_value2.serialize());
  }

  {
    FSMEdge rule_edge{FSMEdge::EdgeType::kRuleRef, 10, 7};
    auto json_value = AutoSerializeJSONValue(rule_edge);

    // Test literal string comparison
    std::string expected = "[-2,10,7]";
    ASSERT_EQ(json_value.serialize(), expected);

    FSMEdge deserialized;
    auto error = AutoDeserializeJSONValue(&deserialized, json_value);
    ASSERT_FALSE(error.has_value());
    ASSERT_EQ(deserialized, rule_edge);
    ASSERT_TRUE(deserialized.IsRuleRef());
    ASSERT_EQ(deserialized.GetRefRuleId(), 10);

    // Test roundtrip
    auto json_value2 = AutoSerializeJSONValue(deserialized);
    ASSERT_EQ(json_value.serialize(), json_value2.serialize());
  }
}

TEST(XGrammarSerializationTest, TestCompact2DArray) {
  using namespace xgrammar;

  // Test empty array
  {
    Compact2DArray<int> array;
    auto json_value = AutoSerializeJSONValue(array);
    ASSERT_TRUE(json_value.is<picojson::object>());

    // Test literal string comparison
    std::string expected = "{\"data_\":[],\"indptr_\":[0]}";
    ASSERT_EQ(json_value.serialize(), expected);

    Compact2DArray<int> deserialized;
    auto error = AutoDeserializeJSONValue(&deserialized, json_value);
    ASSERT_FALSE(error.has_value());
    ASSERT_EQ(deserialized.size(), 0);

    // Test roundtrip
    auto json_value2 = AutoSerializeJSONValue(deserialized);
    ASSERT_EQ(json_value.serialize(), json_value2.serialize());
  }

  // Test non-empty array
  {
    Compact2DArray<int> array;
    array.PushBack({0, 1, 2, 3});
    array.PushBack({4, 5, 6, 7});
    array.PushBack({8, 9});

    auto json_value = AutoSerializeJSONValue(array);
    ASSERT_TRUE(json_value.is<picojson::object>());

    // Test literal string comparison
    std::string expected = "{\"data_\":[0,1,2,3,4,5,6,7,8,9],\"indptr_\":[0,4,8,10]}";
    ASSERT_EQ(json_value.serialize(), expected);

    // Check JSON structure
    const auto& obj = json_value.get<picojson::object>();
    ASSERT_TRUE(obj.find("data_") != obj.end());
    ASSERT_TRUE(obj.find("indptr_") != obj.end());

    const auto& data_array = obj.at("data_").get<picojson::array>();
    ASSERT_EQ(data_array.size(), 10);

    const auto& indptr_array = obj.at("indptr_").get<picojson::array>();
    ASSERT_EQ(indptr_array.size(), 4);  // 3 rows + 1

    Compact2DArray<int> deserialized;
    auto error = AutoDeserializeJSONValue(&deserialized, json_value);
    ASSERT_FALSE(error.has_value());
    ASSERT_EQ(deserialized, array);

    // Test roundtrip
    auto json_value2 = AutoSerializeJSONValue(deserialized);
    ASSERT_EQ(json_value.serialize(), json_value2.serialize());
  }

  // Test with FSMEdge
  {
    Compact2DArray<FSMEdge> array;
    array.PushBack({{1, 2, 3}, {4, 5, 6}});
    array.PushBack({{FSMEdge::EdgeType::kEpsilon, 0, 7}});

    auto json_value = AutoSerializeJSONValue(array);
    ASSERT_TRUE(json_value.is<picojson::object>());

    // Test literal string comparison
    std::string expected = "{\"data_\":[[1,2,3],[4,5,6],[-1,0,7]],\"indptr_\":[0,2,3]}";
    ASSERT_EQ(json_value.serialize(), expected);

    Compact2DArray<FSMEdge> deserialized;
    auto error = AutoDeserializeJSONValue(&deserialized, json_value);
    ASSERT_FALSE(error.has_value());
    ASSERT_TRUE(deserialized == array);

    // Test roundtrip
    auto json_value2 = AutoSerializeJSONValue(deserialized);
    ASSERT_EQ(json_value.serialize(), json_value2.serialize());
  }
}

TEST(XGrammarSerializationTest, TestDynamicBitset) {
  using namespace xgrammar;

  // Test empty bitset
  {
    DynamicBitset bitset(0);
    auto json_value = AutoSerializeJSONValue(bitset);
    ASSERT_TRUE(json_value.is<picojson::array>());

    // Test literal string comparison
    std::string expected = "[0,0]";
    ASSERT_EQ(json_value.serialize(), expected);

    DynamicBitset deserialized;
    auto error = AutoDeserializeJSONValue(&deserialized, json_value);
    ASSERT_FALSE(error.has_value());
    ASSERT_EQ(deserialized, bitset);

    // Test roundtrip
    auto json_value2 = AutoSerializeJSONValue(deserialized);
    ASSERT_EQ(json_value.serialize(), json_value2.serialize());
  }

  // Test non-empty bitset
  {
    DynamicBitset bitset(64);
    bitset.Set(0);
    bitset.Set(10);
    bitset.Set(63);

    auto json_value = AutoSerializeJSONValue(bitset);
    ASSERT_TRUE(json_value.is<picojson::array>());

    // Test literal string comparison
    std::string expected = "[64,2,1025,2147483648]";
    ASSERT_EQ(json_value.serialize(), expected);

    const auto& arr = json_value.get<picojson::array>();
    ASSERT_EQ(arr.size(), 4);              // size, buffer_size, data[0], data[1]
    ASSERT_EQ(arr[0].get<int64_t>(), 64);  // size
    ASSERT_EQ(arr[1].get<int64_t>(), 2);   // buffer_size

    DynamicBitset deserialized;
    auto error = AutoDeserializeJSONValue(&deserialized, json_value);
    ASSERT_FALSE(error.has_value());
    ASSERT_EQ(deserialized, bitset);

    // Verify bit values
    ASSERT_TRUE(deserialized[0]);
    ASSERT_TRUE(deserialized[10]);
    ASSERT_TRUE(deserialized[63]);
    ASSERT_FALSE(deserialized[1]);
    ASSERT_FALSE(deserialized[62]);

    // Test roundtrip
    auto json_value2 = AutoSerializeJSONValue(deserialized);
    ASSERT_EQ(json_value.serialize(), json_value2.serialize());
  }

  // Test smaller bitset
  {
    DynamicBitset bitset(10);
    bitset.Set(0);
    bitset.Set(5);
    bitset.Set(9);

    auto json_value = AutoSerializeJSONValue(bitset);
    ASSERT_TRUE(json_value.is<picojson::array>());

    // Test literal string comparison
    // Bits 0, 5, 9 are set: 2^0 + 2^5 + 2^9 = 1 + 32 + 512 = 545
    std::string expected = "[10,1,545]";
    ASSERT_EQ(json_value.serialize(), expected);

    DynamicBitset deserialized;
    auto error = AutoDeserializeJSONValue(&deserialized, json_value);
    ASSERT_FALSE(error.has_value());
    ASSERT_EQ(deserialized, bitset);

    // Test roundtrip
    auto json_value2 = AutoSerializeJSONValue(deserialized);
    ASSERT_EQ(json_value.serialize(), json_value2.serialize());
  }

  // Count should ignore padding bits in the last block.
  {
    DynamicBitset bitset(10);
    bitset.Set();

    ASSERT_EQ(bitset.Count(), 10);
    ASSERT_TRUE(bitset.All());
  }

  // Count should also ignore padding bits when the bitset spans multiple blocks.
  {
    DynamicBitset bitset(33);
    bitset.Set();

    ASSERT_EQ(bitset.Count(), 33);
    ASSERT_TRUE(bitset.All());
  }

  // Finders should ignore padding bits on non-word-aligned bitsets.
  {
    DynamicBitset bitset(10);
    for (int i = 0; i < 10; ++i) {
      bitset.Set(i);
    }

    ASSERT_EQ(bitset.FindFirstOne(), 0);
    ASSERT_EQ(bitset.FindFirstZero(), -1);
  }

  // OR-ing from a larger same-buffer-size bitset should not contaminate padding bits.
  {
    DynamicBitset bitset(10);
    DynamicBitset other(20);
    other.Set(15);

    bitset |= other;

    ASSERT_EQ(bitset.Count(), 0);
    ASSERT_EQ(bitset.FindFirstOne(), -1);
    ASSERT_EQ(AutoSerializeJSONValue(bitset).serialize(), "[10,1,0]");
    ASSERT_EQ(bitset, DynamicBitset(10));
  }

  // Serialization and equality should ignore padding bits.
  {
    DynamicBitset clean(10);
    DynamicBitset padded(10);
    padded.Set();
    for (int i = 0; i < 10; ++i) {
      padded.Reset(i);
    }

    ASSERT_EQ(clean.Count(), 0);
    ASSERT_EQ(padded.Count(), 0);
    ASSERT_EQ(clean, padded);
    ASSERT_EQ(AutoSerializeJSONValue(padded).serialize(), "[10,1,0]");
  }

  // Copying a bitset backed by external storage should deep-copy its contents.
  {
    uint32_t storage[1] = {0};
    DynamicBitset external(10, storage);
    external.Set(3);

    DynamicBitset copied(external);
    external.Reset(3);

    ASSERT_TRUE(copied[3]);
    ASSERT_FALSE(external[3]);
  }
}

TEST(XGrammarSerializationTest, TestCompactFSM) {
  using namespace xgrammar;

  // Test simple FSM
  {
    FSM fsm(3);
    fsm.AddEdge(0, 1, 'a', 'a');
    fsm.AddEdge(1, 2, 'b', 'b');
    fsm.AddEpsilonEdge(0, 2);

    CompactFSM compact_fsm = fsm.ToCompact();

    auto json_value = AutoSerializeJSONValue(compact_fsm);
    ASSERT_TRUE(json_value.is<picojson::object>());

    // Test literal string comparison - edges are sorted by CompactFSM
    std::string expected =
        "{\"edges\":{\"data_\":[[-1,0,2],[97,97,1],[98,98,2]],\"indptr_\":[0,2,3,3]},\"edge_aux_"
        "data\":[]}";
    ASSERT_EQ(json_value.serialize(), expected);

    CompactFSM deserialized;
    auto error = AutoDeserializeJSONValue(&deserialized, json_value);
    ASSERT_FALSE(error.has_value());

    // Test basic properties
    ASSERT_EQ(deserialized.NumStates(), compact_fsm.NumStates());

    // Test roundtrip
    auto json_value2 = AutoSerializeJSONValue(deserialized);
    ASSERT_EQ(json_value.serialize(), json_value2.serialize());
  }

  // Test FSM with rule references
  {
    FSM fsm(3);
    fsm.AddEdge(0, 1, 'a', 'z');
    fsm.AddRuleEdge(1, 2, 5);
    fsm.AddEOSEdge(2, 0);

    CompactFSM compact_fsm = fsm.ToCompact();

    auto json_value = AutoSerializeJSONValue(compact_fsm);

    // Test literal string comparison
    std::string expected =
        "{\"edges\":{\"data_\":[[97,122,1],[-2,5,2],[-3,0,0]],\"indptr_\":[0,1,2,3]},\"edge_aux_"
        "data\":[]}";
    ASSERT_EQ(json_value.serialize(), expected);

    CompactFSM deserialized;
    auto error = AutoDeserializeJSONValue(&deserialized, json_value);
    ASSERT_FALSE(error.has_value());

    ASSERT_EQ(deserialized.NumStates(), compact_fsm.NumStates());

    // Test roundtrip
    auto json_value2 = AutoSerializeJSONValue(deserialized);
    ASSERT_EQ(json_value.serialize(), json_value2.serialize());
  }

  // Repeat edge rule ids should preserve full int32_t range.
  {
    FSM fsm(2);
    fsm.AddRepeatEdge(0, 1, 40000, 2, 5);

    CompactFSM compact_fsm = fsm.ToCompact();
    ASSERT_EQ(compact_fsm.GetEdges(0).size(), 1U);
    const auto& edge = compact_fsm.GetEdges(0)[0];
    ASSERT_TRUE(edge.IsRepeatRef());

    auto repeat_info = compact_fsm.GetRepeatEdgeInfo(edge.GetAuxIndex());
    ASSERT_EQ(repeat_info.RuleId(), 40000);
    ASSERT_EQ(repeat_info.Lower(), 2);
    ASSERT_EQ(repeat_info.Upper(), 5);
  }
}

TEST(XGrammarSerializationTest, TestCompactFSMWithStartEnd) {
  using namespace xgrammar;

  // Test simple FSM
  {
    FSM fsm(3);
    fsm.AddEdge(0, 1, 'a', 'a');
    fsm.AddEdge(1, 2, 'b', 'b');
    fsm.AddEpsilonEdge(0, 2);

    CompactFSM compact_fsm = fsm.ToCompact();
    CompactFSMWithStartEnd compact_fsm_with_start_end(compact_fsm, 0, {false, false, true});

    auto json_value = AutoSerializeJSONValue(compact_fsm_with_start_end);
    ASSERT_TRUE(json_value.is<picojson::array>());

    // Test literal string comparison - edges are sorted by CompactFSM
    std::string expected =
        "[{\"edges\":{\"data_\":[[-1,0,2],[97,97,1],[98,98,2]],\"indptr_\":[0,2,3,3]},\"edge_aux_"
        "data\":[]},0,[2],false,3]";
    ASSERT_EQ(json_value.serialize(), expected);

    CompactFSMWithStartEnd deserialized;
    auto error = AutoDeserializeJSONValue(&deserialized, json_value);
    ASSERT_FALSE(error.has_value());

    // Test basic properties
    ASSERT_EQ(deserialized.GetFsm().NumStates(), compact_fsm.NumStates());
    ASSERT_EQ(deserialized.GetStart(), 0);
    ASSERT_EQ(deserialized.GetEnds(), std::vector<bool>({false, false, true}));
    ASSERT_EQ(deserialized.GetNumEdges(), 3);

    // Test roundtrip
    auto json_value2 = AutoSerializeJSONValue(deserialized);
    ASSERT_EQ(json_value.serialize(), json_value2.serialize());
  }

  // Test FSM with rule references
  {
    FSM fsm(3);
    fsm.AddEdge(0, 1, 'a', 'z');
    fsm.AddRuleEdge(1, 2, 5);
    fsm.AddEOSEdge(2, 0);

    CompactFSM compact_fsm = fsm.ToCompact();
    CompactFSMWithStartEnd compact_fsm_with_start_end(compact_fsm, 0, {false, false, true});

    auto json_value = AutoSerializeJSONValue(compact_fsm_with_start_end);

    // Test literal string comparison
    std::string expected =
        "[{\"edges\":{\"data_\":[[97,122,1],[-2,5,2],[-3,0,0]],\"indptr_\":[0,1,2,3]},\"edge_aux_"
        "data\":[]},0,[2],false,3]";
    ASSERT_EQ(json_value.serialize(), expected);

    CompactFSMWithStartEnd deserialized;
    auto error = AutoDeserializeJSONValue(&deserialized, json_value);
    ASSERT_FALSE(error.has_value());

    ASSERT_EQ(deserialized.GetFsm().NumStates(), compact_fsm.NumStates());
    ASSERT_EQ(deserialized.GetStart(), 0);
    ASSERT_EQ(deserialized.GetEnds(), std::vector<bool>({false, false, true}));
    ASSERT_EQ(deserialized.GetNumEdges(), 3);

    // Test roundtrip
    auto json_value2 = AutoSerializeJSONValue(deserialized);
    ASSERT_EQ(json_value.serialize(), json_value2.serialize());
  }
}

TEST(XGrammarSerializationTest, TestComplexStructures) {
  using namespace xgrammar;

  // Test vector of FSMEdges
  {
    std::vector<FSMEdge> edges = {
        {1, 2, 3}, {FSMEdge::EdgeType::kEpsilon, 0, 4}, {FSMEdge::EdgeType::kRuleRef, 5, 6}
    };

    auto json_value = AutoSerializeJSONValue(edges);
    ASSERT_TRUE(json_value.is<picojson::array>());

    // Test literal string comparison
    std::string expected = "[[1,2,3],[-1,0,4],[-2,5,6]]";
    ASSERT_EQ(json_value.serialize(), expected);

    std::vector<FSMEdge> deserialized;
    auto error = AutoDeserializeJSONValue(&deserialized, json_value);
    ASSERT_FALSE(error.has_value());
    ASSERT_EQ(deserialized, edges);

    // Test roundtrip
    auto json_value2 = AutoSerializeJSONValue(deserialized);
    ASSERT_EQ(json_value.serialize(), json_value2.serialize());
  }

  // Test unordered_map with complex types
  {
    std::unordered_map<std::string, std::vector<int>> map = {
        {"key1", {1, 2, 3}}, {"key2", {4, 5, 6}}
    };

    auto json_value = AutoSerializeJSONValue(map);
    ASSERT_TRUE(json_value.is<picojson::object>());

    // Test literal string comparison (note: order might vary in map)
    // We'll just verify the structure instead of exact string
    const auto& obj = json_value.get<picojson::object>();
    ASSERT_EQ(obj.size(), 2);
    ASSERT_TRUE(obj.find("key1") != obj.end());
    ASSERT_TRUE(obj.find("key2") != obj.end());

    std::unordered_map<std::string, std::vector<int>> deserialized;
    auto error = AutoDeserializeJSONValue(&deserialized, json_value);
    ASSERT_FALSE(error.has_value());
    ASSERT_EQ(deserialized, map);

    // Test roundtrip
    auto json_value2 = AutoSerializeJSONValue(deserialized);
    ASSERT_EQ(json_value.serialize(), json_value2.serialize());
  }
}

TEST(XGrammarSerializationTest, TestTokenizerFirstByteBoundaries) {
  using namespace xgrammar;

  const std::vector<std::string> vocab = {
      "b",
      "a",
      "aa",
      std::string("\x80", 1),
      std::string("\x80z", 2),
      std::string("\xff", 1),
      "z"};
  TokenizerInfo tokenizer_info(vocab, VocabType::RAW);

  const auto& sorted_decoded_vocab = tokenizer_info.GetSortedDecodedVocab();
  const auto& boundaries = tokenizer_info.ImplPtr()->GetFirstByteToSortedVocabBoundary();

  auto compare_token = [](const std::pair<int32_t, std::string>& lhs, const std::string& rhs) {
    return lhs.second < rhs;
  };

  for (int byte = 0; byte < 256; ++byte) {
    std::string probe(1, static_cast<char>(static_cast<uint8_t>(byte)));
    int32_t expected = static_cast<int32_t>(
        std::lower_bound(sorted_decoded_vocab.begin(), sorted_decoded_vocab.end(), probe, compare_token) -
        sorted_decoded_vocab.begin()
    );
    EXPECT_EQ(boundaries[byte], expected) << "byte=" << byte;
  }
  EXPECT_EQ(boundaries[256], static_cast<int32_t>(sorted_decoded_vocab.size()));
}

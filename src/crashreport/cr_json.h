// src/crashreport/cr_json.h — minimal ordered JSON writer and strict reader.
//
// Writer: keys come out in call order, with no whitespace, so the same inputs
// always produce the same bytes. Strings are fully escaped and the output is
// always valid UTF-8: invalid input bytes become U+FFFD.
//
// Reader: JsonDoc reads one whole document into a flat node array. It is
// strict on purpose — the console reads API answers with it, and the contract
// (D2Vita-website contract/README.md, response signatures) says a body that
// does not match its schema is handled like a network failure: no trailing
// content, no duplicate member, no unescaped control character, no invalid
// UTF-8, JSON numbers only, and bounded depth, node count and size.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace d2cr {

// JSON string literal for `s`, including the surrounding quotes.
std::string json_quote(const std::string& s);

class JsonWriter {
 public:
  JsonWriter& begin_object();
  JsonWriter& end_object();
  JsonWriter& begin_array();
  JsonWriter& end_array();
  JsonWriter& key(const std::string& k);
  JsonWriter& str(const std::string& v);
  JsonWriter& num(int64_t v);
  JsonWriter& unum(uint64_t v);
  JsonWriter& boolean(bool v);
  JsonWriter& null();
  // Inserts already-serialized JSON as one value (e.g. Evidence::features_json).
  JsonWriter& raw(const std::string& json);
  const std::string& out() const { return out_; }

 private:
  void before_value();
  std::string out_;
  std::vector<bool> has_items_;   // one entry per open object/array
  bool after_key_ = false;
};

enum class JsonType : uint8_t { Null, Bool, Number, String, Array, Object };

struct JsonLimits {
  size_t max_depth = 16;            // nesting of arrays and objects
  size_t max_nodes = 512;           // values in the document
  size_t max_bytes = 64 * 1024;     // length of the document
  // Wide limits for the contract vector files read by the tests.
  static JsonLimits vectors();
};

struct JsonNode {
  JsonType type = JsonType::Null;
  bool bval = false;                // Bool
  std::string key;                  // member name when the parent is an object
  std::string text;                 // String: the decoded value
  size_t codepoints = 0;            // String: code points of `text` (JSON Schema maxLength)
  size_t count = 0;                 // Object, Array: members or elements
  int first = -1;                   // Object, Array: first child, -1 when empty
  int next = -1;                    // next sibling, -1 at the end

  // Number whose exact value is an integer in [0, 2^64). JSON Schema reads
  // 1420, 1420.0 and 1.42e3 as the same integer, so the reader does too.
  bool is_u64() const { return integer; }
  uint64_t u64() const { return uvalue; }
  bool integer = false;
  uint64_t uvalue = 0;
};

class JsonDoc {
 public:
  // Parses one whole document. False leaves error() set and the document empty.
  bool parse(const std::string& text, const JsonLimits& limits = JsonLimits());
  const JsonNode* root() const { return nodes_.empty() ? nullptr : &nodes_[0]; }
  // Member `key` of an object node, or null.
  const JsonNode* member(const JsonNode& object, const char* key) const;
  const JsonNode* first(const JsonNode& parent) const { return at(parent.first); }
  const JsonNode* next(const JsonNode& node) const { return at(node.next); }
  const std::string& error() const { return error_; }

 private:
  const JsonNode* at(int index) const {
    return index >= 0 && (size_t)index < nodes_.size() ? &nodes_[(size_t)index] : nullptr;
  }
  int parse_value(size_t depth);
  bool parse_string(std::string* out, size_t* codepoints);
  bool parse_number(JsonNode* node);
  bool skip_space();
  bool fail(const char* why);
  int add_node();

  std::vector<JsonNode> nodes_;
  const char* p_ = nullptr;         // parsing cursor, valid during parse()
  const char* end_ = nullptr;
  JsonLimits limits_;
  std::string error_;
};

}  // namespace d2cr

#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

// Just enough JSON for LSP: the protocol never nests deeply and never needs
// numbers that do not fit a double.
struct Json {
  enum Kind { NUL, BOOL, NUMBER, STRING, ARRAY, OBJECT } kind = NUL;

  bool boolean = false;
  double number = 0;
  std::string text;
  std::vector<Json> items;
  std::vector<std::pair<std::string, Json>> fields;

  static Json null() { return Json(); }
  static Json of(bool v);
  static Json of(double v);
  static Json of(long long v);
  static Json of(const std::string &v);
  static Json of(const char *v);
  static Json array();
  static Json object();

  void push(Json value);
  void set(const std::string &key, Json value);

  const Json *find(const std::string &key) const;
  const Json &at(const std::string &key) const; // null when missing

  std::string as_string() const { return kind == STRING ? text : std::string(); }
  long long as_int() const { return kind == NUMBER ? (long long)number : 0; }
  bool as_bool() const { return kind == BOOL && boolean; }

  std::string dump() const;
};

// Returns false when the text is not valid JSON.
bool json_parse(const std::string &text, Json &out);

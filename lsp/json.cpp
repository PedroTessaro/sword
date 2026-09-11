#include "json.h"

#include <cstdio>
#include <cstdlib>

Json Json::of(bool v) {
  Json j;
  j.kind = BOOL;
  j.boolean = v;
  return j;
}

Json Json::of(double v) {
  Json j;
  j.kind = NUMBER;
  j.number = v;
  return j;
}

Json Json::of(long long v) { return of((double)v); }

Json Json::of(const std::string &v) {
  Json j;
  j.kind = STRING;
  j.text = v;
  return j;
}

Json Json::of(const char *v) { return of(std::string(v)); }

Json Json::array() {
  Json j;
  j.kind = ARRAY;
  return j;
}

Json Json::object() {
  Json j;
  j.kind = OBJECT;
  return j;
}

void Json::push(Json value) {
  kind = ARRAY;
  items.push_back(std::move(value));
}

void Json::set(const std::string &key, Json value) {
  kind = OBJECT;
  for (auto &field : fields)
    if (field.first == key) {
      field.second = std::move(value);
      return;
    }
  fields.emplace_back(key, std::move(value));
}

const Json *Json::find(const std::string &key) const {
  if (kind != OBJECT) return nullptr;
  for (const auto &field : fields)
    if (field.first == key) return &field.second;
  return nullptr;
}

const Json &Json::at(const std::string &key) const {
  static const Json nothing;
  const Json *found = find(key);
  return found ? *found : nothing;
}

namespace {

void escape(const std::string &text, std::string &out) {
  out += '"';
  for (unsigned char c : text) {
    switch (c) {
    case '"': out += "\\\""; break;
    case '\\': out += "\\\\"; break;
    case '\n': out += "\\n"; break;
    case '\r': out += "\\r"; break;
    case '\t': out += "\\t"; break;
    default:
      if (c < 0x20) {
        char buf[8];
        snprintf(buf, sizeof(buf), "\\u%04x", c);
        out += buf;
      } else {
        out += (char)c;
      }
    }
  }
  out += '"';
}

void write(const Json &j, std::string &out) {
  switch (j.kind) {
  case Json::NUL:
    out += "null";
    break;
  case Json::BOOL:
    out += j.boolean ? "true" : "false";
    break;
  case Json::NUMBER: {
    char buf[32];
    if (j.number == (long long)j.number)
      snprintf(buf, sizeof(buf), "%lld", (long long)j.number);
    else
      snprintf(buf, sizeof(buf), "%.17g", j.number);
    out += buf;
    break;
  }
  case Json::STRING:
    escape(j.text, out);
    break;
  case Json::ARRAY:
    out += '[';
    for (size_t i = 0; i < j.items.size(); i++) {
      if (i) out += ',';
      write(j.items[i], out);
    }
    out += ']';
    break;
  case Json::OBJECT:
    out += '{';
    for (size_t i = 0; i < j.fields.size(); i++) {
      if (i) out += ',';
      escape(j.fields[i].first, out);
      out += ':';
      write(j.fields[i].second, out);
    }
    out += '}';
    break;
  }
}

struct Reader {
  const std::string &text;
  size_t i = 0;

  explicit Reader(const std::string &t) : text(t) {}

  void spaces() {
    while (i < text.size() && (unsigned char)text[i] <= ' ') i++;
  }

  bool literal(const char *word) {
    size_t n = 0;
    while (word[n]) n++;
    if (text.compare(i, n, word) != 0) return false;
    i += n;
    return true;
  }

  bool string_value(std::string &out) {
    if (i >= text.size() || text[i] != '"') return false;
    i++;
    while (i < text.size() && text[i] != '"') {
      char c = text[i++];
      if (c != '\\') {
        out += c;
        continue;
      }
      if (i >= text.size()) return false;
      char esc = text[i++];
      switch (esc) {
      case 'n': out += '\n'; break;
      case 't': out += '\t'; break;
      case 'r': out += '\r'; break;
      case 'b': out += '\b'; break;
      case 'f': out += '\f'; break;
      case 'u': {
        if (i + 4 > text.size()) return false;
        int code = (int)strtol(text.substr(i, 4).c_str(), nullptr, 16);
        i += 4;
        // Encode as UTF-8; surrogate pairs are left alone, which is fine for
        // the identifiers and paths that actually show up here.
        if (code < 0x80) {
          out += (char)code;
        } else if (code < 0x800) {
          out += (char)(0xC0 | (code >> 6));
          out += (char)(0x80 | (code & 0x3F));
        } else {
          out += (char)(0xE0 | (code >> 12));
          out += (char)(0x80 | ((code >> 6) & 0x3F));
          out += (char)(0x80 | (code & 0x3F));
        }
        break;
      }
      default: out += esc; break;
      }
    }
    if (i >= text.size()) return false;
    i++; // closing quote
    return true;
  }

  bool value(Json &out) {
    spaces();
    if (i >= text.size()) return false;
    char c = text[i];

    if (c == 'n') { out = Json::null(); return literal("null"); }
    if (c == 't') { out = Json::of(true); return literal("true"); }
    if (c == 'f') { out = Json::of(false); return literal("false"); }

    if (c == '"') {
      std::string s;
      if (!string_value(s)) return false;
      out = Json::of(s);
      return true;
    }

    if (c == '[') {
      i++;
      out = Json::array();
      spaces();
      if (i < text.size() && text[i] == ']') { i++; return true; }
      while (true) {
        Json item;
        if (!value(item)) return false;
        out.items.push_back(std::move(item));
        spaces();
        if (i >= text.size()) return false;
        if (text[i] == ',') { i++; continue; }
        if (text[i] == ']') { i++; return true; }
        return false;
      }
    }

    if (c == '{') {
      i++;
      out = Json::object();
      spaces();
      if (i < text.size() && text[i] == '}') { i++; return true; }
      while (true) {
        spaces();
        std::string key;
        if (!string_value(key)) return false;
        spaces();
        if (i >= text.size() || text[i] != ':') return false;
        i++;
        Json item;
        if (!value(item)) return false;
        out.fields.emplace_back(std::move(key), std::move(item));
        spaces();
        if (i >= text.size()) return false;
        if (text[i] == ',') { i++; continue; }
        if (text[i] == '}') { i++; return true; }
        return false;
      }
    }

    char *end = nullptr;
    double n = strtod(text.c_str() + i, &end);
    if (end == text.c_str() + i) return false;
    i = (size_t)(end - text.c_str());
    out = Json::of(n);
    return true;
  }
};

} // namespace

std::string Json::dump() const {
  std::string out;
  write(*this, out);
  return out;
}

bool json_parse(const std::string &text, Json &out) {
  Reader reader(text);
  if (!reader.value(out)) return false;
  return true;
}

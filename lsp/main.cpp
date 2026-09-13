#include "../src/check.h"
#include "../src/diag.h"
#include "../src/package.h"
#include "../src/paths.h"
#include "../src/types.h"
#include "complete.h"
#include "json.h"
#include "semantic.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {

// LSP speaks in URIs; the compiler speaks in paths.
std::string path_of(const std::string &uri) {
  std::string text = uri;
  if (text.compare(0, 7, "file://") == 0) text = text.substr(7);
  std::string out;
  for (size_t i = 0; i < text.size(); i++) {
    if (text[i] == '%' && i + 2 < text.size()) {
      out += (char)strtol(text.substr(i + 1, 2).c_str(), nullptr, 16);
      i += 2;
    } else {
      out += text[i];
    }
  }
  return out;
}

// One compilation's worth of state. Only one may be alive at a time, because
// the source registry behind it is global.
struct Analysis {
  Program prog;
  TypeTable types;
  std::vector<Diagnostic> diagnostics;
  int file = -1;
};

struct Server {
  std::unique_ptr<Analysis> cached;
  std::string cached_uri;
  long long cached_version = -1;
  bool running = true;
  // What the editor last sent for each open file. Completion needs it, because
  // it analyses a patched copy and has to put the real one back.
  std::map<std::string, std::string> open_text;
  long long scratch_version = -2; // never collides with a real one

  void send(const Json &message) {
    std::string body = message.dump();
    printf("Content-Length: %zu\r\n\r\n%s", body.size(), body.c_str());
    fflush(stdout);
  }

  void reply(const Json &id, Json result) {
    Json message = Json::object();
    message.set("jsonrpc", Json::of("2.0"));
    message.set("id", id);
    message.set("result", std::move(result));
    send(message);
  }

  Analysis *analyze(const std::string &uri, long long version) {
    if (cached && cached_uri == uri && cached_version == version)
      return cached.get();

    // Dropping the old analysis first: its nodes point into types and sources
    // that are about to be replaced.
    cached.reset();
    cached_uri = uri;
    cached_version = version;

    reset_sources();
    reset_errors();

    auto fresh = std::unique_ptr<Analysis>(new Analysis());
    capture_diagnostics(&fresh->diagnostics);

    std::string path = path_of(uri);
    // The *directory*, not the file. A file belongs to its package, and a name
    // declared in a sibling file is not undefined just because the editor has
    // this one open — reporting it as undefined was the single most misleading
    // thing this server did.
    std::string dir = directory_of(path);
    std::vector<std::string> search{dir};
    for (const std::string &root : package_roots()) search.push_back(root);
    load_program(dir, search, fresh->prog, LOAD_EDITOR);
    // A file that failed to parse still has whatever came before the error,
    // which is enough to colour it.
    if (!fresh->prog.order.empty()) check(fresh->prog, fresh->types);

    capture_diagnostics(nullptr);
    for (int i = 0; i < source_count(); i++)
      if (source_at(i).path == path) fresh->file = i;

    cached = std::move(fresh);
    return cached.get();
  }

  void publish(const std::string &uri, long long version) {
    Analysis *analysis = analyze(uri, version);
    std::string path = path_of(uri);

    Json list = Json::array();
    for (const Diagnostic &d : analysis->diagnostics) {
      // Notes belong to the error above them, and a diagnostic without a
      // place has nowhere to be drawn.
      if (!d.is_error || d.pos.file < 0) continue;
      if (d.pos.file >= source_count()) continue;
      if (source_at(d.pos.file).path != path) continue;

      Json start = Json::object();
      start.set("line", Json::of((long long)d.pos.line - 1));
      start.set("character", Json::of((long long)d.pos.col - 1));
      Json end = Json::object();
      end.set("line", Json::of((long long)d.pos.line - 1));
      end.set("character", Json::of((long long)d.pos.col));

      Json range = Json::object();
      range.set("start", start);
      range.set("end", end);

      Json entry = Json::object();
      entry.set("range", range);
      entry.set("severity", Json::of((long long)1));
      entry.set("source", Json::of("sword"));
      entry.set("message", Json::of(d.message));
      list.push(entry);
    }

    Json params = Json::object();
    params.set("uri", Json::of(uri));
    params.set("diagnostics", list);

    Json message = Json::object();
    message.set("jsonrpc", Json::of("2.0"));
    message.set("method", Json::of("textDocument/publishDiagnostics"));
    message.set("params", params);
    send(message);
  }

  Json legend() {
    Json types = Json::array();
    for (const std::string &name : kTokenTypes) types.push(Json::of(name));
    Json modifiers = Json::array();
    for (const std::string &name : kTokenModifiers)
      modifiers.push(Json::of(name));

    Json out = Json::object();
    out.set("tokenTypes", types);
    out.set("tokenModifiers", modifiers);
    return out;
  }

  void on_initialize(const Json &id) {
    Json semantic = Json::object();
    semantic.set("legend", legend());
    semantic.set("full", Json::of(true));

    // Completion has to be advertised or an editor will not wire it up: this is
    // what sets `omnifunc` in vim and Neovim.
    Json triggers = Json::array();
    triggers.push(Json::of("."));
    Json completion = Json::object();
    completion.set("triggerCharacters", triggers);
    completion.set("resolveProvider", Json::of(false));

    Json caps = Json::object();
    caps.set("textDocumentSync", Json::of((long long)1)); // full text each time
    caps.set("semanticTokensProvider", semantic);
    caps.set("completionProvider", completion);

    Json info = Json::object();
    info.set("name", Json::of("swordls"));
    info.set("version", Json::of("0.1"));

    Json result = Json::object();
    result.set("capabilities", caps);
    result.set("serverInfo", info);
    reply(id, result);
  }

  void on_semantic_tokens(const Json &id, const Json &params) {
    std::string uri = params.at("textDocument").at("uri").as_string();
    Analysis *analysis = analyze(uri, cached_uri == uri ? cached_version : -1);

    Json data = Json::array();
    if (analysis->file >= 0) {
      int last_line = 0;
      int last_col = 0;
      for (const SemToken &t : semantic_tokens(analysis->file, analysis->prog)) {
        int delta_line = t.line - last_line;
        int delta_col = delta_line == 0 ? t.col - last_col : t.col;
        data.push(Json::of((long long)delta_line));
        data.push(Json::of((long long)delta_col));
        data.push(Json::of((long long)t.length));
        data.push(Json::of((long long)t.type));
        data.push(Json::of((long long)t.modifiers));
        last_line = t.line;
        last_col = t.col;
      }
    }

    Json result = Json::object();
    result.set("data", data);
    reply(id, result);
  }

  // The protocol counts characters in UTF-16 units; this file is bytes.
  static size_t byte_offset(const std::string &line, long long utf16) {
    size_t i = 0;
    long long units = 0;
    while (i < line.size() && units < utf16) {
      unsigned char c = (unsigned char)line[i];
      int width = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
      units += width == 4 ? 2 : 1;
      i += width;
    }
    return i;
  }

  // The line the cursor is on, and where in it the cursor is, as bytes.
  static size_t offset_in(const std::string &text, long long line,
                          long long character, std::string *line_text) {
    size_t at = 0;
    for (long long n = 0; n < line; n++) {
      size_t eol = text.find('\n', at);
      if (eol == std::string::npos) return text.size();
      at = eol + 1;
    }
    size_t eol = text.find('\n', at);
    if (eol == std::string::npos) eol = text.size();
    *line_text = text.substr(at, eol - at);
    return at + byte_offset(*line_text, character);
  }

  void on_completion(const Json &id, const Json &params) {
    std::string uri = params.at("textDocument").at("uri").as_string();
    std::string path = path_of(uri);
    const Json &at = params.at("position");
    long long line = at.at("line").as_int();
    long long character = at.at("character").as_int();

    std::string text = open_text.count(path) ? open_text[path] : std::string();
    std::string line_text;
    size_t cursor = offset_in(text, line, character, &line_text);

    // At the moment somebody asks, the buffer almost never parses: `c.` is a
    // field access with no field yet, and a failed parse has no types in it to
    // answer from. So a placeholder goes in where the cursor is, the patched
    // copy is what gets analysed, and the real text goes back afterwards.
    bool patched = cursor > 0 && cursor <= text.size() && text[cursor - 1] == '.';
    if (patched) {
      std::string copy = text;
      copy.insert(cursor, "zz_hole");
      set_overlay(path, copy);
    }

    Analysis *analysis = analyze(uri, scratch_version--);

    Json items = Json::array();
    if (analysis->file >= 0) {
      std::string before = line_text.substr(0, byte_offset(line_text, character));
      for (const Completion &c :
           completions(before, analysis->file, (int)line + 1, analysis->prog,
                       analysis->types)) {
        Json entry = Json::object();
        entry.set("label", Json::of(c.label));
        entry.set("kind", Json::of((long long)c.kind));
        if (!c.detail.empty()) entry.set("detail", Json::of(c.detail));
        items.push(entry);
      }
    }

    if (patched) {
      set_overlay(path, text);
      // The analysis that answered this is of a file nobody wrote, so it must
      // not be the one a later request finds.
      cached.reset();
      cached_uri.clear();
      cached_version = -1;
    }

    Json result = Json::object();
    result.set("isIncomplete", Json::of(false));
    result.set("items", items);
    reply(id, result);
  }

  void handle(const Json &message) {
    std::string method = message.at("method").as_string();
    const Json &params = message.at("params");
    const Json *id = message.find("id");

    if (method == "initialize") {
      on_initialize(id ? *id : Json::null());
    } else if (method == "shutdown") {
      reply(id ? *id : Json::null(), Json::null());
    } else if (method == "exit") {
      running = false;
    } else if (method == "textDocument/didOpen") {
      const Json &doc = params.at("textDocument");
      std::string uri = doc.at("uri").as_string();
      open_text[path_of(uri)] = doc.at("text").as_string();
      set_overlay(path_of(uri), doc.at("text").as_string());
      publish(uri, doc.at("version").as_int());
    } else if (method == "textDocument/didChange") {
      const Json &doc = params.at("textDocument");
      std::string uri = doc.at("uri").as_string();
      const Json &changes = params.at("contentChanges");
      if (!changes.items.empty()) {
        std::string text = changes.items.back().at("text").as_string();
        open_text[path_of(uri)] = text;
        set_overlay(path_of(uri), text);
      }
      publish(uri, doc.at("version").as_int());
    } else if (method == "textDocument/didSave") {
      publish(params.at("textDocument").at("uri").as_string(), -1);
    } else if (method == "textDocument/didClose") {
      std::string uri = params.at("textDocument").at("uri").as_string();
      open_text.erase(path_of(uri));
      clear_overlay(path_of(uri));
      // Clearing the list is how the editor is told the squiggles are gone.
      Json empty = Json::object();
      empty.set("uri", Json::of(uri));
      empty.set("diagnostics", Json::array());
      Json note = Json::object();
      note.set("jsonrpc", Json::of("2.0"));
      note.set("method", Json::of("textDocument/publishDiagnostics"));
      note.set("params", empty);
      send(note);
    } else if (method == "textDocument/completion") {
      on_completion(id ? *id : Json::null(), params);
    } else if (method == "textDocument/semanticTokens/full") {
      on_semantic_tokens(id ? *id : Json::null(), params);
    } else if (id) {
      // Anything else that expects an answer gets an empty one rather than a
      // hang.
      reply(*id, Json::null());
    }
  }
};

bool read_message(std::string &body) {
  size_t length = 0;
  std::string line;
  while (true) {
    int c = getchar();
    if (c == EOF) return false;
    if (c == '\n') {
      if (line.empty() || line == "\r") break;
      if (line.compare(0, 15, "Content-Length:") == 0)
        length = (size_t)atoll(line.c_str() + 15);
      line.clear();
      continue;
    }
    line += (char)c;
  }

  body.assign(length, '\0');
  for (size_t i = 0; i < length; i++) {
    int c = getchar();
    if (c == EOF) return false;
    body[i] = (char)c;
  }
  return true;
}

} // namespace

int main(int argc, char **argv) {
  if (argc > 1 && !strcmp(argv[1], "--version")) {
    printf("swordls 0.1\n");
    return 0;
  }

  Server server;
  std::string body;
  while (server.running && read_message(body)) {
    Json message;
    if (!json_parse(body, message)) continue;
    server.handle(message);
  }
  return 0;
}

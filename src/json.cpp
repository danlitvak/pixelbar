// Small JSON reader: enough for transcript lines, rollout events and the config file.
#include "pixelbar.h"

#include <charconv>
#include <cstdio>
#include <cstring>

static const JVal kNull;

const JVal& JVal::operator[](std::string_view key) const {
  if (t != Obj) return kNull;
  for (auto& kv : o)
    if (kv.first == key) return kv.second;
  return kNull;
}

const JVal& JVal::at(size_t i) const { return (t == Arr && i < a.size()) ? a[i] : kNull; }

namespace {

void PutUtf8(std::string& out, uint32_t cp) {
  if (cp < 0x80) {
    out += (char)cp;
  } else if (cp < 0x800) {
    out += (char)(0xC0 | (cp >> 6));
    out += (char)(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += (char)(0xE0 | (cp >> 12));
    out += (char)(0x80 | ((cp >> 6) & 0x3F));
    out += (char)(0x80 | (cp & 0x3F));
  } else {
    out += (char)(0xF0 | (cp >> 18));
    out += (char)(0x80 | ((cp >> 12) & 0x3F));
    out += (char)(0x80 | ((cp >> 6) & 0x3F));
    out += (char)(0x80 | (cp & 0x3F));
  }
}

struct Parser {
  const char* p;
  const char* e;
  int depth = 0;

  void ws() {
    while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
  }
  bool lit(const char* s) {
    size_t n = strlen(s);
    if ((size_t)(e - p) < n || memcmp(p, s, n) != 0) return false;
    p += n;
    return true;
  }
  bool hex4(uint32_t& v) {
    if (e - p < 4) return false;
    v = 0;
    for (int i = 0; i < 4; i++) {
      char c = *p++;
      v <<= 4;
      if (c >= '0' && c <= '9')
        v |= (uint32_t)(c - '0');
      else if (c >= 'a' && c <= 'f')
        v |= (uint32_t)(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F')
        v |= (uint32_t)(c - 'A' + 10);
      else
        return false;
    }
    return true;
  }
  // Expects *p == '"'.
  bool str(std::string& out) {
    p++;
    while (p < e) {
      const char* run = p;
      while (p < e && *p != '"' && *p != '\\') p++;
      out.append(run, p);
      if (p >= e) return false;
      if (*p == '"') {
        p++;
        return true;
      }
      if (++p >= e) return false;
      char c = *p++;
      switch (c) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
          uint32_t cp;
          if (!hex4(cp)) return false;
          if (cp >= 0xD800 && cp <= 0xDBFF && e - p >= 6 && p[0] == '\\' && p[1] == 'u') {
            const char* save = p;
            p += 2;
            uint32_t lo;
            if (hex4(lo) && lo >= 0xDC00 && lo <= 0xDFFF)
              cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            else
              p = save;
          }
          if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;
          PutUtf8(out, cp);
          break;
        }
        default: return false;
      }
    }
    return false;
  }
  bool val(JVal& v) {
    ws();
    if (p >= e || ++depth > 200) return false;
    bool ok = true;
    switch (*p) {
      case '{': {
        v.t = JVal::Obj;
        p++;
        ws();
        if (p < e && *p == '}') {
          p++;
          break;
        }
        for (;;) {
          ws();
          if (p >= e || *p != '"') { ok = false; break; }
          std::string k;
          if (!str(k)) { ok = false; break; }
          ws();
          if (p >= e || *p != ':') { ok = false; break; }
          p++;
          v.o.emplace_back(std::move(k), JVal{});
          if (!val(v.o.back().second)) { ok = false; break; }
          ws();
          if (p < e && *p == ',') { p++; continue; }
          if (p < e && *p == '}') { p++; break; }
          ok = false;
          break;
        }
        break;
      }
      case '[': {
        v.t = JVal::Arr;
        p++;
        ws();
        if (p < e && *p == ']') {
          p++;
          break;
        }
        for (;;) {
          v.a.emplace_back();
          if (!val(v.a.back())) { ok = false; break; }
          ws();
          if (p < e && *p == ',') { p++; continue; }
          if (p < e && *p == ']') { p++; break; }
          ok = false;
          break;
        }
        break;
      }
      case '"': v.t = JVal::Str; ok = str(v.s); break;
      case 't': v.t = JVal::Bool; v.b = true; ok = lit("true"); break;
      case 'f': v.t = JVal::Bool; v.b = false; ok = lit("false"); break;
      case 'n': v.t = JVal::Null; ok = lit("null"); break;
      default: {
        const char* s = p;
        while (p < e && ((*p >= '0' && *p <= '9') || *p == '-' || *p == '+' || *p == '.' || *p == 'e' || *p == 'E')) p++;
        if (p == s) { ok = false; break; }
        v.t = JVal::Num;
        ok = std::from_chars(s, p, v.n).ec == std::errc();
      }
    }
    depth--;
    return ok;
  }
};

// Finds `"key"` followed by optional whitespace and a colon; returns the offset just past the colon.
size_t FindKey(std::string_view text, std::string_view key) {
  std::string needle;
  needle.reserve(key.size() + 2);
  needle += '"';
  needle += key;
  needle += '"';
  size_t pos = 0;
  while ((pos = text.find(needle, pos)) != std::string_view::npos) {
    size_t q = pos + needle.size();
    while (q < text.size() && (text[q] == ' ' || text[q] == '\t' || text[q] == '\r' || text[q] == '\n')) q++;
    if (q < text.size() && text[q] == ':') return q + 1;
    pos += needle.size();
  }
  return std::string_view::npos;
}

}  // namespace

bool JParse(std::string_view text, JVal& out) {
  out = JVal{};
  Parser ps{text.data(), text.data() + text.size()};
  if (!ps.val(out)) return false;
  ps.ws();
  return ps.p == ps.e;
}

bool JFindString(std::string_view text, std::string_view key, std::string& out) {
  size_t q = FindKey(text, key);
  if (q == std::string_view::npos) return false;
  Parser ps{text.data() + q, text.data() + text.size()};
  ps.ws();
  if (ps.p >= ps.e || *ps.p != '"') return false;
  out.clear();
  return ps.str(out);
}

bool JFindNumber(std::string_view text, std::string_view key, double& out) {
  size_t q = FindKey(text, key);
  if (q == std::string_view::npos) return false;
  Parser ps{text.data() + q, text.data() + text.size()};
  ps.ws();
  JVal v;
  if (!ps.val(v) || v.t != JVal::Num) return false;
  out = v.n;
  return true;
}

std::string JQuote(std::string_view s) {
  std::string out = "\"";
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char b[8];
          snprintf(b, sizeof b, "\\u%04x", c);
          out += b;
        } else {
          out += (char)c;
        }
    }
  }
  return out + "\"";
}

bool JFindBool(std::string_view text, std::string_view key, bool& out) {
  size_t q = FindKey(text, key);
  if (q == std::string_view::npos) return false;
  Parser ps{text.data() + q, text.data() + text.size()};
  ps.ws();
  if (ps.lit("true")) { out = true; return true; }
  if (ps.lit("false")) { out = false; return true; }
  return false;
}

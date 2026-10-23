#include "ipc/protocol.h"

namespace rime_beautify::ipc {

std::string PortFileToJson(const PortFile& pf) {
  std::string s = "{\"protocol_version\":";
  s += std::to_string(pf.protocol_version);
  s += ",\"port\":";
  s += std::to_string(pf.port);
  s += ",\"token\":\"";
  s += pf.token;
  s += "\"}";
  return s;
}

namespace {

bool SkipWs(const char*& p, const char* end) {
  while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) {
    ++p;
  }
  return p < end;
}

bool Match(const char*& p, const char* end, const char* lit) {
  while (*lit != '\0') {
    if (p >= end || *p != *lit) {
      return false;
    }
    ++p;
    ++lit;
  }
  return true;
}

bool IsDigit(char c) {
  return c >= '0' && c <= '9';
}

}  // namespace

bool PortFileFromJson(const std::string& json, PortFile* pf) {
  // 接受结构：{"protocol_version":N,"port":N,"token":"hex"}
  const char* p = json.data();
  const char* end = p + json.size();
  PortFile out;
  if (!SkipWs(p, end) || !Match(p, end, "{")) {
    return false;
  }
  for (int field = 0; field < 3; ++field) {
    if (!SkipWs(p, end) || !Match(p, end, "\"")) {
      return false;
    }
    std::string key;
    while (p < end && *p != '"') {
      key.push_back(*p++);
    }
    if (p >= end || !Match(p, end, "\"")) {
      return false;
    }
    if (!SkipWs(p, end) || !Match(p, end, ":")) {
      return false;
    }
    if (key == "protocol_version" || key == "port") {
      if (!SkipWs(p, end) || !IsDigit(*p)) {
        return false;
      }
      long long v = 0;
      while (p < end && IsDigit(*p)) {
        v = v * 10 + (*p - '0');
        ++p;
      }
      if (key == "protocol_version") {
        out.protocol_version = static_cast<int>(v);
      } else {
        out.port = static_cast<std::uint16_t>(v);
      }
    } else if (key == "token") {
      if (!SkipWs(p, end) || !Match(p, end, "\"")) {
        return false;
      }
      while (p < end && *p != '"') {
        out.token.push_back(*p++);
      }
      if (p >= end) {
        return false;
      }
      ++p;  // 跳过右引号
    } else {
      return false;
    }
    if (!SkipWs(p, end)) {
      return false;
    }
    if (field < 2) {
      if (!Match(p, end, ",")) {
        return false;
      }
    }
  }
  if (!SkipWs(p, end) || !Match(p, end, "}")) {
    return false;
  }
  if (pf) {
    *pf = std::move(out);
  }
  return true;
}

}  // namespace rime_beautify::ipc

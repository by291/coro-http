#include "app/router.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace coro_http {

namespace {

std::vector<std::string_view> split_path(std::string_view path) {
  std::vector<std::string_view> segments;
  if (!path.empty() && path.front() == '/') {
    path.remove_prefix(1);
  }
  while (!path.empty()) {
    auto slash = path.find('/');
    if (slash == std::string_view::npos) {
      segments.push_back(path);
      break;
    }
    segments.push_back(path.substr(0, slash));
    path.remove_prefix(slash + 1);
  }
  return segments;
}

struct route_match {
  bool matched{false};
  std::vector<name_value> params;
};

route_match match_pattern(std::string_view pattern, std::string_view path) {
  auto pattern_segments = split_path(pattern);
  auto path_segments = split_path(path);
  if (pattern_segments.size() != path_segments.size()) {
    return {};
  }

  route_match result;
  result.matched = true;
  for (std::size_t i = 0; i < pattern_segments.size(); ++i) {
    std::string_view expected = pattern_segments[i];
    std::string_view actual = path_segments[i];

    if (!expected.empty() && expected.front() == ':') {
      if (actual.empty()) {
        return {};
      }
      result.params.push_back(name_value{std::string(expected.substr(1)), std::string(actual)});
      continue;
    }
    if (expected != actual) {
      return {};
    }
  }
  return result;
}

}  // namespace

void router::add(std::string method, std::string pattern, handler handle) {
  routes_.push_back(route{std::move(method), std::move(pattern), std::move(handle)});
}

task<response> router::dispatch(request req) const {
  std::string_view path = req.path();

  std::vector<std::pair<std::size_t, std::vector<name_value>>> path_matches;
  std::string allow;
  for (std::size_t i = 0; i < routes_.size(); ++i) {
    route_match match = match_pattern(routes_[i].pattern, path);
    if (!match.matched) {
      continue;
    }
    path_matches.emplace_back(i, std::move(match.params));

    if (const route& candidate = routes_[i]; candidate.method != req.method) {
      if (allow.find(candidate.method) == std::string::npos) {
        if (!allow.empty()) {
          allow += ", ";
        }
        allow += candidate.method;
      }
    }
  }

  for (auto& [index, params] : path_matches) {
    if (routes_[index].method == req.method) {
      req.params = std::move(params);
      co_return co_await routes_[index].handle(req);
    }
  }

  // HEAD is answered by the GET route; the serializer drops the body.
  if (req.method == "HEAD") {
    for (auto& [index, params] : path_matches) {
      if (routes_[index].method == "GET") {
        req.params = std::move(params);
        co_return co_await routes_[index].handle(req);
      }
    }
  }

  if (!path_matches.empty()) {
    response not_allowed = response::with_status(405);
    not_allowed.set_header("Allow", allow);
    co_return not_allowed;
  }

  co_return response::with_status(404);
}

}  // namespace coro_http

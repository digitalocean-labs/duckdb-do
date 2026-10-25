#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

// HTTP layer on top of libcurl. Uses no DuckDB types, so it stays unchanged
// when the DuckDB API layer moves to a newer C API.

struct HttpResponse {
	// HTTP status code, or 0 if no response arrived (DNS, TLS, timeout, ...)
	long status = 0;
	std::string body;
	// Header names are lower-cased, e.g. "x-ratelimit-remaining-requests"
	std::map<std::string, std::string> headers;
	// libcurl error message when status is 0
	std::string error;
	// True if the request gave up waiting (connect or total timeout)
	bool timed_out = false;
};

// Sends one POST per entry in `bodies` to `url`, concurrently. At most
// `max_concurrent` requests are in flight across the whole process (all DuckDB
// threads share the limit). Failed requests are retried as RetryDelay() decides.
// If a request finally gets no response at all, the requests not yet sent are
// skipped (status 0) so a hung service cannot hold the query for long.
// Returns one response per body, in the same order as `bodies`.
std::vector<HttpResponse> PostJsonConcurrent(const std::string &url, const std::string &bearer_token,
                                             const std::vector<std::string> &bodies, int64_t max_concurrent);

// Sends one GET to `url`, retrying as RetryDelay() decides. Not counted in the
// concurrency limit (used for small one-off calls such as listing models).
// An empty `bearer_token` sends no Authorization header.
HttpResponse GetJson(const std::string &url, const std::string &bearer_token);

// Retry policy, called after every finished attempt (including successful ones).
// `attempt` is 1 after the first try, 2 after the first retry, and so on.
// Returns how long to wait before trying again, or std::nullopt to stop and
// return `response` as the final result.
std::optional<std::chrono::milliseconds> RetryDelay(const HttpResponse &response, int attempt);

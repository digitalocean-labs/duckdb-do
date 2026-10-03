#include "common/http.hpp"

// Retry policy for all inference requests. See RetryDelay() in http.hpp.
//
// Useful facts from the live API (see CLAUDE.md, Part 3):
// - 429 means rate limited. Headers: x-ratelimit-remaining-requests,
//   x-ratelimit-reset-requests (seen as "0" so far, so do not rely on it alone).
// - 424 and 502 mean the upstream model provider failed; often temporary.
// - 400, 401, 403, 422 mean the request itself is wrong; retrying will not help.
// - status 0 means no response at all (timeout, DNS, TLS); see response.error.
std::optional<std::chrono::milliseconds> RetryDelay(const HttpResponse &response, int attempt) {
	// Retry only temporary failures: no response, rate limited, or server/upstream errors.
	bool temporary = response.status == 0 || response.status == 429 || response.status >= 500 ||
	                 response.status == 424;
	if (!temporary || attempt >= 3) {
		return std::nullopt;
	}
	// Wait 1 s after the first failure, 2 s after the second.
	return std::chrono::milliseconds(1000 * attempt);
}
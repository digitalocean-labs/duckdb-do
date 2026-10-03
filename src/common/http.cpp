#include "common/http.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <fstream>
#include <list>
#include <memory>
#include <mutex>
#include <thread>

namespace {

using Clock = std::chrono::steady_clock;

// Process-wide cap on requests in flight, shared by every DuckDB thread.
// C++17 has no std::counting_semaphore (that is C++20), so this is the classic
// mutex + condition variable version of one.
class InFlightLimiter {
public:
	// Blocks until a slot is free, then takes it.
	void Acquire(int64_t limit) {
		std::unique_lock<std::mutex> lock(mutex_);
		changed_.wait(lock, [&] { return in_flight_ < limit; });
		in_flight_++;
	}

	// Takes a slot if one is free right now. Never blocks.
	bool TryAcquire(int64_t limit) {
		std::lock_guard<std::mutex> lock(mutex_);
		if (in_flight_ >= limit) {
			return false;
		}
		in_flight_++;
		return true;
	}

	void Release() {
		{
			std::lock_guard<std::mutex> lock(mutex_);
			in_flight_--;
		}
		changed_.notify_all();
	}

private:
	std::mutex mutex_;
	std::condition_variable changed_;
	int64_t in_flight_ = 0;
};

InFlightLimiter &GlobalLimiter() {
	// A function-local static is created once, thread-safely, on first use.
	static InFlightLimiter limiter;
	return limiter;
}

void EnsureCurlGlobalInit() {
	static std::once_flag once;
	std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

// OpenSSL is linked statically, so its built-in CA path points into the build
// machine's vcpkg folder. Use the system CA bundle instead. Returns nullptr if
// none is found (e.g. on Windows, where curl uses the system store anyway).
const char *SystemCaBundle() {
	static const char *const candidates[] = {
	    "/etc/ssl/cert.pem",                  // macOS, Alpine
	    "/etc/ssl/certs/ca-certificates.crt", // Debian, Ubuntu
	    "/etc/pki/tls/certs/ca-bundle.crt",   // Fedora, RHEL
	    "/etc/ssl/ca-bundle.pem",             // openSUSE
	};
	for (const char *path : candidates) {
		if (std::ifstream(path).good()) {
			return path;
		}
	}
	return nullptr;
}

size_t AppendBody(char *data, size_t size, size_t count, void *user_data) {
	static_cast<std::string *>(user_data)->append(data, size * count);
	return size * count;
}

// Called once per header line, e.g. "X-Ratelimit-Limit-Requests: 960\r\n".
size_t StoreHeader(char *data, size_t size, size_t count, void *user_data) {
	auto &headers = *static_cast<std::map<std::string, std::string> *>(user_data);
	std::string line(data, size * count);
	auto colon = line.find(':');
	if (colon != std::string::npos) {
		std::string name = line.substr(0, colon);
		std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::tolower(c); });
		auto start = line.find_first_not_of(" \t", colon + 1);
		auto end = line.find_last_not_of(" \t\r\n");
		headers[name] = (start == std::string::npos || end < start) ? "" : line.substr(start, end - start + 1);
	}
	return size * count;
}

using HeaderList = std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)>;

HeaderList MakeHeaders(const std::string &bearer_token) {
	const std::string auth_header = "Authorization: Bearer " + bearer_token;
	curl_slist *headers = curl_slist_append(nullptr, "Content-Type: application/json");
	headers = curl_slist_append(headers, auth_header.c_str());
	return HeaderList(headers, &curl_slist_free_all);
}

// Options shared by every request: URL, headers, response capture, timeouts, TLS.
// libcurl keeps pointers to `response` and `error_buffer` until the request ends.
void SetCommonOptions(CURL *easy, const std::string &url, curl_slist *headers, HttpResponse &response,
                      char *error_buffer) {
	static const char *ca_bundle = SystemCaBundle();
	curl_easy_setopt(easy, CURLOPT_URL, url.c_str());
	curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, AppendBody);
	curl_easy_setopt(easy, CURLOPT_WRITEDATA, &response.body);
	curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, StoreHeader);
	curl_easy_setopt(easy, CURLOPT_HEADERDATA, &response.headers);
	curl_easy_setopt(easy, CURLOPT_ERRORBUFFER, error_buffer);
	curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
	curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, 60000L);
	// Required when using libcurl from multiple threads.
	curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
	if (ca_bundle) {
		curl_easy_setopt(easy, CURLOPT_CAINFO, ca_bundle);
	}
}

// One attempt of one request. Lives on the heap so libcurl can keep pointers
// to `response` and `error_buffer` while the transfer runs.
struct Transfer {
	size_t index = 0;
	int attempt = 0;
	HttpResponse response;
	char error_buffer[CURL_ERROR_SIZE] = {};
};

// A request waiting to start, possibly not before a retry delay has passed.
struct Pending {
	size_t index;
	int attempt;
	Clock::time_point not_before;
};

} // namespace

std::vector<HttpResponse> PostJsonConcurrent(const std::string &url, const std::string &bearer_token,
                                             const std::vector<std::string> &bodies, int64_t max_concurrent) {
	EnsureCurlGlobalInit();
	const int64_t limit = std::max<int64_t>(max_concurrent, 1);
	InFlightLimiter &limiter = GlobalLimiter();

	std::vector<HttpResponse> results(bodies.size());

	// unique_ptr with a custom deleter: the C handle is freed automatically
	// when this function returns, on every path (RAII).
	std::unique_ptr<CURLM, decltype(&curl_multi_cleanup)> multi(curl_multi_init(), &curl_multi_cleanup);

	HeaderList headers = MakeHeaders(bearer_token);

	std::list<Pending> pending;
	for (size_t i = 0; i < bodies.size(); i++) {
		pending.push_back({i, 0, Clock::time_point()});
	}
	std::map<CURL *, std::unique_ptr<Transfer>> running;

	auto start_transfer = [&](const Pending &request) {
		auto transfer = std::make_unique<Transfer>();
		transfer->index = request.index;
		transfer->attempt = request.attempt;
		const std::string &body = bodies[request.index];

		CURL *easy = curl_easy_init();
		SetCommonOptions(easy, url, headers.get(), transfer->response, transfer->error_buffer);
		// Not copied by libcurl: `bodies` outlives every transfer.
		curl_easy_setopt(easy, CURLOPT_POSTFIELDS, body.data());
		curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));

		curl_multi_add_handle(multi.get(), easy);
		running[easy] = std::move(transfer);
	};

	while (!pending.empty() || !running.empty()) {
		// 1. Start every ready request the shared limit allows.
		auto now = Clock::now();
		for (auto it = pending.begin(); it != pending.end();) {
			if (it->not_before > now) {
				++it;
				continue;
			}
			if (running.empty()) {
				// Nothing of ours in flight, so blocking cannot deadlock.
				limiter.Acquire(limit);
			} else if (!limiter.TryAcquire(limit)) {
				break;
			}
			start_transfer(*it);
			it = pending.erase(it);
		}

		// 2. Only delayed retries left: sleep until the earliest one is due.
		if (running.empty()) {
			auto earliest = std::min_element(pending.begin(), pending.end(), [](const Pending &a, const Pending &b) {
				return a.not_before < b.not_before;
			});
			if (earliest != pending.end()) {
				std::this_thread::sleep_until(earliest->not_before);
			}
			continue;
		}

		// 3. Let libcurl move data, waiting up to 100 ms for network activity.
		int still_running = 0;
		curl_multi_perform(multi.get(), &still_running);
		curl_multi_poll(multi.get(), nullptr, 0, 100, nullptr);
		curl_multi_perform(multi.get(), &still_running);

		// 4. Collect finished transfers.
		int messages_left = 0;
		while (CURLMsg *message = curl_multi_info_read(multi.get(), &messages_left)) {
			if (message->msg != CURLMSG_DONE) {
				continue;
			}
			CURL *easy = message->easy_handle;
			CURLcode result = message->data.result;

			std::unique_ptr<Transfer> transfer = std::move(running[easy]);
			running.erase(easy);
			if (result == CURLE_OK) {
				curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &transfer->response.status);
			} else {
				transfer->response.error =
				    transfer->error_buffer[0] ? transfer->error_buffer : curl_easy_strerror(result);
			}
			curl_multi_remove_handle(multi.get(), easy);
			curl_easy_cleanup(easy);
			limiter.Release();

			int attempt = transfer->attempt + 1;
			std::optional<std::chrono::milliseconds> delay = RetryDelay(transfer->response, attempt);
			if (delay) {
				pending.push_back({transfer->index, attempt, Clock::now() + *delay});
			} else {
				results[transfer->index] = std::move(transfer->response);
			}
		}
	}
	return results;
}
HttpResponse GetJson(const std::string &url, const std::string &bearer_token) {
	EnsureCurlGlobalInit();
	HeaderList headers = MakeHeaders(bearer_token);
	for (int attempt = 1;; attempt++) {
		HttpResponse response;
		char error_buffer[CURL_ERROR_SIZE] = {};
		std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> easy(curl_easy_init(), &curl_easy_cleanup);
		SetCommonOptions(easy.get(), url, headers.get(), response, error_buffer);

		CURLcode result = curl_easy_perform(easy.get());
		if (result == CURLE_OK) {
			curl_easy_getinfo(easy.get(), CURLINFO_RESPONSE_CODE, &response.status);
		} else {
			response.error = error_buffer[0] ? error_buffer : curl_easy_strerror(result);
		}

		std::optional<std::chrono::milliseconds> delay = RetryDelay(response, attempt);
		if (!delay) {
			return response;
		}
		std::this_thread::sleep_for(*delay);
	}
}

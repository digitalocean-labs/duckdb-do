#include "ai/prompt.hpp"

#include "ai/prompt_json.hpp"
#include "common/bind_args.hpp"
#include "common/config.hpp"
#include "common/duckdb_handles.hpp"
#include "common/http.hpp"

#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// do_prompt, the DuckDB side.
//
//   do_prompt(prompt)          uses the do_prompt_model setting
//   do_prompt(prompt, model)   model must be a constant
//
// Returns the reply as text. The chat API takes one conversation per request, so
// each distinct prompt in a chunk becomes one request, sent concurrently.

namespace {

constexpr const char *kChatUrl = "https://inference.do-ai.run/v1/chat/completions";

void PromptBind(duckdb_bind_info info) {
	try {
		auto data = std::make_unique<InferenceBind>();
		std::string error;
		if (!BindInference(info, kPromptModelSetting, *data, error)) {
			duckdb_scalar_function_bind_set_error(info, ("do_prompt: " + error).c_str());
			return;
		}
		// DuckDB now owns the bind data.
		SetScalarBindData(info, std::move(data));
	} catch (std::exception &e) {
		duckdb_scalar_function_bind_set_error(info, (std::string("do_prompt: ") + e.what()).c_str());
	}
}

void Execute(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
	auto *bind_data = static_cast<InferenceBind *>(duckdb_scalar_function_get_bind_data(info));
	if (!bind_data) {
		duckdb_scalar_function_set_error(info, "do_prompt: missing bind data");
		return;
	}
	const auto &data = *bind_data;
	idx_t row_count = duckdb_data_chunk_get_size(input);
	duckdb_vector prompt_vector = duckdb_data_chunk_get_vector(input, 0);
	auto *prompts = static_cast<duckdb_string_t *>(duckdb_vector_get_data(prompt_vector));
	uint64_t *validity = duckdb_vector_get_validity(prompt_vector);
	duckdb_vector_ensure_validity_writable(output);
	uint64_t *output_validity = duckdb_vector_get_validity(output);

	// One request per distinct prompt. row_body[row] is that row's request, or -1 for NULL.
	std::vector<std::string> bodies;
	std::unordered_map<std::string, size_t> seen;
	std::vector<long long> row_body(row_count, -1);
	for (idx_t row = 0; row < row_count; row++) {
		if (!duckdb_validity_row_is_valid(validity, row)) {
			continue;
		}
		std::string prompt(duckdb_string_t_data(&prompts[row]), duckdb_string_t_length(prompts[row]));
		auto found = seen.find(prompt);
		if (found != seen.end()) {
			row_body[row] = static_cast<long long>(found->second);
			continue;
		}
		std::string body = BuildChatBody(data.model, prompt);
		if (body.empty()) {
			continue;
		}
		seen.emplace(std::move(prompt), bodies.size());
		row_body[row] = static_cast<long long>(bodies.size());
		bodies.push_back(std::move(body));
	}

	std::vector<HttpResponse> responses = PostJsonConcurrent(kChatUrl, data.access_key, bodies, data.max_concurrent);
	std::vector<std::optional<std::string>> replies(responses.size());
	for (size_t i = 0; i < responses.size(); i++) {
		const HttpResponse &response = responses[i];
		// A rejected key or unknown model fails every row the same way: stop the query.
		if (response.status == 401 || response.status == 403 || response.status == 404) {
			std::string message = "do_prompt: request rejected (HTTP " + std::to_string(response.status) +
			                      "). Check the model access key and the model name. Response: " + response.body;
			duckdb_scalar_function_set_error(info, message.c_str());
			return;
		}
		if (response.status == 200) {
			replies[i] = ParseChatText(response.body);
		}
	}

	// Rows without a reply (NULL input or a failed request) are NULL.
	for (idx_t row = 0; row < row_count; row++) {
		const std::optional<std::string> *reply = row_body[row] >= 0 ? &replies[row_body[row]] : nullptr;
		if (reply && *reply) {
			duckdb_vector_assign_string_element_len(output, row, (*reply)->data(), (*reply)->size());
		} else {
			duckdb_validity_set_row_invalid(output_validity, row);
		}
	}
}

void PromptExecute(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
	try {
		Execute(info, input, output);
	} catch (std::exception &e) {
		duckdb_scalar_function_set_error(info, (std::string("do_prompt: ") + e.what()).c_str());
	}
}

} // namespace

bool RegisterPromptFunction(duckdb_connection connection) {
	ScalarFunctionSetHandle set(duckdb_create_scalar_function_set("do_prompt"));
	LogicalTypeHandle varchar(duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR));

	auto add_overload = [&](idx_t argument_count) {
		ScalarFunctionHandle function(duckdb_create_scalar_function());
		duckdb_scalar_function_set_name(function.get(), "do_prompt");
		for (idx_t i = 0; i < argument_count; i++) {
			duckdb_scalar_function_add_parameter(function.get(), varchar.get());
		}
		duckdb_scalar_function_set_return_type(function.get(), varchar.get());
		// The same prompt can give a different reply each time: never reuse results.
		duckdb_scalar_function_set_volatile(function.get());
		duckdb_scalar_function_set_bind(function.get(), PromptBind);
		duckdb_scalar_function_set_function(function.get(), PromptExecute);
		return duckdb_add_scalar_function_to_set(set.get(), function.get()) == DuckDBSuccess;
	};

	return add_overload(1) && add_overload(2) &&
	       duckdb_register_scalar_function_set(connection, set.get()) == DuckDBSuccess;
}

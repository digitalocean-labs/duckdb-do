#include "ai/embed.hpp"

#include "ai/embed_json.hpp"
#include "common/bind_args.hpp"
#include "common/config.hpp"
#include "common/duckdb_handles.hpp"
#include "common/http.hpp"

#include <algorithm>
#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// do_embed, the DuckDB side.
//
//   do_embed(text)          uses the do_embed_model setting
//   do_embed(text, model)   model must be a constant
//
// Returns the embedding as FLOAT[]. The embeddings API takes many texts per
// request, so distinct texts in a chunk are sent in batches, concurrently.

namespace {

constexpr const char *kEmbeddingsUrl = "https://inference.do-ai.run/v1/embeddings";
// Texts per request. The API embeds a request's texts one after another (about 0.1 s
// each), so many small requests in parallel are much faster than a few large ones.
// 16 keeps the request count low (good for rate limits) while staying fast.
constexpr size_t kBatchSize = 16;

void EmbedBind(duckdb_bind_info info) {
	try {
		auto data = std::make_unique<InferenceBind>();
		std::string error;
		if (!BindInference(info, kEmbedModelSetting, *data, error)) {
			duckdb_scalar_function_bind_set_error(info, ("do_embed: " + error).c_str());
			return;
		}
		// DuckDB now owns the bind data.
		SetScalarBindData(info, std::move(data));
	} catch (std::exception &e) {
		duckdb_scalar_function_bind_set_error(info, (std::string("do_embed: ") + e.what()).c_str());
	}
}

void Execute(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
	auto *bind_data = static_cast<InferenceBind *>(duckdb_scalar_function_get_bind_data(info));
	if (!bind_data) {
		duckdb_scalar_function_set_error(info, "do_embed: missing bind data");
		return;
	}
	const auto &data = *bind_data;
	idx_t row_count = duckdb_data_chunk_get_size(input);
	duckdb_vector text_vector = duckdb_data_chunk_get_vector(input, 0);
	auto *texts = static_cast<duckdb_string_t *>(duckdb_vector_get_data(text_vector));
	uint64_t *validity = duckdb_vector_get_validity(text_vector);

	// Distinct texts in this chunk. row_text[row] is that row's text index, or -1 for NULL.
	std::vector<std::string> unique;
	std::unordered_map<std::string, size_t> seen;
	std::vector<long long> row_text(row_count, -1);
	for (idx_t row = 0; row < row_count; row++) {
		if (!duckdb_validity_row_is_valid(validity, row)) {
			continue;
		}
		std::string text(duckdb_string_t_data(&texts[row]), duckdb_string_t_length(texts[row]));
		auto found = seen.emplace(text, unique.size());
		if (found.second) {
			unique.push_back(std::move(text));
		}
		row_text[row] = static_cast<long long>(found.first->second);
	}

	// One request per batch of kBatchSize texts.
	std::vector<std::string> bodies;
	for (size_t start = 0; start < unique.size(); start += kBatchSize) {
		size_t end = std::min(start + kBatchSize, unique.size());
		std::vector<std::string> batch(unique.begin() + start, unique.begin() + end);
		bodies.push_back(BuildEmbedBody(data.model, batch));
	}
	std::vector<HttpResponse> responses =
	    PostJsonConcurrent(kEmbeddingsUrl, data.access_key, bodies, data.max_concurrent);

	// vectors[i] is the embedding of unique[i]; empty if its batch failed.
	std::vector<std::vector<float>> vectors(unique.size());
	for (size_t b = 0; b < responses.size(); b++) {
		const HttpResponse &response = responses[b];
		if (response.status == 401 || response.status == 403 || response.status == 404) {
			std::string message = "do_embed: request rejected (HTTP " + std::to_string(response.status) +
			                      "). Check the model access key and the model name. Response: " + response.body;
			duckdb_scalar_function_set_error(info, message.c_str());
			return;
		}
		if (response.status != 200) {
			continue;
		}
		size_t start = b * kBatchSize;
		size_t expected = std::min(kBatchSize, unique.size() - start);
		std::string error;
		std::vector<std::vector<float>> batch = ParseEmbeddings(response.body, expected, error);
		for (size_t i = 0; i < batch.size(); i++) {
			vectors[start + i] = std::move(batch[i]);
		}
	}

	// Write the list result: one child buffer holding every row's numbers back to back,
	// plus an (offset, length) entry per row pointing into it.
	idx_t total = 0;
	for (idx_t row = 0; row < row_count; row++) {
		if (row_text[row] >= 0) {
			total += vectors[row_text[row]].size();
		}
	}
	duckdb_list_vector_reserve(output, total);
	duckdb_vector child = duckdb_list_vector_get_child(output);
	// Read the child buffer only after reserving: reserving can move it.
	auto *numbers = static_cast<float *>(duckdb_vector_get_data(child));
	auto *entries = static_cast<duckdb_list_entry *>(duckdb_vector_get_data(output));
	duckdb_vector_ensure_validity_writable(output);
	uint64_t *output_validity = duckdb_vector_get_validity(output);

	idx_t offset = 0;
	for (idx_t row = 0; row < row_count; row++) {
		const std::vector<float> *vec = row_text[row] >= 0 ? &vectors[row_text[row]] : nullptr;
		if (!vec || vec->empty()) {
			entries[row] = {offset, 0};
			duckdb_validity_set_row_invalid(output_validity, row);
			continue;
		}
		std::memcpy(numbers + offset, vec->data(), vec->size() * sizeof(float));
		entries[row] = {offset, vec->size()};
		offset += vec->size();
	}
	duckdb_list_vector_set_size(output, offset);
}

void EmbedExecute(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
	try {
		Execute(info, input, output);
	} catch (std::exception &e) {
		duckdb_scalar_function_set_error(info, (std::string("do_embed: ") + e.what()).c_str());
	}
}

} // namespace

bool RegisterEmbedFunction(duckdb_connection connection) {
	ScalarFunctionSetHandle set(duckdb_create_scalar_function_set("do_embed"));
	LogicalTypeHandle varchar(duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR));
	LogicalTypeHandle number(duckdb_create_logical_type(DUCKDB_TYPE_FLOAT));
	LogicalTypeHandle vector_type(duckdb_create_list_type(number.get()));

	auto add_overload = [&](idx_t argument_count) {
		ScalarFunctionHandle function(duckdb_create_scalar_function());
		duckdb_scalar_function_set_name(function.get(), "do_embed");
		for (idx_t i = 0; i < argument_count; i++) {
			duckdb_scalar_function_add_parameter(function.get(), varchar.get());
		}
		duckdb_scalar_function_set_return_type(function.get(), vector_type.get());
		// Not volatile: the same text and model give the same vector, so DuckDB may
		// compute a constant input (such as a search phrase) once.
		duckdb_scalar_function_set_bind(function.get(), EmbedBind);
		duckdb_scalar_function_set_function(function.get(), EmbedExecute);
		return duckdb_add_scalar_function_to_set(set.get(), function.get()) == DuckDBSuccess;
	};

	return add_overload(1) && add_overload(2) &&
	       duckdb_register_scalar_function_set(connection, set.get()) == DuckDBSuccess;
}

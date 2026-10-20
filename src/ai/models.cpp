#include "ai/models.hpp"

#include "ai/models_json.hpp"
#include "common/config.hpp"
#include "common/duckdb_handles.hpp"
#include "common/http.hpp"

#include <exception>
#include <memory>
#include <string>
#include <vector>

// do_models, the DuckDB side. A table function runs in three steps:
//   bind:     once per query; declares the output columns
//   init:     once per query, when it executes; fetches the model list
//   function: called repeatedly; each call fills one chunk of rows, 0 rows means done
//
//   FROM do_models();

namespace {

// One call: the serverless models of the catalog (dedicated-only models excluded).
constexpr const char *kModelsUrl =
    "https://api.digitalocean.com/v2/gen-ai/models/catalog?availability=serverless&per_page=500";

struct ModelsBindData {
	std::string access_key;
};

struct ModelsState {
	std::vector<ModelInfo> models;
	// Index of the next model to output
	size_t next = 0;
};

void ModelsBind(duckdb_bind_info info) {
	try {
		duckdb_client_context raw_context = nullptr;
		duckdb_table_function_get_client_context(info, &raw_context);
		ClientContextHandle context(raw_context);

		auto data = std::make_unique<ModelsBindData>();
		data->access_key = GetModelAccessKey(context.get());
		if (data->access_key.empty()) {
			duckdb_bind_set_error(info, "do_models: no model access key. Set the DO_MODEL_ACCESS_KEY environment variable");
			return;
		}

		LogicalTypeHandle varchar(duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR));
		LogicalTypeHandle bigint(duckdb_create_logical_type(DUCKDB_TYPE_BIGINT));
		duckdb_bind_add_result_column(info, "name", varchar.get());
		duckdb_bind_add_result_column(info, "type", varchar.get());
		duckdb_bind_add_result_column(info, "description", varchar.get());
		duckdb_bind_add_result_column(info, "creator", varchar.get());
		duckdb_bind_add_result_column(info, "context_length", bigint.get());
		duckdb_bind_add_result_column(info, "max_output_tokens", bigint.get());

		duckdb_bind_set_bind_data(info, data.release(), [](void *ptr) { delete static_cast<ModelsBindData *>(ptr); });
	} catch (std::exception &e) {
		duckdb_bind_set_error(info, (std::string("do_models: ") + e.what()).c_str());
	}
}

void ModelsInit(duckdb_init_info info) {
	try {
		const auto &data = *static_cast<ModelsBindData *>(duckdb_init_get_bind_data(info));
		HttpResponse response = GetJson(kModelsUrl, data.access_key);
		if (response.status != 200) {
			std::string detail = response.status == 0 ? response.error : response.body;
			std::string message =
			    "do_models: request failed (HTTP " + std::to_string(response.status) + "): " + detail;
			duckdb_init_set_error(info, message.c_str());
			return;
		}

		auto state = std::make_unique<ModelsState>();
		std::string error;
		state->models = ParseModels(response.body, error);
		if (!error.empty()) {
			duckdb_init_set_error(info, ("do_models: " + error).c_str());
			return;
		}
		duckdb_init_set_init_data(info, state.release(), [](void *ptr) { delete static_cast<ModelsState *>(ptr); });
	} catch (std::exception &e) {
		duckdb_init_set_error(info, (std::string("do_models: ") + e.what()).c_str());
	}
}

void SetNull(duckdb_vector vector, idx_t row) {
	duckdb_vector_ensure_validity_writable(vector);
	duckdb_validity_set_row_invalid(duckdb_vector_get_validity(vector), row);
}

// Writes a string, or NULL when it is empty.
void SetString(duckdb_vector vector, idx_t row, const std::string &value) {
	if (value.empty()) {
		SetNull(vector, row);
	} else {
		duckdb_vector_assign_string_element_len(vector, row, value.data(), value.size());
	}
}

// Writes a value, or NULL when the optional is empty.
void SetOptionalInt(duckdb_vector vector, idx_t row, const std::optional<int64_t> &value) {
	if (value) {
		static_cast<int64_t *>(duckdb_vector_get_data(vector))[row] = *value;
	} else {
		SetNull(vector, row);
	}
}

void ModelsFunction(duckdb_function_info info, duckdb_data_chunk output) {
	auto &state = *static_cast<ModelsState *>(duckdb_function_get_init_data(info));
	duckdb_vector name = duckdb_data_chunk_get_vector(output, 0);
	duckdb_vector type = duckdb_data_chunk_get_vector(output, 1);
	duckdb_vector description = duckdb_data_chunk_get_vector(output, 2);
	duckdb_vector creator = duckdb_data_chunk_get_vector(output, 3);
	duckdb_vector context_length = duckdb_data_chunk_get_vector(output, 4);
	duckdb_vector max_output_tokens = duckdb_data_chunk_get_vector(output, 5);

	idx_t row = 0;
	while (state.next < state.models.size() && row < duckdb_vector_size()) {
		const ModelInfo &model = state.models[state.next];
		SetString(name, row, model.name);
		SetString(type, row, model.type);
		SetString(description, row, model.description);
		SetString(creator, row, model.creator);
		SetOptionalInt(context_length, row, model.context_length);
		SetOptionalInt(max_output_tokens, row, model.max_output_tokens);
		state.next++;
		row++;
	}
	duckdb_data_chunk_set_size(output, row);
}

} // namespace

bool RegisterModelsFunction(duckdb_connection connection) {
	TableFunctionHandle function(duckdb_create_table_function());
	duckdb_table_function_set_name(function.get(), "do_models");
	duckdb_table_function_set_bind(function.get(), ModelsBind);
	duckdb_table_function_set_init(function.get(), ModelsInit);
	duckdb_table_function_set_function(function.get(), ModelsFunction);
	return duckdb_register_table_function(connection, function.get()) == DuckDBSuccess;
}
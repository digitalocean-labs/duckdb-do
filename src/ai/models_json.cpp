#include "ai/models_json.hpp"

#include <yyjson.h>

#include <cstdlib>
#include <memory>

namespace {

std::string GetString(yyjson_val *object, const char *key) {
	const char *value = yyjson_get_str(yyjson_obj_get(object, key));
	return value ? value : "";
}

// The catalog sends numbers as strings, e.g. "context_window": "128000".
std::optional<int64_t> GetInt(yyjson_val *object, const char *key) {
	yyjson_val *value = yyjson_obj_get(object, key);
	if (yyjson_is_num(value)) {
		return static_cast<int64_t>(yyjson_get_num(value));
	}
	const char *text = yyjson_get_str(value);
	if (!text || !*text) {
		return std::nullopt;
	}
	char *end = nullptr;
	long long number = std::strtoll(text, &end, 10);
	if (*end != '\0') {
		return std::nullopt;
	}
	return number;
}

} // namespace

std::vector<ModelInfo> ParseModels(const std::string &body, std::string &error) {
	std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)> doc(yyjson_read(body.data(), body.size(), 0),
	                                                            &yyjson_doc_free);
	yyjson_val *data = doc ? yyjson_obj_get(yyjson_doc_get_root(doc.get()), "data") : nullptr;
	if (!yyjson_is_arr(data)) {
		error = "unexpected response from the model catalog";
		return {};
	}

	std::vector<ModelInfo> models;
	size_t index, count;
	yyjson_val *item;
	yyjson_arr_foreach(data, index, count, item) {
		ModelInfo model;
		model.name = GetString(item, "model_id");
		model.type = GetString(item, "type");
		model.description = GetString(item, "short_description");
		model.creator = GetString(item, "creator");
		model.context_length = GetInt(item, "context_window");
		model.max_output_tokens = GetInt(item, "max_output_tokens");
		models.push_back(std::move(model));
	}
	return models;
}
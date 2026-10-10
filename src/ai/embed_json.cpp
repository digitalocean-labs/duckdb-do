#include "ai/embed_json.hpp"

#include <yyjson.h>

#include <cstdlib>
#include <memory>

std::string BuildEmbedBody(const std::string &model, const std::vector<std::string> &texts) {
	std::unique_ptr<yyjson_mut_doc, decltype(&yyjson_mut_doc_free)> doc(yyjson_mut_doc_new(nullptr), &yyjson_mut_doc_free);
	yyjson_mut_doc *d = doc.get();
	yyjson_mut_val *root = yyjson_mut_obj(d);
	yyjson_mut_obj_add_strncpy(d, root, "model", model.data(), model.size());
	yyjson_mut_val *input = yyjson_mut_arr(d);
	for (const std::string &text : texts) {
		yyjson_mut_arr_add_strncpy(d, input, text.data(), text.size());
	}
	yyjson_mut_obj_add_val(d, root, "input", input);
	yyjson_mut_doc_set_root(d, root);

	size_t length = 0;
	std::unique_ptr<char, decltype(&free)> json(yyjson_mut_write(d, 0, &length), &free);
	return json ? std::string(json.get(), length) : std::string();
}

std::vector<std::vector<float>> ParseEmbeddings(const std::string &body, size_t expected, std::string &error) {
	std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)> doc(yyjson_read(body.data(), body.size(), 0),
	                                                            &yyjson_doc_free);
	// {"data": [{"index": 0, "embedding": [0.1, ...]}, ...]}
	yyjson_val *data = doc ? yyjson_obj_get(yyjson_doc_get_root(doc.get()), "data") : nullptr;
	if (!yyjson_is_arr(data) || yyjson_arr_size(data) != expected) {
		error = "unexpected response from the embeddings endpoint";
		return {};
	}
	std::vector<std::vector<float>> vectors(expected);
	size_t i, count;
	yyjson_val *item;
	yyjson_arr_foreach(data, i, count, item) {
		yyjson_val *index = yyjson_obj_get(item, "index");
		yyjson_val *embedding = yyjson_obj_get(item, "embedding");
		if (!yyjson_is_uint(index) || yyjson_get_uint(index) >= expected || !yyjson_is_arr(embedding)) {
			error = "unexpected embedding entry in the response";
			return {};
		}
		std::vector<float> &vec = vectors[yyjson_get_uint(index)];
		vec.reserve(yyjson_arr_size(embedding));
		size_t j, n;
		yyjson_val *number;
		yyjson_arr_foreach(embedding, j, n, number) {
			vec.push_back(static_cast<float>(yyjson_get_num(number)));
		}
	}
	return vectors;
}

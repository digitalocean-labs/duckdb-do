#include "ai/prompt_json.hpp"

#include <yyjson.h>

#include <cstdlib>
#include <memory>

std::string BuildChatBody(const std::string &model, const std::string &prompt) {
	std::unique_ptr<yyjson_mut_doc, decltype(&yyjson_mut_doc_free)> doc(yyjson_mut_doc_new(nullptr), &yyjson_mut_doc_free);
	yyjson_mut_doc *d = doc.get();
	yyjson_mut_val *root = yyjson_mut_obj(d);
	yyjson_mut_obj_add_strncpy(d, root, "model", model.data(), model.size());
	yyjson_mut_val *message = yyjson_mut_obj(d);
	yyjson_mut_obj_add_str(d, message, "role", "user");
	yyjson_mut_obj_add_strncpy(d, message, "content", prompt.data(), prompt.size());
	yyjson_mut_val *messages = yyjson_mut_arr(d);
	yyjson_mut_arr_append(messages, message);
	yyjson_mut_obj_add_val(d, root, "messages", messages);
	yyjson_mut_doc_set_root(d, root);

	size_t length = 0;
	std::unique_ptr<char, decltype(&free)> json(yyjson_mut_write(d, 0, &length), &free);
	return json ? std::string(json.get(), length) : std::string();
}

std::optional<std::string> ParseChatText(const std::string &body) {
	std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)> doc(yyjson_read(body.data(), body.size(), 0),
	                                                            &yyjson_doc_free);
	if (!doc) {
		return std::nullopt;
	}
	// {"choices": [{"message": {"content": "..."}}]}
	yyjson_val *choices = yyjson_obj_get(yyjson_doc_get_root(doc.get()), "choices");
	yyjson_val *message = yyjson_obj_get(yyjson_arr_get_first(choices), "message");
	yyjson_val *content = yyjson_obj_get(message, "content");
	if (!yyjson_is_str(content) || yyjson_get_len(content) == 0) {
		return std::nullopt;
	}
	return std::string(yyjson_get_str(content), yyjson_get_len(content));
}

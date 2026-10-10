#pragma once

#include "duckdb_extension.h"

#include <memory>
#include <string>

// Every DuckDB API call goes through the `duckdb_ext_api` function table.
// Declaring it here lets any file that includes this header call the API.
DUCKDB_EXTENSION_EXTERN

// RAII wrapper for a DuckDB C API handle: owns the handle and frees it in the
// destructor, so it is released on every code path (normal return or early exit).
// `Destroyer` is a small struct whose operator() calls the matching duckdb_destroy_*.
// Copying is disabled (two owners would free the handle twice); moving transfers ownership.
template <class T, class Destroyer>
class Handle {
public:
	Handle() = default;
	explicit Handle(T handle) : handle_(handle) {
	}
	~Handle() {
		reset();
	}

	Handle(const Handle &) = delete;
	Handle &operator=(const Handle &) = delete;

	Handle(Handle &&other) noexcept : handle_(other.handle_) {
		other.handle_ = nullptr;
	}
	Handle &operator=(Handle &&other) noexcept {
		if (this != &other) {
			reset();
			handle_ = other.handle_;
			other.handle_ = nullptr;
		}
		return *this;
	}

	T get() const {
		return handle_;
	}
	explicit operator bool() const {
		return handle_ != nullptr;
	}
	void reset() {
		if (handle_) {
			Destroyer()(&handle_);
			handle_ = nullptr;
		}
	}

private:
	T handle_ = nullptr;
};

struct DestroyValue {
	void operator()(duckdb_value *value) const {
		duckdb_destroy_value(value);
	}
};
struct DestroyExpression {
	void operator()(duckdb_expression *expression) const {
		duckdb_destroy_expression(expression);
	}
};
struct DestroyClientContext {
	void operator()(duckdb_client_context *context) const {
		duckdb_destroy_client_context(context);
	}
};
struct DestroyLogicalType {
	void operator()(duckdb_logical_type *type) const {
		duckdb_destroy_logical_type(type);
	}
};
struct DestroyScalarFunction {
	void operator()(duckdb_scalar_function *function) const {
		duckdb_destroy_scalar_function(function);
	}
};
struct DestroyScalarFunctionSet {
	void operator()(duckdb_scalar_function_set *set) const {
		duckdb_destroy_scalar_function_set(set);
	}
};
struct DestroyTableFunction {
	void operator()(duckdb_table_function *function) const {
		duckdb_destroy_table_function(function);
	}
};
struct DestroyErrorData {
	void operator()(duckdb_error_data *error) const {
		duckdb_destroy_error_data(error);
	}
};

using ValueHandle = Handle<duckdb_value, DestroyValue>;
using ExpressionHandle = Handle<duckdb_expression, DestroyExpression>;
using ClientContextHandle = Handle<duckdb_client_context, DestroyClientContext>;
using LogicalTypeHandle = Handle<duckdb_logical_type, DestroyLogicalType>;
using ScalarFunctionHandle = Handle<duckdb_scalar_function, DestroyScalarFunction>;
using ScalarFunctionSetHandle = Handle<duckdb_scalar_function_set, DestroyScalarFunctionSet>;
using TableFunctionHandle = Handle<duckdb_table_function, DestroyTableFunction>;
using ErrorDataHandle = Handle<duckdb_error_data, DestroyErrorData>;

// Copies a C string that DuckDB allocated, then frees it with duckdb_free.
// Use for duckdb_get_varchar() and duckdb_struct_type_child_name().
inline std::string TakeDuckDBString(char *text) {
	std::string result = text ? text : "";
	duckdb_free(text);
	return result;
}

// Hands a scalar function's bind data to DuckDB with both callbacks it needs.
// DuckDB copies bind data when the optimizer copies an expression (for example
// ORDER BY ... LIMIT). Without the copy callback the copy has no bind data and
// execute reads a null pointer. A failed copy returns nullptr: execute checks it.
template <class T>
void SetScalarBindData(duckdb_bind_info info, std::unique_ptr<T> data) {
	duckdb_scalar_function_set_bind_data(info, data.release(), [](void *ptr) { delete static_cast<T *>(ptr); });
	duckdb_scalar_function_set_bind_data_copy(info, [](void *ptr) -> void * {
		try {
			return new T(*static_cast<const T *>(ptr));
		} catch (...) {
			return nullptr;
		}
	});
}

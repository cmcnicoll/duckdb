#include "json_common.hpp"
#include "json_functions.hpp"

namespace duckdb {

//! Forward declaration for recursion
static inline yyjson_mut_val *GetConsistentArrayStructure(const vector<yyjson_mut_val *> &elem_structures,
                                                          yyjson_mut_doc *structure_doc);

static inline yyjson_mut_val *GetConsistentArrayStructureArray(const vector<yyjson_mut_val *> &elem_structures,
                                                               yyjson_mut_doc *structure_doc) {
	yyjson_mut_val *val;
	yyjson_mut_arr_iter iter;
	vector<yyjson_mut_val *> nested_elem_structures;
	for (const auto &elem_structure : elem_structures) {
		yyjson_mut_arr_iter_init(elem_structure, &iter);
		while ((val = yyjson_mut_arr_iter_next(&iter))) {
			nested_elem_structures.push_back(val);
		}
	}
	auto result = yyjson_mut_arr(structure_doc);
	yyjson_mut_arr_append(result, GetConsistentArrayStructure(nested_elem_structures, structure_doc));
	return result;
}

static inline yyjson_mut_val *GetConsistentArrayStructureObject(const vector<yyjson_mut_val *> &elem_structures,
                                                                yyjson_mut_doc *structure_doc) {
	yyjson_mut_val *key, *val;
	yyjson_mut_obj_iter iter;
	vector<string> key_insert_order;
	unordered_map<string, vector<yyjson_mut_val *>> key_values;
	for (const auto &elem_structure : elem_structures) {
		yyjson_mut_obj_iter_init(elem_structure, &iter);
		while ((key = yyjson_mut_obj_iter_next(&iter))) {
			auto key_string = yyjson_mut_get_str(key);
			val = yyjson_mut_obj_iter_get_val(key);
			if (key_values.find(key_string) == key_values.end()) {
				key_insert_order.emplace_back(key_string);
			}
			key_values[key_string].push_back(val);
		}
	}
	auto result = yyjson_mut_obj(structure_doc);
	for (const auto &key_string : key_insert_order) {
		key = yyjson_mut_strncpy(structure_doc, key_string.c_str(), key_string.size());
		val = GetConsistentArrayStructure(key_values.at(key_string), structure_doc);
		yyjson_mut_obj_add(result, key, val);
	}
	return result;
}

static inline bool StringEquals(const char *lhs, const char *rhs) {
	return strcmp(lhs, rhs) == 0;
}

static inline bool EitherEquals(const char *s1, const char *s2, const char *target) {
	return StringEquals(s1, target) || StringEquals(s2, target);
}

static inline void GetMaxTypeString(const char *type_string, const char *elem_type_string, const char **result) {
	if (!type_string) {
		*result = elem_type_string;
	} else if (!elem_type_string) {
		*result = type_string;
	} else if (StringEquals(type_string, elem_type_string)) {
		*result = type_string;
	} else if (EitherEquals(type_string, elem_type_string, JSONCommon::TYPE_STRING_VARCHAR)) {
		*result = JSONCommon::TYPE_STRING_VARCHAR;
	} else if (EitherEquals(type_string, elem_type_string, JSONCommon::TYPE_STRING_DOUBLE)) {
		*result = JSONCommon::TYPE_STRING_DOUBLE;
	} else if (EitherEquals(type_string, elem_type_string, JSONCommon::TYPE_STRING_BIGINT)) {
		*result = JSONCommon::TYPE_STRING_BIGINT;
	} else if (EitherEquals(type_string, elem_type_string, JSONCommon::TYPE_STRING_UBIGINT)) {
		*result = JSONCommon::TYPE_STRING_UBIGINT;
	} else {
		D_ASSERT(StringEquals(type_string, JSONCommon::TYPE_STRING_BOOLEAN) &&
		         StringEquals(elem_type_string, JSONCommon::TYPE_STRING_BOOLEAN));
		*result = JSONCommon::TYPE_STRING_BOOLEAN;
	}
}

static inline yyjson_mut_val *GetConsistentArrayStructure(const vector<yyjson_mut_val *> &elem_structures,
                                                          yyjson_mut_doc *structure_doc) {
	if (elem_structures.empty()) {
		return yyjson_mut_str(structure_doc, JSONCommon::TYPE_STRING_NULL);
	}
	auto type = yyjson_mut_get_type(elem_structures[0]);
	auto type_string = yyjson_mut_get_str(elem_structures[0]);
	for (idx_t i = 1; i < elem_structures.size(); i++) {
		auto elem_type = yyjson_mut_get_type(elem_structures[i]);
		auto elem_type_string = yyjson_mut_get_str(elem_structures[i]);
		if (type_string && strcmp(type_string, JSONCommon::TYPE_STRING_NULL) == 0) {
			// Iterate until we find non-null
			type = elem_type;
			type_string = elem_type_string;
			continue;
		}
		if (elem_type_string && strcmp(elem_type_string, JSONCommon::TYPE_STRING_NULL) == 0) {
			// Skip over encountered nulls after we found one
			continue;
		}
		if (type != elem_type) {
			throw InvalidInputException("Inconsistent JSON structure");
		}
		GetMaxTypeString(type_string, elem_type_string, &type_string);
	}
	switch (type) {
	case YYJSON_TYPE_ARR:
		return GetConsistentArrayStructureArray(elem_structures, structure_doc);
	case YYJSON_TYPE_OBJ:
		return GetConsistentArrayStructureObject(elem_structures, structure_doc);
	case YYJSON_TYPE_STR:
		return yyjson_mut_str(structure_doc, type_string);
	default:
		throw InternalException("Unexpected JSON type arrived at GetConsistentArrayStructure");
	}
}

//! Forward declaration for recursion
static inline yyjson_mut_val *BuildStructure(yyjson_val *val, yyjson_mut_doc *structure_doc);

static inline yyjson_mut_val *BuildStructureArray(yyjson_val *arr, yyjson_mut_doc *structure_doc) {
	vector<yyjson_mut_val *> elem_structures;
	// Iterate over array
	yyjson_val *val;
	yyjson_arr_iter iter;
	yyjson_arr_iter_init(arr, &iter);
	while ((val = yyjson_arr_iter_next(&iter))) {
		elem_structures.push_back(BuildStructure(val, structure_doc));
	}
	// Array is consistent if it is empty, or if all its elements have the same type (NULL is fine too)
	// If the array has nested types, we need to verify that these match too
	// We combine the structures in the array and try to return a structure without nulls
	auto result = yyjson_mut_arr(structure_doc);
	yyjson_mut_arr_append(result, GetConsistentArrayStructure(elem_structures, structure_doc));
	return result;
}

static inline yyjson_mut_val *BuildStructureObject(yyjson_val *obj, yyjson_mut_doc *structure_doc) {
	auto result = yyjson_mut_obj(structure_doc);
	// Iterate over object
	yyjson_val *key, *val;
	yyjson_obj_iter iter;
	yyjson_obj_iter_init(obj, &iter);
	while ((key = yyjson_obj_iter_next(&iter))) {
		val = yyjson_obj_iter_get_val(key);
		yyjson_mut_obj_add(result, yyjson_val_mut_copy(structure_doc, key), BuildStructure(val, structure_doc));
	}
	return result;
}

static inline yyjson_mut_val *BuildStructure(yyjson_val *val, yyjson_mut_doc *structure_doc) {
	switch (yyjson_get_type(val)) {
	case YYJSON_TYPE_ARR:
		return BuildStructureArray(val, structure_doc);
	case YYJSON_TYPE_OBJ:
		return BuildStructureObject(val, structure_doc);
	default:
		return yyjson_mut_str(structure_doc, JSONCommon::ValTypeToString(val));
	}
}

static inline bool Structure(yyjson_val *val, string_t &result_val, Vector &result) {
	auto structure_doc = JSONCommon::CreateDocument();
	yyjson_mut_doc_set_root(*structure_doc, BuildStructure(val, *structure_doc));
	result_val = JSONCommon::WriteVal(*structure_doc, result);
	return true;
}

static void StructureFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	JSONCommon::UnaryExecute<string_t>(args, state, result, Structure);
}

CreateScalarFunctionInfo JSONFunctions::GetStructureFunction() {
	return CreateScalarFunctionInfo(ScalarFunction("json_structure", {LogicalType::JSON}, LogicalType::JSON,
	                                               StructureFunction, false, nullptr, nullptr, nullptr));
}

} // namespace duckdb

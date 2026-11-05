#include "storage/ducklake_metadata_schema.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {

static const vector<DuckLakeMetadataTable> &MetadataTables() {
	// clang-format off
	static const vector<DuckLakeMetadataTable> tables {
		{"ducklake_metadata", {
			{"key", "VARCHAR NOT NULL"},
			{"value", "VARCHAR NOT NULL"},
			{"scope", "VARCHAR"},
			{"scope_id", "BIGINT"},
		}},
		{"ducklake_snapshot", {
			{"snapshot_id", "BIGINT PRIMARY KEY"},
			{"snapshot_time", "TIMESTAMPTZ"},
			{"schema_version", "BIGINT"},
			{"next_catalog_id", "BIGINT"},
			{"next_file_id", "BIGINT"},
		}},
		{"ducklake_snapshot_changes", {
			{"snapshot_id", "BIGINT PRIMARY KEY"},
			{"changes_made", "VARCHAR"},
			{"author", "VARCHAR"},
			{"commit_message", "VARCHAR"},
			{"commit_extra_info", "VARCHAR"},
		}},
		{"ducklake_schema", {
			{"schema_id", "BIGINT PRIMARY KEY"},
			{"schema_uuid", "UUID"},
			{"begin_snapshot", "BIGINT"},
			{"end_snapshot", "BIGINT"},
			{"schema_name", "VARCHAR"},
			{"path", "VARCHAR"},
			{"path_is_relative", "BOOLEAN"},
			{"parent_schema_id", "BIGINT", DuckLakeVersion::V1_1},
		}},
		{"ducklake_table", {
			{"table_id", "BIGINT"},
			{"table_uuid", "UUID"},
			{"begin_snapshot", "BIGINT"},
			{"end_snapshot", "BIGINT"},
			{"schema_id", "BIGINT"},
			{"table_name", "VARCHAR"},
			{"path", "VARCHAR"},
			{"path_is_relative", "BOOLEAN"},
		}},
		{"ducklake_view", {
			{"view_id", "BIGINT"},
			{"view_uuid", "UUID"},
			{"begin_snapshot", "BIGINT"},
			{"end_snapshot", "BIGINT"},
			{"schema_id", "BIGINT"},
			{"view_name", "VARCHAR"},
			{"dialect", "VARCHAR"},
			{"sql", "VARCHAR"},
			{"column_aliases", "VARCHAR"},
		}},
		{"ducklake_tag", {
			{"object_id", "BIGINT"},
			{"begin_snapshot", "BIGINT"},
			{"end_snapshot", "BIGINT"},
			{"key", "VARCHAR"},
			{"value", "VARCHAR"},
		}},
		{"ducklake_column_tag", {
			{"table_id", "BIGINT"},
			{"column_id", "BIGINT"},
			{"begin_snapshot", "BIGINT"},
			{"end_snapshot", "BIGINT"},
			{"key", "VARCHAR"},
			{"value", "VARCHAR"},
		}},
		{"ducklake_data_file", {
			{"data_file_id", "BIGINT PRIMARY KEY"},
			{"table_id", "BIGINT"},
			{"begin_snapshot", "BIGINT"},
			{"end_snapshot", "BIGINT"},
			{"file_order", "BIGINT"},
			{"path", "VARCHAR"},
			{"path_is_relative", "BOOLEAN"},
			{"file_format", "VARCHAR"},
			{"record_count", "BIGINT"},
			{"file_size_bytes", "BIGINT"},
			{"footer_size", "BIGINT"},
			{"row_id_start", "BIGINT"},
			{"partition_id", "BIGINT"},
			{"encryption_key", "VARCHAR"},
			{"mapping_id", "BIGINT"},
			{"partial_max", "BIGINT"},
			{"row_group_count", "BIGINT", DuckLakeVersion::V1_1},
		}},
		{"ducklake_file_column_stats", {
			{"data_file_id", "BIGINT"},
			{"table_id", "BIGINT"},
			{"column_id", "BIGINT"},
			{"column_size_bytes", "BIGINT"},
			{"value_count", "BIGINT"},
			{"null_count", "BIGINT"},
			{"min_value", "VARCHAR"},
			{"max_value", "VARCHAR"},
			{"contains_nan", "BOOLEAN"},
			{"extra_stats", "VARCHAR"},
			{"min_is_exact", "BOOLEAN", DuckLakeVersion::V1_1},
			{"max_is_exact", "BOOLEAN", DuckLakeVersion::V1_1},
		}},
		{"ducklake_file_variant_stats", {
			{"data_file_id", "BIGINT"},
			{"table_id", "BIGINT"},
			{"column_id", "BIGINT"},
			{"variant_path", "VARCHAR"},
			{"shredded_type", "VARCHAR"},
			{"column_size_bytes", "BIGINT"},
			{"value_count", "BIGINT"},
			{"null_count", "BIGINT"},
			{"min_value", "VARCHAR"},
			{"max_value", "VARCHAR"},
			{"contains_nan", "BOOLEAN"},
			{"extra_stats", "VARCHAR"},
		}},
		{"ducklake_delete_file", {
			{"delete_file_id", "BIGINT PRIMARY KEY"},
			{"table_id", "BIGINT"},
			{"begin_snapshot", "BIGINT"},
			{"end_snapshot", "BIGINT"},
			{"data_file_id", "BIGINT"},
			{"path", "VARCHAR"},
			{"path_is_relative", "BOOLEAN"},
			{"format", "VARCHAR"},
			{"delete_count", "BIGINT"},
			{"file_size_bytes", "BIGINT"},
			{"footer_size", "BIGINT"},
			{"encryption_key", "VARCHAR"},
			{"partial_max", "BIGINT"},
			{"row_group_count", "BIGINT", DuckLakeVersion::V1_1},
		}},
		{"ducklake_column", {
			{"column_id", "BIGINT"},
			{"begin_snapshot", "BIGINT"},
			{"end_snapshot", "BIGINT"},
			{"table_id", "BIGINT"},
			{"column_order", "BIGINT"},
			{"column_name", "VARCHAR"},
			{"column_type", "VARCHAR"},
			{"initial_default", "VARCHAR"},
			{"default_value", "VARCHAR"},
			{"nulls_allowed", "BOOLEAN"},
			{"parent_column", "BIGINT"},
			{"default_value_type", "VARCHAR"},
			{"default_value_dialect", "VARCHAR"},
		}},
		{"ducklake_table_stats", {
			{"table_id", "BIGINT"},
			{"record_count", "BIGINT"},
			{"next_row_id", "BIGINT"},
			{"file_size_bytes", "BIGINT"},
		}},
		{"ducklake_table_column_stats", {
			{"table_id", "BIGINT"},
			{"column_id", "BIGINT"},
			{"contains_null", "BOOLEAN"},
			{"contains_nan", "BOOLEAN"},
			{"min_value", "VARCHAR"},
			{"max_value", "VARCHAR"},
			{"extra_stats", "VARCHAR"},
			{"min_is_exact", "BOOLEAN", DuckLakeVersion::V1_1},
			{"max_is_exact", "BOOLEAN", DuckLakeVersion::V1_1},
		}},
		{"ducklake_partition_info", {
			{"partition_id", "BIGINT"},
			{"table_id", "BIGINT"},
			{"begin_snapshot", "BIGINT"},
			{"end_snapshot", "BIGINT"},
		}},
		{"ducklake_partition_column", {
			{"partition_id", "BIGINT"},
			{"table_id", "BIGINT"},
			{"partition_key_index", "BIGINT"},
			{"column_id", "BIGINT"},
			{"transform", "VARCHAR"},
		}},
		{"ducklake_file_partition_value", {
			{"data_file_id", "BIGINT"},
			{"table_id", "BIGINT"},
			{"partition_key_index", "BIGINT"},
			{"partition_value", "VARCHAR"},
		}},
		{"ducklake_files_scheduled_for_deletion", {
			{"data_file_id", "BIGINT"},
			{"path", "VARCHAR"},
			{"path_is_relative", "BOOLEAN"},
			{"schedule_start", "TIMESTAMPTZ"},
		}},
		{"ducklake_inlined_data_tables", {
			{"table_id", "BIGINT"},
			{"table_name", "VARCHAR"},
			{"schema_version", "BIGINT"},
		}},
		{"ducklake_column_mapping", {
			{"mapping_id", "BIGINT"},
			{"table_id", "BIGINT"},
			{"type", "VARCHAR"},
		}},
		{"ducklake_name_mapping", {
			{"mapping_id", "BIGINT"},
			{"column_id", "BIGINT"},
			{"source_name", "VARCHAR"},
			{"target_field_id", "BIGINT"},
			{"parent_column", "BIGINT"},
			{"is_partition", "BOOLEAN"},
		}},
		{"ducklake_schema_versions", {
			{"begin_snapshot", "BIGINT"},
			{"schema_version", "BIGINT"},
			{"table_id", "BIGINT"},
		}},
		{"ducklake_macro", {
			{"schema_id", "BIGINT"},
			{"macro_id", "BIGINT"},
			{"macro_name", "VARCHAR"},
			{"begin_snapshot", "BIGINT"},
			{"end_snapshot", "BIGINT"},
		}},
		{"ducklake_macro_impl", {
			{"macro_id", "BIGINT"},
			{"impl_id", "BIGINT"},
			{"dialect", "VARCHAR"},
			{"sql", "VARCHAR"},
			{"type", "VARCHAR"},
		}},
		{"ducklake_macro_parameters", {
			{"macro_id", "BIGINT"},
			{"impl_id", "BIGINT"},
			{"column_id", "BIGINT"},
			{"parameter_name", "VARCHAR"},
			{"parameter_type", "VARCHAR"},
			{"default_value", "VARCHAR"},
			{"default_value_type", "VARCHAR"},
		}},
		{"ducklake_sort_info", {
			{"sort_id", "BIGINT"},
			{"table_id", "BIGINT"},
			{"begin_snapshot", "BIGINT"},
			{"end_snapshot", "BIGINT"},
		}},
		{"ducklake_sort_expression", {
			{"sort_id", "BIGINT"},
			{"table_id", "BIGINT"},
			{"sort_key_index", "BIGINT"},
			{"expression", "VARCHAR"},
			{"dialect", "VARCHAR"},
			{"sort_direction", "VARCHAR"},
			{"null_order", "VARCHAR"},
		}},
		{"ducklake_view_column_tag", {
			{"view_id", "BIGINT"},
			{"column_name", "VARCHAR"},
			{"begin_snapshot", "BIGINT"},
			{"end_snapshot", "BIGINT"},
			{"key", "VARCHAR"},
			{"value", "VARCHAR"},
		}, DuckLakeVersion::V1_1},
		{"ducklake_snapshot_tag", {
			{"tag_name", "VARCHAR"},
			{"snapshot_id", "BIGINT"},
			{"begin_snapshot", "BIGINT"},
			{"end_snapshot", "BIGINT"},
			{"expires_at", "TIMESTAMPTZ"},
		}, DuckLakeVersion::V2_0_DEV1},
	};
	// clang-format on
	return tables;
}

static const DuckLakeMetadataTable &GetMetadataTable(const string &table_name) {
	for (auto &table : MetadataTables()) {
		if (table_name == table.name) {
			return table;
		}
	}
	throw InternalException("Unknown DuckLake metadata table \"%s\"", table_name);
}

string DuckLakeMetadataSchema::CreateTableStatements(DuckLakeVersion version) {
	vector<string> statements;
	for (auto &table : MetadataTables()) {
		if (table.added_in <= version) {
			statements.push_back(CreateTableStatement(table.name, version));
		}
	}
	return "\n" + StringUtil::Join(statements, "\n") + "\n";
}

string DuckLakeMetadataSchema::CreateTableStatement(const string &table_name, DuckLakeVersion version) {
	vector<string> column_definitions;
	for (auto &column : GetMetadataTable(table_name).columns) {
		if (column.added_in <= version) {
			column_definitions.push_back(string(column.name) + " " + column.definition);
		}
	}
	return StringUtil::Format("CREATE TABLE {METADATA_CATALOG}.%s(%s);", table_name,
	                          StringUtil::Join(column_definitions, ", "));
}

vector<string> DuckLakeMetadataSchema::ColumnNames(const string &table_name, DuckLakeVersion version) {
	vector<string> names;
	for (auto &column : GetMetadataTable(table_name).columns) {
		if (column.added_in <= version) {
			names.push_back(column.name);
		}
	}
	return names;
}

vector<string> DuckLakeMetadataSchema::ColumnNames(const string &table_name) {
	auto &table = GetMetadataTable(table_name);
	auto names = ColumnNames(table_name, table.added_in);
	if (names.size() != table.columns.size()) {
		throw InternalException("The columns of DuckLake metadata table \"%s\" depend on the DuckLake version",
		                        table_name);
	}
	return names;
}

vector<pair<string, string>> DuckLakeMetadataSchema::ColumnTypes(const string &table_name) {
	vector<pair<string, string>> result;
	for (auto &column : GetMetadataTable(table_name).columns) {
		// the type is the first word of the definition, before constraints such as PRIMARY KEY
		auto definition = string(column.definition);
		result.emplace_back(column.name, definition.substr(0, definition.find(' ')));
	}
	return result;
}

bool DuckLakeMetadataSchema::HasTable(const string &table_name, DuckLakeVersion version) {
	return GetMetadataTable(table_name).added_in <= version;
}

} // namespace duckdb

//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/ducklake_metadata_schema.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "common/ducklake_version.hpp"

namespace duckdb {

//! A column of a DuckLake metadata table
struct DuckLakeMetadataColumn {
	DuckLakeMetadataColumn(const char *name, const char *definition, DuckLakeVersion added_in = DuckLakeVersion::V1_0)
	    : name(name), definition(definition), added_in(added_in) {
	}

	const char *name;
	//! The type of the column with its constraints, e.g. BIGINT PRIMARY KEY
	const char *definition;
	DuckLakeVersion added_in;
};

//! A DuckLake metadata table
struct DuckLakeMetadataTable {
	DuckLakeMetadataTable(const char *name, vector<DuckLakeMetadataColumn> columns,
	                      DuckLakeVersion added_in = DuckLakeVersion::V1_0)
	    : name(name), columns(std::move(columns)), added_in(added_in) {
	}

	const char *name;
	vector<DuckLakeMetadataColumn> columns;
	DuckLakeVersion added_in;
};

//! The metadata tables of DuckLake with their columns in every version
class DuckLakeMetadataSchema {
public:
	//! The CREATE TABLE statements of all metadata tables of a version
	static string CreateTableStatements(DuckLakeVersion version);
	//! The CREATE TABLE statement of a metadata table in a version
	static string CreateTableStatement(const string &table_name, DuckLakeVersion version);
	//! The columns of a metadata table in a version, in the order of its definition
	static vector<string> ColumnNames(const string &table_name, DuckLakeVersion version);
	//! The columns of a metadata table whose columns are the same in every version that has it
	static vector<string> ColumnNames(const string &table_name);
};

} // namespace duckdb

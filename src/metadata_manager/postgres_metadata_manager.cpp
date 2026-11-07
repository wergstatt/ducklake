#include "metadata_manager/postgres_metadata_manager.hpp"
#include "common/ducklake_util.hpp"
#include "duckdb/main/database.hpp"
#include "storage/ducklake_catalog.hpp"
#include "storage/ducklake_transaction.hpp"
#include "storage/ducklake_metadata_info.hpp"
#include "storage/ducklake_table_entry.hpp"
#include "common/ducklake_types.hpp"
#include "duckdb/common/type_visitor.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"

namespace duckdb {

static bool HasFourDigitDatePrefix(const string &value) {
	return value.size() >= 10 && StringUtil::CharacterIsDigit(value[0]) && StringUtil::CharacterIsDigit(value[1]) &&
	       StringUtil::CharacterIsDigit(value[2]) && StringUtil::CharacterIsDigit(value[3]) && value[4] == '-' &&
	       StringUtil::CharacterIsDigit(value[5]) && StringUtil::CharacterIsDigit(value[6]) && value[7] == '-' &&
	       StringUtil::CharacterIsDigit(value[8]) && StringUtil::CharacterIsDigit(value[9]);
}

static bool IsInfinityLiteral(const string &value) {
	return value == "infinity" || value == "-infinity";
}

static bool IsPostgresTemporalStatsType(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::DATE:
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_TZ:
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::TIMESTAMP_TZ_NS:
	case LogicalTypeId::TIME:
	case LogicalTypeId::TIME_NS:
		return true;
	default:
		return false;
	}
}

static bool IsNanosecondTemporalType(const LogicalType &type) {
	return type.id() == LogicalTypeId::TIMESTAMP_NS || type.id() == LogicalTypeId::TIMESTAMP_TZ_NS ||
	       type.id() == LogicalTypeId::TIME_NS;
}

static bool IsTimeOfDayType(const LogicalType &type) {
	return type.id() == LogicalTypeId::TIME || type.id() == LogicalTypeId::TIME_NS;
}

static string GetPostgresStatsType(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
		return "BOOLEAN";
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
		return "SMALLINT";
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
		return "INTEGER";
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::UINTEGER:
		return "BIGINT";
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UHUGEINT:
		return "NUMERIC";
	case LogicalTypeId::FLOAT:
		return "REAL";
	case LogicalTypeId::DOUBLE:
		return "DOUBLE PRECISION";
	case LogicalTypeId::DATE:
		return "DATE";
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::TIME:
	case LogicalTypeId::TIME_NS:
		return "TIMESTAMP";
	case LogicalTypeId::TIMESTAMP_TZ:
	case LogicalTypeId::TIMESTAMP_TZ_NS:
		return "TIMESTAMPTZ";
	case LogicalTypeId::DECIMAL:
		return type.ToString();
	default:
		// LCOV_EXCL_START
		return string();
		// LCOV_EXCL_STOP
	}
}

static bool CanCastPostgresStatsForValueComparison(const LogicalType &type) {
	return type.IsNumeric() || type.id() == LogicalTypeId::BOOLEAN || IsPostgresTemporalStatsType(type);
}

static bool CanCastPostgresTemporalValue(const Value &value, const LogicalType &type) {
	auto string_value = value.ToString();
	if (IsInfinityLiteral(string_value)) {
		return true;
	}
	if (!HasFourDigitDatePrefix(string_value)) {
		return false;
	}
	return type.id() != LogicalTypeId::DATE || string_value.size() == 10;
}

string PostgresMetadataManager::CastValueToTarget(const Value &value, const LogicalType &type) {
	if (value.IsNull() || value.ToString().find('\0') != string::npos || type.id() == LogicalTypeId::BLOB) {
		return string();
	}
	if (RequiresValueComparison(type) && (!CanCastPostgresStatsForValueComparison(type) || !ValueIsFinite(value))) {
		return string();
	}
	if (type.id() == LogicalTypeId::UUID) {
		// canonical text sorts like the bytes
		return DuckLakeUtil::WithBinaryCollation(SQLString::ToString(value.ToString()));
	}
	if (!RequiresValueComparison(type) && type.id() != LogicalTypeId::VARCHAR) {
		return string();
	}
	if (IsTimeOfDayType(type)) {
		if (value.type().id() != type.id()) {
			return string();
		}
		// Anchor time to the epoch date so timestamp bounds can widen without wrapping.
		auto micros = value.GetValueUnsafe<int64_t>();
		if (type.id() == LogicalTypeId::TIME_NS) {
			micros /= Interval::NANOS_PER_MICRO;
		}
		return CastValueToTarget(Value::TIMESTAMP(timestamp_t(micros)), LogicalType::TIMESTAMP);
	}
	if (IsNanosecondTemporalType(type) && value.type().id() == type.id()) {
		// postgres keeps microseconds, so floor the constant and widen the stats bounds to match
		auto nanos = value.GetValueUnsafe<int64_t>();
		if (timestamp_t(nanos).IsFinite()) {
			auto micros = nanos / Interval::NANOS_PER_MICRO - (nanos % Interval::NANOS_PER_MICRO < 0 ? 1 : 0);
			auto floored = type.id() == LogicalTypeId::TIMESTAMP_NS ? Value::TIMESTAMP(timestamp_t(micros))
			                                                        : Value::TIMESTAMPTZ(timestamp_tz_t(micros));
			return CastValueToTarget(floored, floored.type());
		}
	}
	if (IsPostgresTemporalStatsType(type) && !CanCastPostgresTemporalValue(value, type)) {
		return string();
	}
	if (type.id() == LogicalTypeId::FLOAT) {
		// a bare number is NUMERIC, which compares with REAL bounds as DOUBLE PRECISION
		return "CAST(" + value.ToString() + " AS " + GetPostgresStatsType(type) + ")";
	}
	if (type.IsNumeric()) {
		return value.ToString();
	}
	auto literal = SQLString::ToString(value.ToString());
	if (type.id() == LogicalTypeId::VARCHAR) {
		return DuckLakeUtil::WithBinaryCollation(literal);
	}
	if (IsPostgresTemporalStatsType(type)) {
		return literal + "::" + GetPostgresStatsType(type);
	}
	if (type.id() == LogicalTypeId::BOOLEAN) {
		return literal + "::BOOLEAN";
	}
	return string();
}

static string PostgresSafeTemporalStatsCast(const string &stats, const LogicalType &type) {
	string regex;
	auto fraction = IsNanosecondTemporalType(type) ? "9" : "6";
	auto value = stats;
	if (IsNanosecondTemporalType(type)) {
		value = StringUtil::Format("regexp_replace(%s, '(\\.[0-9]{6})[0-9]+', '\\1')", stats);
	}
	if (IsTimeOfDayType(type)) {
		regex = StringUtil::Format("'^(([01][0-9]|2[0-3]):[0-5][0-9]:[0-5][0-9](\\.[0-9]{1,%s})?|24:00:00(\\.0+)?)$'",
		                           fraction);
		return StringUtil::Format("(CASE WHEN %s ~ %s THEN DATE '1970-01-01' + %s::TIME END)", stats, regex, value);
	}
	if (type.id() == LogicalTypeId::DATE) {
		regex = "'^[0-9]{4}-(0[1-9]|1[0-2])-([0][1-9]|[12][0-9]|3[01])$'";
	} else if (type.id() == LogicalTypeId::TIMESTAMP_TZ || type.id() == LogicalTypeId::TIMESTAMP_TZ_NS) {
		regex = StringUtil::Format("'^[0-9]{4}-(0[1-9]|1[0-2])-([0][1-9]|[12][0-9]|3[01]) "
		                           "([01][0-9]|2[0-3]):[0-5][0-9]:[0-5][0-9](\\.[0-9]{1,%s})?"
		                           "(Z|[+-](0[0-9]|1[0-5])(:[0-5][0-9])?)$'",
		                           fraction);
	} else {
		regex = StringUtil::Format("'^[0-9]{4}-(0[1-9]|1[0-2])-([0][1-9]|[12][0-9]|3[01])"
		                           "( ([01][0-9]|2[0-3]):[0-5][0-9]:[0-5][0-9](\\.[0-9]{1,%s})?)?$'",
		                           fraction);
	}

	auto year = StringUtil::Format("substring(%s FROM 1 FOR 4)::INTEGER", stats);
	auto month = StringUtil::Format("substring(%s FROM 6 FOR 2)::INTEGER", stats);
	auto day = StringUtil::Format("substring(%s FROM 9 FOR 2)::INTEGER", stats);
	auto max_day = StringUtil::Format(
	    "(CASE WHEN %s = 2 THEN CASE WHEN mod(%s, 4) = 0 AND (mod(%s, 100) <> 0 OR mod(%s, 400) = 0) "
	    "THEN 29 ELSE 28 END WHEN %s IN (4, 6, 9, 11) THEN 30 ELSE 31 END)",
	    month, year, year, year, month);
	auto valid_date = StringUtil::Format("%s > 0 AND %s <= %s", year, day, max_day);
	return StringUtil::Format(
	    "(CASE WHEN %s ~ %s THEN CASE WHEN %s THEN %s::%s END WHEN %s IN ('infinity', '-infinity') THEN %s::%s END)",
	    stats, regex, valid_date, value, GetPostgresStatsType(type), stats, stats, GetPostgresStatsType(type));
}

string PostgresMetadataManager::CastStatsToTarget(const string &stats, const LogicalType &type,
                                                  StatsCastType cast_type) {
	if (IsPostgresTemporalStatsType(type)) {
		auto cast = PostgresSafeTemporalStatsCast(stats, type);
		if (cast_type == StatsCastType::ORDERING) {
			return cast;
		}
		if (IsNanosecondTemporalType(type)) {
			// widen truncated bounds by a microsecond
			cast = StringUtil::Format("(%s %s INTERVAL '1 microsecond')", cast,
			                          cast_type == StatsCastType::MIN ? "-" : "+");
		}
		return BoundOrInfinity(cast, GetPostgresStatsType(type), cast_type);
	}
	if (CanCastPostgresStatsForValueComparison(type)) {
		return stats + "::" + GetPostgresStatsType(type);
	}
	if (type.id() == LogicalTypeId::VARCHAR || type.id() == LogicalTypeId::UUID) {
		return DuckLakeUtil::WithBinaryCollation(stats);
	}
	return string();
}

PostgresMetadataManager::PostgresMetadataManager(DuckLakeTransaction &transaction)
    : DuckLakeMetadataManager(transaction) {
}

bool PostgresMetadataManager::ShowsConcurrentCommits() const {
	// READ COMMITTED metadata reads see the commits of other transactions
	auto &parameters = transaction.GetCatalog().MetadataParameters();
	auto isolation_level = parameters.find("isolation_level");
	return isolation_level != parameters.end() &&
	       StringUtil::CIEquals(isolation_level->second.ToString(), "READ COMMITTED");
}

//! The function of postgres_scanner with this name, if it has one
static optional_ptr<CatalogEntry> ScannerFunction(ClientContext &context, const string &name) {
	auto transaction = CatalogTransaction::GetSystemCatalogTransaction(context);
	auto &schema = Catalog::GetSystemCatalog(context).GetSchema(transaction, Identifier::DefaultSchema());
	return schema.GetEntry(transaction, CatalogType::TABLE_FUNCTION_ENTRY, Identifier(name));
}

//! The function of postgres_scanner that runs statements with their parameters in one round trip, if it has one
static optional_ptr<CatalogEntry> PipelineFunction(ClientContext &context) {
	return ScannerFunction(context, "postgres_execute_pipeline");
}

bool PostgresMetadataManager::InsertsInlinedRowsAfterBatch(idx_t row_count, idx_t column_count) const {
	// a pipeline sends the rows as parameters, their row ids, snapshots and cells, while they fit in one statement
	if (PipelineFunction(*transaction.GetConnection().context)) {
		return row_count * (column_count + 2) > MAX_PARAMETERS_PER_STATEMENT;
	}
	// the copy of a separate insert costs more round trips than parsing the VALUES text of a few rows
	static constexpr idx_t MAX_INLINED_ROWS_IN_BATCH = 100;
	return row_count > MAX_INLINED_ROWS_IN_BATCH;
}

bool PostgresMetadataManager::TypeIsNativelySupported(const LogicalType &type) {
	switch (type.id()) {
	// Unnamed composite types are not supported.
	case LogicalTypeId::STRUCT:
	case LogicalTypeId::MAP:
	case LogicalTypeId::LIST:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UHUGEINT:
	// Postgres timestamp/date ranges are narrower than DuckDB's, and its TIME keeps only microseconds
	case LogicalTypeId::TIME_NS:
	case LogicalTypeId::DATE:
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_TZ:
	case LogicalTypeId::TIMESTAMP_TZ_NS:
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_NS:
	// Postgres bytea input format differs from DuckDB's blob text format
	case LogicalTypeId::BLOB:
	// Postgres cannot store null bytes in VARCHAR/TEXT columns
	case LogicalTypeId::VARCHAR:
	case LogicalTypeId::VARIANT:
	// If we knew that the Postgres installation has PostGIS installed, we could support GEOMETRY in the future.
	case LogicalTypeId::GEOMETRY:
		return false;
	default:
		return true;
	}
}

static bool ContainsNulByte(Vector &vector, idx_t offset, idx_t count) {
	vector.Flatten();
	auto &validity = FlatVector::Validity(vector);
	switch (vector.GetType().id()) {
	case LogicalTypeId::VARCHAR: {
		auto strings = FlatVector::GetData<string_t>(vector);
		for (idx_t i = offset; i < offset + count; i++) {
			if (validity.RowIsValid(i) && memchr(strings[i].GetData(), '\0', strings[i].GetSize())) {
				return true;
			}
		}
		return false;
	}
	case LogicalTypeId::STRUCT:
		for (auto &child : StructVector::GetEntries(vector)) {
			if (ContainsNulByte(child, offset, count)) {
				return true;
			}
		}
		return false;
	case LogicalTypeId::LIST:
	case LogicalTypeId::MAP: {
		auto entries = FlatVector::GetData<list_entry_t>(vector);
		auto &child = ListVector::GetChildMutable(vector);
		for (idx_t i = offset; i < offset + count; i++) {
			if (validity.RowIsValid(i) && ContainsNulByte(child, entries[i].offset, entries[i].length)) {
				return true;
			}
		}
		return false;
	}
	default:
		return false;
	}
}

bool PostgresMetadataManager::CanInlineValues(DataChunk &chunk) {
	// nested values are stored as text, which cannot hold a NUL byte
	for (auto &column : chunk.data) {
		auto &type = column.GetType();
		if (DuckLakeTypes::IsNested(type) && TypeVisitor::Contains(type, LogicalTypeId::VARCHAR) &&
		    ContainsNulByte(column, 0, chunk.size())) {
			return false;
		}
	}
	return true;
}

string PostgresMetadataManager::GetColumnTypeInternal(const LogicalType &column_type) {
	switch (column_type.id()) {
	case LogicalTypeId::DOUBLE:
		return "DOUBLE PRECISION";
	case LogicalTypeId::TINYINT:
		return "SMALLINT";
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::SQLNULL:
		return "INTEGER";
	case LogicalTypeId::UINTEGER:
		return "BIGINT";
	case LogicalTypeId::FLOAT:
		return "REAL";
	case LogicalTypeId::BLOB:
	case LogicalTypeId::VARCHAR:
		return "BYTEA";
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UHUGEINT:
	case LogicalTypeId::TIME_NS:
	case LogicalTypeId::DATE:
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_TZ:
	case LogicalTypeId::TIMESTAMP_TZ_NS:
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_NS:
		return "VARCHAR";
	default:
		return column_type.ToString();
	}
}

optional_idx PostgresMetadataManager::BatchServerErrorStart(const string &message) const {
	// the batch and the server error can contain any text, so the batch is found by its size
	// the scanner names a failed statement "query", or in a pipeline by its position, and then quotes it
	const string failed_batch = "Failed to execute ";
	auto failed_start = message.find(failed_batch);
	if (failed_start == string::npos) {
		return optional_idx();
	}
	auto batch_start = message.find('"', failed_start + failed_batch.size());
	if (batch_start == string::npos) {
		return optional_idx();
	}
	batch_start++;
	for (auto &query : executed_queries) {
		auto batch_end = batch_start + query.size();
		if (batch_end + 3 <= message.size() && message.compare(batch_start, query.size(), query) == 0 &&
		    message.compare(batch_end, 3, "\": ") == 0) {
			return batch_end;
		}
	}
	return optional_idx();
}

string PostgresMetadataManager::CommitErrorWithoutQuery(const string &message) const {
	auto server_error_start = BatchServerErrorStart(message);
	return server_error_start.IsValid() ? message.substr(server_error_start.GetIndex()) : message;
}

bool PostgresMetadataManager::IsRetryableCommitError(const string &message) const {
	auto batch_server_error_start = BatchServerErrorStart(message);
	// for another failed query, the server error is guessed to follow the last quote and colon
	auto server_error_start =
	    batch_server_error_start.IsValid() ? batch_server_error_start.GetIndex() : message.rfind("\": ");
	auto server_error = server_error_start == string::npos ? message : message.substr(server_error_start);
	// constraint names are not translated, so a conflict on a metadata primary key is found in any language
	for (auto end = server_error.find("_pkey"); end != string::npos; end = server_error.find("_pkey", end + 1)) {
		auto start = end;
		while (start > 0 &&
		       (StringUtil::CharacterIsAlphaNumeric(server_error[start - 1]) || server_error[start - 1] == '_')) {
			start--;
		}
		if (StringUtil::StartsWith(server_error.substr(start, end - start), "ducklake_")) {
			return true;
		}
	}
	return false;
}

string PostgresMetadataManager::InlinedDeletionTablesQuery() const {
	// one listing serves every table of the snapshot, a name the LIKE wildcard _ matches too is never looked up
	// a kept result of a pipeline has no binary reader for the type name
	return StringUtil::Format("SELECT CAST(tablename AS TEXT) FROM pg_catalog.pg_tables WHERE schemaname = %s AND "
	                          "tablename LIKE 'ducklake_inlined_delete_%%'",
	                          SQLString::ToString(transaction.GetCatalog().MetadataSchemaName().GetIdentifierName()));
}

bool PostgresMetadataManager::InlinedDeletionTableExists(const Identifier &table_name) {
	auto query =
	    StringUtil::Format("SELECT * FROM postgres_query({METADATA_CATALOG_NAME_LITERAL}, %s, use_transaction = true)",
	                       SQLString::ToString(InlinedDeletionTablesQuery()));
	return InlinedDeletionTableListed(table_name, std::move(query));
}

void PostgresMetadataManager::MigrateInlinedDataTypes() {
	auto columns = DuckLakeMetadataManager::Query(GetInlinedTableColumnsSql());
	columns->ThrowIfError("Failed to read the columns of inlined-data tables while migrating: ");
	map<Identifier, identifier_map_t<string>> inlined_tables;
	for (auto &row : *columns) {
		inlined_tables[row.GetValue<Identifier>(0)][row.GetValue<Identifier>(1)] = row.GetValue<string>(2);
	}
	for (auto &inlined_table : inlined_tables) {
		auto &table_name = inlined_table.first;
		auto probe = DuckLakeMetadataManager::Query(
		    StringUtil::Format("SELECT * FROM {METADATA_CATALOG}.%s LIMIT 0", table_name));
		if (probe->HasError() || probe->GetNames().size() < 3) {
			continue;
		}
		// the metadata columns are always the first three columns, user columns follow
		auto &names = probe->GetNames();
		DuckLakeInlinedColNames col_names(false);
		col_names.row_id = names[0];
		col_names.begin_snapshot = names[1];
		col_names.end_snapshot = names[2];
		vector<string> select_list;
		for (idx_t i = 0; i < 3; i++) {
			select_list.push_back(SQLQuotedIdentifier::ToString(names[i]));
		}
		string column_defs;
		bool rewrite = false;
		for (idx_t i = 3; i < names.size(); i++) {
			auto column_type = inlined_table.second.find(names[i]);
			if (column_type == inlined_table.second.end()) {
				rewrite = false;
				break;
			}
			auto type = DuckLakeTypes::FromString(column_type->second);
			auto native_type = type.HasAlias() ? LogicalType(type.id()) : type;
			// DuckLake 0.3 stored values with the native type of their DuckLake type, other columns are kept
			auto &stored_type = probe->GetTypes()[i];
			string storage_type_name;
			bool convert = false;
			// nested columns are stored as VARCHAR in every version
			if (!DuckLakeTypes::IsNested(type)) {
				DuckLakeColumnInfo column;
				column.type = column_type->second;
				storage_type_name = GetColumnType(column);
				auto storage_type = UnboundType::TryParseAndDefaultBind(storage_type_name);
				convert = stored_type != storage_type && stored_type == native_type;
			}
			auto column_name = SQLQuotedIdentifier::ToString(names[i]);
			column_defs += StringUtil::Format("%s%s %s", column_defs.empty() ? "" : ", ", column_name,
			                                  convert ? storage_type_name : stored_type.ToString());
			select_list.push_back(convert ? DuckLakeUtil::InlinedStorageExpression(*this, column_name, type)
			                              : column_name);
			rewrite = rewrite || convert;
		}
		if (!rewrite) {
			continue;
		}
		auto migrated_name = Identifier(table_name + "_migrated");
		auto migrate_query = InlinedTableDdlSql(migrated_name, column_defs, col_names);
		migrate_query += StringUtil::Format("INSERT INTO {METADATA_CATALOG}.%s SELECT %s FROM {METADATA_CATALOG}.%s;",
		                                    migrated_name, StringUtil::Join(select_list, ", "), table_name);
		migrate_query += StringUtil::Format("DROP TABLE {METADATA_CATALOG}.%s;", table_name);
		migrate_query +=
		    StringUtil::Format("ALTER TABLE {METADATA_CATALOG}.%s RENAME TO %s;", migrated_name, table_name);
		auto result = DuckLakeMetadataManager::Execute(migrate_query);
		result->ThrowIfError(
		    StringUtil::Format("Failed to migrate the column types of inlined-data table %s: ", table_name));
	}
}

string PostgresMetadataManager::ConfigOptionLockSql() const {
	// every such commit updates the version row, so of two concurrent ones the later fails or waits for the other
	return "UPDATE {METADATA_CATALOG}.ducklake_metadata SET value = value WHERE key = 'version' AND scope IS NULL;\n";
}

static void CheckNoNulByte(const string &text) {
	if (text.find('\0') != string::npos) {
		throw InvalidInputException("A DuckLake catalog on PostgreSQL cannot store text that contains a NUL byte, "
		                            "such as a partition value, a name or a commit message");
	}
}

unique_ptr<QueryResult> PostgresMetadataManager::Execute(DuckLakeSnapshot snapshot, string &query) {
	SubstituteTransactionPlaceholders(snapshot, query);
	auto &ducklake_catalog = transaction.GetCatalog();
	SubstituteCatalogPlaceholders(query, SQLQuotedIdentifier::ToString(ducklake_catalog.MetadataSchemaName()));
	CheckNoNulByte(query);
	auto catalog_literal = SQLString::ToString(ducklake_catalog.MetadataDatabaseName().GetIdentifierName());
	// casts of float bounds to text keep their exact value
	query = "SET LOCAL extra_float_digits = 3;" + query;
	// the scanner drops trailing semicolons before it repeats the query in an error
	StringUtil::RTrim(query, "; \t\n\v\f\r");
	executed_queries = {query};
	return transaction.ExecuteRaw(
	    StringUtil::Format("CALL postgres_execute(%s, %s, prepare=FALSE)", catalog_literal, SQLString(query)));
}

unique_ptr<QueryResult> PostgresMetadataManager::Execute(DuckLakeSnapshot snapshot, const DuckLakeBatch &batch) {
	if (!PipelineFunction(*transaction.GetConnection().context)) {
		return DuckLakeMetadataManager::Execute(snapshot, batch);
	}
	// casts of float bounds to text keep their exact value
	DuckLakeBatch pipeline("SET LOCAL extra_float_digits = 3");
	pipeline += batch;
	return ExecutePipeline(snapshot, pipeline.SplitStatements());
}

vector<unique_ptr<QueryResult>> PostgresMetadataManager::Query(DuckLakeSnapshot snapshot,
                                                               const DuckLakeReadBatch &batch) {
	if (!ScannerFunction(*transaction.GetConnection().context, "postgres_pipeline_result")) {
		return DuckLakeMetadataManager::Query(snapshot, batch);
	}
	vector<unique_ptr<QueryResult>> results;
	for (auto &scan : KeepResults(snapshot, batch)) {
		results.push_back(transaction.ExecuteRaw(scan));
	}
	return results;
}

vector<string> PostgresMetadataManager::KeepResults(DuckLakeSnapshot snapshot, const DuckLakeReadBatch &batch) {
	auto pipeline = ExecutePipeline(snapshot, batch.GetReads(), true);
	pipeline->ThrowIfError("Failed to read the DuckLake metadata: ");
	auto catalog_literal = SQLString::ToString(transaction.GetCatalog().MetadataDatabaseName().GetIdentifierName());
	vector<string> scans;
	for (auto &row : *pipeline) {
		scans.push_back(
		    StringUtil::Format("FROM postgres_pipeline_result(%s, %d)", catalog_literal, row.GetValue<int64_t>(1)));
	}
	return scans;
}

//! Adds a read of every metadata table of the catalog with its filter, and returns the tables in the order of the reads
static vector<string> AddCatalogReads(DuckLakeReadBatch &reads, bool supports_v1_1_metadata,
                                      const DuckLakeStatement &versioned_filter, const DuckLakeStatement &part_filter) {
	// the tables of the catalog are read as they are at a snapshot, the tables of their parts whole
	vector<string> tables = {"ducklake_schema", "ducklake_table",          "ducklake_tag",
	                         "ducklake_column", "ducklake_column_tag",     "ducklake_view",
	                         "ducklake_macro",  "ducklake_partition_info", "ducklake_sort_info"};
	if (supports_v1_1_metadata) {
		tables.emplace_back("ducklake_view_column_tag");
	}
	for (auto &table : tables) {
		reads.Add(
		    {"SELECT * FROM {METADATA_CATALOG}." + table + " WHERE " + versioned_filter.sql, versioned_filter.params});
	}
	for (string table : {"ducklake_inlined_data_tables", "ducklake_macro_impl", "ducklake_macro_parameters",
	                     "ducklake_partition_column", "ducklake_sort_expression"}) {
		reads.Add(
		    {"SELECT * FROM {METADATA_CATALOG}." + table + (part_filter.sql.empty() ? "" : " WHERE ") + part_filter.sql,
		     part_filter.params});
		tables.push_back(std::move(table));
	}
	return tables;
}

DuckLakeCatalogInfo PostgresMetadataManager::GetCatalogForSnapshot(DuckLakeSnapshot snapshot) {
	{
		lock_guard<mutex> guard(read_ahead_lock);
		if (read_ahead_catalog && read_ahead_catalog->first == snapshot.schema_version) {
			auto catalog = std::move(read_ahead_catalog->second);
			read_ahead_catalog.reset();
			return catalog;
		}
	}
	if (!ScannerFunction(*transaction.GetConnection().context, "postgres_pipeline_result")) {
		return DuckLakeMetadataManager::GetCatalogForSnapshot(snapshot);
	}
	DuckLakeReadBatch reads;
	auto tables = AddCatalogReads(reads, transaction.GetCatalog().SupportsV1_1Metadata(),
	                              {"$1 >= begin_snapshot AND ($1 < end_snapshot OR end_snapshot IS NULL)",
	                               {Value::UBIGINT(snapshot.snapshot_id)}},
	                              {});
	return BuildCatalog(snapshot, tables, KeepResults(snapshot, reads));
}

DuckLakeCatalogInfo PostgresMetadataManager::BuildCatalog(DuckLakeSnapshot snapshot, const vector<string> &tables,
                                                          const vector<string> &scans) {
	auto &ducklake_catalog = transaction.GetCatalog();
	// the queries of the catalog run on the results of the reads, each in place of its table
	return BuildCatalogForSnapshot(
	    snapshot,
	    [&](DuckLakeSnapshot s, string query) {
		    vector<string> results;
		    for (idx_t i = 0; i < tables.size(); i++) {
			    if (StringUtil::Contains(query, "{METADATA_CATALOG}." + tables[i])) {
				    results.push_back(StringUtil::Format("%s AS (%s)", tables[i], scans[i]));
			    }
		    }
		    query = StringUtil::Replace(query, "{METADATA_CATALOG}.", "");
		    return DuckLakeMetadataManager::Query(s, "WITH " + StringUtil::Join(results, ", ") + query);
	    },
	    ducklake_catalog.DataPath(), ducklake_catalog.Separator(), ducklake_catalog.SupportsV1_1Metadata());
}

//! The snapshot id of the latest snapshot
static string LatestSnapshotId() {
	return "(SELECT MAX(snapshot_id) FROM {METADATA_CATALOG}.ducklake_snapshot)";
}

//! The condition of a read that the server skips when the latest snapshot has the schema version of the cache, $1
static string SchemaVersionChanged() {
	return "$1 <> (SELECT schema_version FROM {METADATA_CATALOG}.ducklake_snapshot WHERE snapshot_id = " +
	       LatestSnapshotId() + ")";
}

//! The condition of a read that the server skips when the latest snapshot is the snapshot of the cache, $1
static string SnapshotChanged() {
	return "$1 <> " + LatestSnapshotId();
}

//! The key of a cache as a parameter, with -1 for an empty cache, which is no snapshot id or schema version
static DuckLakeStatement CacheKey(const string &condition, optional_idx key) {
	return {condition, {Value::BIGINT(key.IsValid() ? NumericCast<int64_t>(key.GetIndex()) : -1)}};
}

//! Whether the cache key differs from the snapshot id or schema version, so that its read returned rows
static bool Differs(optional_idx key, idx_t value) {
	return !key.IsValid() || key.GetIndex() != value;
}

unique_ptr<DuckLakeSnapshot> PostgresMetadataManager::GetSnapshot() {
	if (!ScannerFunction(*transaction.GetConnection().context, "postgres_pipeline_result")) {
		return DuckLakeMetadataManager::GetSnapshot();
	}
	auto &catalog = transaction.GetCatalog();
	auto snapshot_query = LatestSnapshotQuery();
	StringUtil::RTrim(snapshot_query, ";");
	// READ COMMITTED reads see the commits before them, which a second read of the snapshot at the end shows
	bool read_committed = ShowsConcurrentCommits();
	// a commit that retries binds nothing, it needs no catalog of the latest snapshot
	bool read_catalog = !transaction.ChangesMade();
	for (idx_t attempt = 0;; attempt++) {
		// every read that a binder may need, which the server skips when the cache already holds its rows
		auto keys = catalog.GetCachedReadKeys();
		DuckLakeReadBatch reads;
		reads.Add({snapshot_query, {}});
		vector<string> catalog_tables;
		if (read_catalog) {
			catalog_tables =
			    AddCatalogReads(reads, catalog.SupportsV1_1Metadata(),
			                    CacheKey("end_snapshot IS NULL AND " + SchemaVersionChanged(), keys.schema_version),
			                    CacheKey(SchemaVersionChanged(), keys.schema_version));
		}
		auto schema_versions = reads.Add(CacheKey("SELECT table_id, schema_version, begin_snapshot FROM "
		                                          "{METADATA_CATALOG}.ducklake_schema_versions WHERE " +
		                                              SchemaVersionChanged(),
		                                          keys.table_schema_versions));
		auto deletion_tables = reads.Add(
		    CacheKey(InlinedDeletionTablesQuery() + " AND " + SnapshotChanged(), keys.inlined_deletion_tables));
		// the stats of every table, which a scan reads after each commit, ahead only for a catalog of few tables
		optional_idx stats;
		if (!keys.schema_version.IsValid() || keys.table_count <= MAX_TABLES_WITH_STATS_READ_AHEAD) {
			stats = reads.Add(CacheKey(GlobalTableStatsQuery(catalog.SupportsV1_1Metadata(), {}, SnapshotChanged()),
			                           keys.table_stats));
		}
		if (read_committed) {
			reads.Add({snapshot_query, {}});
		}
		auto scans = KeepResults(DuckLakeSnapshot(), reads);
		auto snapshot = ParseLatestSnapshot(*transaction.ExecuteRaw(scans[0]));
		if (read_committed) {
			auto last_snapshot = ParseSnapshot(*transaction.ExecuteRaw(scans.back()));
			if (!last_snapshot || last_snapshot->snapshot_id != snapshot->snapshot_id) {
				if (attempt == 0) {
					continue;
				}
				// the reads keep seeing commits, the caches read at the snapshot instead
				return snapshot;
			}
		}
		if (read_catalog && Differs(keys.schema_version, snapshot->schema_version)) {
			vector<string> catalog_scans(scans.begin() + 1, scans.begin() + 1 + catalog_tables.size());
			auto catalog_info = BuildCatalog(*snapshot, catalog_tables, catalog_scans);
			lock_guard<mutex> guard(read_ahead_lock);
			read_ahead_catalog =
			    make_uniq<pair<idx_t, DuckLakeCatalogInfo>>(snapshot->schema_version, std::move(catalog_info));
		}
		if (Differs(keys.table_schema_versions, snapshot->schema_version)) {
			auto result = transaction.ExecuteRaw(scans[schema_versions]);
			result->ThrowIfError("Failed to read the schema versions of the tables from DuckLake: ");
			map<TableIndex, map<idx_t, idx_t>> begin_snapshots;
			for (auto &row : *result) {
				begin_snapshots[TableIndex(row.GetValue<idx_t>(0))][row.GetValue<idx_t>(1)] = row.GetValue<idx_t>(2);
			}
			catalog.CacheTableSchemaVersions(snapshot->schema_version, begin_snapshots);
		}
		if (Differs(keys.inlined_deletion_tables, snapshot->snapshot_id)) {
			catalog.GetInlinedDeletionTables(
			    snapshot->snapshot_id, [&]() { return ListedTables(*transaction.ExecuteRaw(scans[deletion_tables])); });
		}
		if (stats.IsValid() && Differs(keys.table_stats, snapshot->snapshot_id)) {
			auto global_stats = ParseGlobalTableStats(*transaction.ExecuteRaw(scans[stats.GetIndex()]));
			lock_guard<mutex> guard(read_ahead_lock);
			read_ahead_stats =
			    make_uniq<pair<idx_t, vector<DuckLakeGlobalStatsInfo>>>(snapshot->snapshot_id, std::move(global_stats));
		}
		return snapshot;
	}
}

vector<DuckLakeGlobalStatsInfo> PostgresMetadataManager::GetGlobalTableStats(DuckLakeSnapshot snapshot,
                                                                             const vector<TableIndex> &table_ids) {
	vector<DuckLakeGlobalStatsInfo> global_stats;
	bool read_ahead = false;
	{
		lock_guard<mutex> guard(read_ahead_lock);
		if (read_ahead_stats && read_ahead_stats->first == snapshot.snapshot_id) {
			read_ahead = true;
			for (auto &table_stats : read_ahead_stats->second) {
				if (table_ids.empty() ||
				    std::find(table_ids.begin(), table_ids.end(), table_stats.table_id) != table_ids.end()) {
					global_stats.push_back(table_stats);
				}
			}
		}
	}
	if (!read_ahead) {
		return DuckLakeMetadataManager::GetGlobalTableStats(snapshot, table_ids);
	}
	FillMissingTableSizes(global_stats, [&](string query) { return Query(snapshot, query); });
	return global_stats;
}

unique_ptr<QueryResult> PostgresMetadataManager::ExecutePipeline(DuckLakeSnapshot snapshot,
                                                                 const vector<DuckLakeStatement> &batch,
                                                                 bool keep_results) {
	auto &ducklake_catalog = transaction.GetCatalog();
	auto schema_identifier = SQLQuotedIdentifier::ToString(ducklake_catalog.MetadataSchemaName());
	vector<Value> statements;
	vector<string> queries;
	for (auto &statement : batch) {
		auto query = statement.sql;
		SubstituteTransactionPlaceholders(snapshot, query);
		SubstituteCatalogPlaceholders(query, schema_identifier);
		CheckNoNulByte(query);
		vector<Value> params;
		for (auto &param : statement.params) {
			CheckNoNulByte(param.ToString());
			params.push_back(param.DefaultCastAs(LogicalType::VARCHAR));
		}
		statements.push_back(
		    Value::STRUCT({{"query", Value(query)}, {"params", Value::LIST(LogicalType::VARCHAR, std::move(params))}}));
		queries.push_back(std::move(query));
	}
	auto logged_query = StringUtil::Join(queries, ";\n");
	if (!keep_results) {
		// only the error of a commit is looked up in its queries, while reads can run on several threads at once
		executed_queries = std::move(queries);
	}
	auto catalog_literal = SQLString::ToString(ducklake_catalog.MetadataDatabaseName().GetIdentifierName());
	// the values stay out of the query, and so out of its logs
	return transaction.ExecuteRaw(StringUtil::Format("FROM postgres_execute_pipeline(%s, $1%s)", catalog_literal,
	                                                 keep_results ? ", keep_results := true" : ""),
	                              {Value::LIST(std::move(statements))}, logged_query);
}

unique_ptr<QueryResult> PostgresMetadataManager::ExecuteDropInTransaction(string &query) {
	// DROP TABLE waits for every open transaction that read the table and the readers queued behind it fail, so it
	// gives up instead and leaves the table to a later cleanup
	auto batch = "CALL postgres_execute({METADATA_CATALOG_NAME_LITERAL}, 'SET LOCAL lock_timeout = 1');\n" + query;
	return ExecuteInTransaction(batch);
}

void PostgresMetadataManager::ClearCache() {
	// clear the cache of the metadata catalog alone, a global clear breaks queries running in other Postgres catalogs
	auto &catalog = transaction.GetCatalog();
	if (catalog.ScopedCacheClear()) {
		auto catalog_literal = SQLString::ToString(catalog.MetadataDatabaseName().GetIdentifierName());
		auto result = transaction.ExecuteRaw(StringUtil::Format(
		    "SELECT * FROM postgres_query(%s, 'SELECT 1', clear_catalog_cache = true);", catalog_literal));
		if (!result->HasError()) {
			return;
		}
		// the error of a postgres_scanner from before the option, which can only clear every catalog
		if (!StringUtil::Contains(result->GetError(), "Invalid named parameter \"clear_catalog_cache\"")) {
			result->ThrowIfError("Failed to clear the PostgreSQL metadata cache: ");
		}
		catalog.SetScopedCacheClear(false);
	}
	auto result = transaction.ExecuteRaw("CALL postgres_clear_cache();");
	result->ThrowIfError("Failed to clear the PostgreSQL metadata cache: ");
}

static string PostgresQuery(const string &remote_query) {
	return StringUtil::Format("SELECT * FROM postgres_query({METADATA_CATALOG_NAME_LITERAL}, %s)",
	                          SQLString(remote_query));
}

string PostgresMetadataManager::GetLatestSnapshotQuery() const {
	auto remote_query = LatestSnapshotQuery();
	SubstituteCatalogPlaceholders(remote_query,
	                              SQLQuotedIdentifier::ToString(transaction.GetCatalog().MetadataSchemaName()));
	return PostgresQuery(remote_query);
}

string PostgresMetadataManager::GetSnapshotAndStatsAndChangesQuery(DuckLakeSnapshot snapshot,
                                                                   bool include_exactness) const {
	// one statement reads the latest snapshot and the table stats from the same state at any isolation level
	auto remote_query = SnapshotAndStatsAndChangesQuery(include_exactness);
	SubstituteCatalogPlaceholders(remote_query,
	                              SQLQuotedIdentifier::ToString(transaction.GetCatalog().MetadataSchemaName()));
	SubstituteSnapshotPlaceholders(snapshot, remote_query);
	return PostgresQuery(remote_query);
}

bool PostgresMetadataManager::MaterializeStatsCTEs() const {
	// Postgres misplans several inlined stats joins and rescans the stats for every surviving file
	return true;
}

string PostgresMetadataManager::GenerateFileListQuery(DuckLakeTableEntry &table, DuckLakeSnapshot snapshot,
                                                      const FilterPushdownInfo *filter_info,
                                                      const vector<DuckLakeFileListDynamicFilter> &dynamic_filters,
                                                      vector<idx_t> &runtime_filter_stats_columns,
                                                      FileListType file_list_type, const string &) {
	auto schema = SQLQuotedIdentifier::ToString(transaction.GetCatalog().MetadataSchemaName());
	auto remote_query = DuckLakeMetadataManager::GenerateFileListQuery(
	    table, snapshot, filter_info, dynamic_filters, runtime_filter_stats_columns, file_list_type, schema);
	// placeholders inside the literal are not replaced later
	SubstituteSnapshotPlaceholders(snapshot, remote_query);
	return PostgresQuery(remote_query);
}

} // namespace duckdb

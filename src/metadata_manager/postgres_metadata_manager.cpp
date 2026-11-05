#include "metadata_manager/postgres_metadata_manager.hpp"
#include "common/ducklake_util.hpp"
#include "duckdb/main/database.hpp"
#include "storage/ducklake_catalog.hpp"
#include "storage/ducklake_transaction.hpp"
#include "storage/ducklake_metadata_info.hpp"
#include "storage/ducklake_metadata_schema.hpp"
#include "storage/ducklake_table_entry.hpp"
#include "common/ducklake_types.hpp"
#include "duckdb/common/type_visitor.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"

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

bool PostgresMetadataManager::InsertsInlinedRowsAfterBatch(idx_t row_count) const {
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

bool PostgresMetadataManager::IsRetryableCommitError(const string &message) const {
	// the server error follows the failed query, which can contain any text
	auto server_error_start = message.rfind("\": ");
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

bool PostgresMetadataManager::InlinedDeletionTableExists(const Identifier &table_name) {
	auto &catalog = transaction.GetCatalog();
	auto remote_query =
	    StringUtil::Format("SELECT 1 FROM pg_catalog.pg_tables WHERE schemaname = %s AND tablename = %s LIMIT 1",
	                       SQLString::ToString(catalog.MetadataSchemaName().GetIdentifierName()),
	                       SQLString::ToString(table_name.GetIdentifierName()));
	auto query =
	    StringUtil::Format("SELECT 1 FROM postgres_query({METADATA_CATALOG_NAME_LITERAL}, %s, use_transaction = true)",
	                       SQLString::ToString(remote_query));
	auto result = DuckLakeMetadataManager::Query(query);
	result->ThrowIfError("Failed to probe for DuckLake inlined-deletion table: ");
	return result->Fetch() != nullptr;
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

//! A value as the text that PostgreSQL reads for a parameter of its type
static Value PostgresParameterText(const Value &value) {
	if (value.IsNull()) {
		return Value(LogicalType::VARCHAR);
	}
	switch (value.type().id()) {
	case LogicalTypeId::LIST: {
		vector<string> elements;
		for (auto &element : ListValue::GetChildren(value)) {
			if (element.IsNull()) {
				elements.push_back("NULL");
				continue;
			}
			auto text = StringValue::Get(PostgresParameterText(element));
			text = StringUtil::Replace(StringUtil::Replace(text, "\\", "\\\\"), "\"", "\\\"");
			elements.push_back("\"" + text + "\"");
		}
		return Value("{" + StringUtil::Join(elements, ",") + "}");
	}
	case LogicalTypeId::BLOB: {
		auto &blob = StringValue::Get(value);
		string hex = "\\x";
		for (auto c : blob) {
			hex += StringUtil::Format("%02x", static_cast<uint8_t>(c));
		}
		return Value(hex);
	}
	default:
		return Value(value.ToString());
	}
}

unique_ptr<QueryResult> PostgresMetadataManager::Execute(DuckLakeBatch &batch) {
	if (duckdb_dialect) {
		return DuckLakeMetadataManager::Execute(batch);
	}
	return ExecutePipeline(batch, false);
}

unique_ptr<QueryResult> PostgresMetadataManager::ExecuteAndCommit(DuckLakeSnapshot snapshot, DuckLakeBatch &batch) {
	if (duckdb_dialect) {
		return DuckLakeMetadataManager::ExecuteAndCommit(snapshot, batch);
	}
	batch.TransformSql([&](string &sql) { SubstituteTransactionPlaceholders(snapshot, sql); });
	return ExecutePipeline(batch, true);
}

unique_ptr<QueryResult> PostgresMetadataManager::ExecutePipeline(DuckLakeBatch &batch, bool commit) {
	auto &ducklake_catalog = transaction.GetCatalog();
	auto schema_identifier = SQLQuotedIdentifier::ToString(ducklake_catalog.MetadataSchemaName());
	// casts of float bounds to text keep their exact value
	vector<Value> statements {Value::STRUCT(
	    {{"query", Value("SET LOCAL extra_float_digits = 3")}, {"params", Value::LIST(LogicalType::VARCHAR, {})}})};
	string reported_query;
	for (auto &statement : batch.SplitStatements()) {
		SubstituteCatalogPlaceholders(statement.sql, schema_identifier);
		vector<Value> params;
		for (auto &param : statement.params) {
			params.push_back(PostgresParameterText(param));
		}
		auto has_nul = statement.sql.find('\0') != string::npos;
		for (auto &param : params) {
			has_nul = has_nul || (!param.IsNull() && StringValue::Get(param).find('\0') != string::npos);
		}
		if (has_nul) {
			throw InvalidInputException("A DuckLake catalog on PostgreSQL cannot store text that contains a NUL byte, "
			                            "such as a partition value, a name or a commit message");
		}
		reported_query += statement.sql + ";\n";
		statements.push_back(Value::STRUCT({{"query", Value(std::move(statement.sql))},
		                                    {"params", Value::LIST(LogicalType::VARCHAR, std::move(params))}}));
	}
	auto catalog_literal = SQLString::ToString(ducklake_catalog.MetadataDatabaseName().GetIdentifierName());
	auto query = StringUtil::Format("FROM postgres_execute_pipeline(%s, ?%s)", catalog_literal,
	                                commit ? ", commit := true" : "");
	return transaction.ExecuteRaw(query, {Value::LIST(std::move(statements))}, reported_query);
}

unique_ptr<QueryResult> PostgresMetadataManager::ExecuteDropInTransaction(string &query) {
	// DROP TABLE waits for every open transaction that read the table and the readers queued behind it fail, so it
	// gives up instead and leaves the table to a later cleanup
	DuckLakeBatch batch("SET LOCAL lock_timeout = 1;\n" + query);
	return ExecutePipeline(batch, true);
}

//! What expiring snapshots leaves behind, removed on the server: no round trip reads it to the client
vector<DuckLakeStatement> PostgresCatalogLockStatements(DuckLakeTransaction &transaction) {
	Value timeout_ms(LogicalType::UBIGINT);
	auto context = transaction.context.lock();
	if (context) {
		context->TryGetCurrentSetting("ducklake_postgres_lock_timeout_ms", timeout_ms);
	}
	auto wait = timeout_ms.IsNull() ? idx_t(0) : UBigIntValue::Get(timeout_ms);
	return {{StringUtil::Format("SET LOCAL lock_timeout = '%dms'", wait), {}},
	        {"SELECT pg_advisory_xact_lock(hashtext($1))",
	         {Value("ducklake_commit:" + transaction.GetCatalog().MetadataSchemaName().GetIdentifierName())}}};
}

static string CleanupExpiredSnapshotsSql(const vector<string> &end_snapshot_tables) {
	return StringUtil::Format(R"($ducklake_cleanup$
DECLARE
	dead_tables BIGINT[];
	r RECORD;
BEGIN
	SELECT COALESCE(ARRAY_AGG(table_id), '{}') INTO dead_tables
	FROM {METADATA_CATALOG}.ducklake_table x
	WHERE end_snapshot IS NOT NULL
	  AND NOT EXISTS (SELECT 1 FROM {METADATA_CATALOG}.ducklake_snapshot
	                  WHERE snapshot_id >= x.begin_snapshot AND snapshot_id < x.end_snapshot)
	  AND NOT EXISTS (SELECT 1 FROM {METADATA_CATALOG}.ducklake_table t2
	                  WHERE t2.table_id = x.table_id
	                    AND (t2.end_snapshot IS NULL OR EXISTS (SELECT 1 FROM {METADATA_CATALOG}.ducklake_snapshot
	                         WHERE snapshot_id >= t2.begin_snapshot AND snapshot_id < t2.end_snapshot)));
	WITH table_paths AS (
		SELECT DISTINCT ON (t.table_id) t.table_id,
		       CASE WHEN t.path IS NULL THEN schema_path
		            WHEN t.path_is_relative THEN schema_path || t.path
		            ELSE t.path END AS table_path
		FROM {METADATA_CATALOG}.ducklake_table t
		JOIN {METADATA_CATALOG}.ducklake_schema s USING (schema_id),
		LATERAL (SELECT CASE WHEN s.path IS NULL THEN {DATA_PATH}
		                     WHEN s.path_is_relative THEN {DATA_PATH} || s.path
		                     ELSE s.path END AS schema_path) sp),
	dead_data_files AS (
		DELETE FROM {METADATA_CATALOG}.ducklake_data_file f
		WHERE f.table_id = ANY(dead_tables)
		   OR (f.end_snapshot IS NOT NULL AND NOT EXISTS (SELECT 1 FROM {METADATA_CATALOG}.ducklake_snapshot s
		       WHERE s.snapshot_id >= f.begin_snapshot AND s.snapshot_id < f.end_snapshot))
		RETURNING data_file_id, table_id, path, path_is_relative),
	column_stats AS (DELETE FROM {METADATA_CATALOG}.ducklake_file_column_stats
	                 WHERE data_file_id IN (SELECT data_file_id FROM dead_data_files)),
	variant_stats AS (DELETE FROM {METADATA_CATALOG}.ducklake_file_variant_stats
	                  WHERE data_file_id IN (SELECT data_file_id FROM dead_data_files)),
	partition_values AS (DELETE FROM {METADATA_CATALOG}.ducklake_file_partition_value
	                     WHERE data_file_id IN (SELECT data_file_id FROM dead_data_files)),
	dead_delete_files AS (
		DELETE FROM {METADATA_CATALOG}.ducklake_delete_file f
		WHERE f.table_id = ANY(dead_tables)
		   OR f.data_file_id IN (SELECT data_file_id FROM dead_data_files)
		   OR (f.end_snapshot IS NOT NULL AND NOT EXISTS (SELECT 1 FROM {METADATA_CATALOG}.ducklake_snapshot s
		       WHERE s.snapshot_id >= f.begin_snapshot AND s.snapshot_id < f.end_snapshot))
		RETURNING delete_file_id AS data_file_id, table_id, path, path_is_relative),
	dead_files AS (
		SELECT * FROM dead_data_files UNION ALL SELECT * FROM dead_delete_files),
	full_paths AS (
		SELECT d.data_file_id,
		       CASE WHEN d.path_is_relative THEN COALESCE(p.table_path, {DATA_PATH}) || d.path ELSE d.path END AS full_path
		FROM dead_files d LEFT JOIN table_paths p USING (table_id))
	INSERT INTO {METADATA_CATALOG}.ducklake_files_scheduled_for_deletion
		(data_file_id, path, path_is_relative, schedule_start)
	SELECT data_file_id,
	       CASE WHEN starts_with(full_path, {DATA_PATH}) THEN substr(full_path, length({DATA_PATH}) + 1) ELSE full_path END,
	       starts_with(full_path, {DATA_PATH}), NOW()
	FROM full_paths;
	FOR r IN SELECT table_name FROM {METADATA_CATALOG}.ducklake_inlined_data_tables WHERE table_id = ANY(dead_tables) LOOP
		EXECUTE FORMAT('DROP TABLE IF EXISTS %%I.%%I', {METADATA_SCHEMA_NAME_LITERAL}, r.table_name);
	END LOOP;
	FOR r IN SELECT UNNEST(dead_tables) AS table_id LOOP
		EXECUTE FORMAT('DROP TABLE IF EXISTS %%I.%%I', {METADATA_SCHEMA_NAME_LITERAL}, 'ducklake_inlined_delete_' || r.table_id);
	END LOOP;
	DELETE FROM {METADATA_CATALOG}.ducklake_table WHERE table_id = ANY(dead_tables);
	DELETE FROM {METADATA_CATALOG}.ducklake_table_stats WHERE table_id = ANY(dead_tables);
	DELETE FROM {METADATA_CATALOG}.ducklake_table_column_stats WHERE table_id = ANY(dead_tables);
	DELETE FROM {METADATA_CATALOG}.ducklake_partition_info WHERE table_id = ANY(dead_tables);
	DELETE FROM {METADATA_CATALOG}.ducklake_partition_column WHERE table_id = ANY(dead_tables);
	DELETE FROM {METADATA_CATALOG}.ducklake_column WHERE table_id = ANY(dead_tables);
	DELETE FROM {METADATA_CATALOG}.ducklake_column_tag WHERE table_id = ANY(dead_tables);
	DELETE FROM {METADATA_CATALOG}.ducklake_sort_info WHERE table_id = ANY(dead_tables);
	DELETE FROM {METADATA_CATALOG}.ducklake_sort_expression WHERE table_id = ANY(dead_tables);
	DELETE FROM {METADATA_CATALOG}.ducklake_schema_versions WHERE table_id = ANY(dead_tables);
	DELETE FROM {METADATA_CATALOG}.ducklake_inlined_data_tables WHERE table_id = ANY(dead_tables);
	DELETE FROM {METADATA_CATALOG}.ducklake_column_mapping WHERE table_id = ANY(dead_tables);
	DELETE FROM {METADATA_CATALOG}.ducklake_metadata WHERE scope = 'table' AND scope_id = ANY(dead_tables);
	FOR r IN SELECT UNNEST(ARRAY[%s]::TEXT[]) AS table_name LOOP
		EXECUTE FORMAT('DELETE FROM %%I.%%I WHERE end_snapshot IS NOT NULL AND NOT EXISTS (SELECT 1 FROM %%I.ducklake_snapshot '
		               'WHERE snapshot_id >= begin_snapshot AND snapshot_id < end_snapshot)',
		               {METADATA_SCHEMA_NAME_LITERAL}, r.table_name, {METADATA_SCHEMA_NAME_LITERAL});
	END LOOP;
	DELETE FROM {METADATA_CATALOG}.ducklake_metadata
	WHERE scope = 'schema' AND NOT EXISTS (SELECT 1 FROM {METADATA_CATALOG}.ducklake_schema WHERE schema_id = scope_id);
	FOR r IN SELECT table_name FROM {METADATA_CATALOG}.ducklake_inlined_data_tables LOOP
		EXECUTE FORMAT('DELETE FROM %%I.%%I WHERE "_ducklake_end_snapshot" IS NOT NULL AND NOT EXISTS ('
		               'SELECT 1 FROM %%I.ducklake_snapshot WHERE snapshot_id >= "_ducklake_begin_snapshot" '
		               'AND snapshot_id < "_ducklake_end_snapshot")',
		               {METADATA_SCHEMA_NAME_LITERAL}, r.table_name, {METADATA_SCHEMA_NAME_LITERAL});
	END LOOP;
	DELETE FROM {METADATA_CATALOG}.ducklake_macro_impl m
	WHERE NOT EXISTS (SELECT 1 FROM {METADATA_CATALOG}.ducklake_macro x WHERE x.macro_id = m.macro_id);
	DELETE FROM {METADATA_CATALOG}.ducklake_macro_parameters m
	WHERE NOT EXISTS (SELECT 1 FROM {METADATA_CATALOG}.ducklake_macro x WHERE x.macro_id = m.macro_id);
	DELETE FROM {METADATA_CATALOG}.ducklake_name_mapping n
	WHERE NOT EXISTS (SELECT 1 FROM {METADATA_CATALOG}.ducklake_column_mapping c WHERE c.mapping_id = n.mapping_id);
END
$ducklake_cleanup$)",
	                          StringUtil::Join(end_snapshot_tables, end_snapshot_tables.size(), ", ",
	                                           [](const string &table) { return "'" + table + "'"; }));
}

static DuckLakeBatch CleanupExpiredSnapshotsBatch(DuckLakeCatalog &catalog) {
	vector<string> end_snapshot_tables {"ducklake_schema", "ducklake_view", "ducklake_tag", "ducklake_macro"};
	if (catalog.SupportsV1_1Metadata()) {
		end_snapshot_tables.push_back("ducklake_view_column_tag");
	}
	if (catalog.SupportsSnapshotTags()) {
		end_snapshot_tables.push_back("ducklake_snapshot_tag");
	}
	return DuckLakeBatch("DO " + CleanupExpiredSnapshotsSql(end_snapshot_tables));
}

void PostgresMetadataManager::CleanupExpiredSnapshots(const vector<DuckLakeSnapshotInfo> &) {
	auto batch = CleanupExpiredSnapshotsBatch(transaction.GetCatalog());
	Execute(batch)->ThrowIfError("Failed to clean up expired snapshots in DuckLake: ");
	// the cleanup can drop inlined tables
	ClearCache();
}

void PostgresMetadataManager::DeleteSnapshots(const vector<DuckLakeSnapshotInfo> &snapshots) {
	// the deletion and its cleanup share one pipeline
	DuckLakeBatch batch;
	for (auto &statement : PostgresCatalogLockStatements(transaction)) {
		batch += statement;
	}
	batch += DuckLakeBatch(DeleteSnapshotsSql(snapshots)) + CleanupExpiredSnapshotsBatch(transaction.GetCatalog());
	auto result = Execute(batch);
	if (result->HasError() && StringUtil::Contains(result->GetError(), "lock timeout")) {
		throw TransactionException(
		    "Expiring snapshots waited longer than ducklake_postgres_lock_timeout_ms for the lock "
		    "of the catalog, which another commit holds: %s",
		    result->GetError());
	}
	result->ThrowIfError("Failed to delete snapshots in DuckLake: ");
	// the cleanup can drop inlined tables
	ClearCache();
}

void PostgresMetadataManager::ClearCache() {
	// clear the cache of the metadata catalog alone, a global clear breaks queries running in other Postgres catalogs
	auto &catalog = transaction.GetCatalog();
	if (catalog.ScopedCacheClear()) {
		auto catalog_literal = SQLString::ToString(catalog.MetadataDatabaseName().GetIdentifierName());
		// without a round trip, or with one on a postgres_scanner from before the catalog option
		vector<pair<string, string>> scoped_clears {
		    {StringUtil::Format("CALL postgres_clear_cache(catalog := %s);", catalog_literal), "catalog"},
		    {StringUtil::Format("SELECT * FROM postgres_query(%s, 'SELECT 1', clear_catalog_cache = true);",
		                        catalog_literal),
		     "clear_catalog_cache"}};
		for (auto &scoped_clear : scoped_clears) {
			auto result = transaction.ExecuteRaw(scoped_clear.first);
			if (!result->HasError()) {
				return;
			}
			// the error of a postgres_scanner from before the option
			if (!StringUtil::Contains(result->GetError(), "Invalid named parameter \"" + scoped_clear.second + "\"")) {
				result->ThrowIfError("Failed to clear the PostgreSQL metadata cache: ");
			}
		}
		// a postgres_scanner from before both options can only clear every catalog
		catalog.SetScopedCacheClear(false);
	}
	auto result = transaction.ExecuteRaw("CALL postgres_clear_cache();");
	result->ThrowIfError("Failed to clear the PostgreSQL metadata cache: ");
}

static string PostgresQuery(const string &remote_query) {
	// the query runs at bind, which saves the round trip that describes it
	return StringUtil::Format(
	    "SELECT * FROM postgres_query({METADATA_CATALOG_NAME_LITERAL}, %s, execute_in_bind := true)",
	    SQLString(remote_query));
}

//! The metadata tables that the catalog of a snapshot is built from
static const vector<string> &CatalogTables() {
	static const vector<string> tables {"ducklake_schema",
	                                    "ducklake_table",
	                                    "ducklake_tag",
	                                    "ducklake_inlined_data_tables",
	                                    "ducklake_column",
	                                    "ducklake_column_tag",
	                                    "ducklake_view",
	                                    "ducklake_view_column_tag",
	                                    "ducklake_macro",
	                                    "ducklake_macro_impl",
	                                    "ducklake_macro_parameters",
	                                    "ducklake_partition_info",
	                                    "ducklake_partition_column",
	                                    "ducklake_sort_info",
	                                    "ducklake_sort_expression"};
	return tables;
}

DuckLakeCatalogInfo PostgresMetadataManager::GetCatalogForSnapshot(DuckLakeSnapshot snapshot) {
	auto &catalog = transaction.GetCatalog();
	auto version = catalog.GetDuckLakeVersion();
	// one round trip reads every column of the tables that the catalog is built from, as an array per column
	// the rows go to temporary tables of fixed names, so loads on the connection take turns
	static mutex load_lock;
	lock_guard<mutex> guard(load_lock);
	string local_prefix = "temp.main.";
	vector<string> remote_tables;
	string local_tables;
	for (auto &table : CatalogTables()) {
		vector<pair<string, string>> columns;
		auto names = DuckLakeMetadataSchema::HasTable(table, version)
		                 ? DuckLakeMetadataSchema::ColumnNames(table, version)
		                 : vector<string>();
		for (auto &column : DuckLakeMetadataSchema::ColumnTypes(table)) {
			if (std::find(names.begin(), names.end(), column.first) != names.end()) {
				columns.push_back(column);
			}
		}
		if (columns.empty()) {
			// a table that the version does not have is empty
			local_tables += StringUtil::Format("CREATE OR REPLACE TEMP TABLE %s%s (%s);\n", local_prefix, table,
			                                   StringUtil::Join(DuckLakeMetadataSchema::ColumnTypes(table),
			                                                    DuckLakeMetadataSchema::ColumnTypes(table).size(), ", ",
			                                                    [](const pair<string, string> &column) {
				                                                    return column.first + " " + column.second;
			                                                    }));
			continue;
		}
		vector<string> arrays, rows;
		bool versioned = false;
		for (auto &column : columns) {
			arrays.push_back(
			    StringUtil::Format("array_agg(t.%s ORDER BY t.ctid) AS %s__%s", column.first, table, column.first));
			rows.push_back(StringUtil::Format("unnest(%s__%s) AS %s", table, column.first, column.first));
			versioned = versioned || column.first == "end_snapshot";
		}
		auto filter = versioned ? " WHERE {SNAPSHOT_ID} >= t.begin_snapshot AND ({SNAPSHOT_ID} < t.end_snapshot OR "
		                          "t.end_snapshot IS NULL)"
		                        : "";
		remote_tables.push_back(StringUtil::Format("(SELECT %s FROM {METADATA_CATALOG}.%s t%s) %s",
		                                           StringUtil::Join(arrays, ", "), table, filter, table));
		// the catalog queries read the rows from temporary tables of the same name
		local_tables +=
		    StringUtil::Format("CREATE OR REPLACE TEMP TABLE %s%s AS SELECT %s FROM __ducklake_catalog_rows;\n",
		                       local_prefix, table, StringUtil::Join(rows, ", "));
	}
	string remote_query = "SELECT * FROM " + StringUtil::Join(remote_tables, " CROSS JOIN ");
	SubstituteTransactionPlaceholders(snapshot, remote_query);
	SubstituteCatalogPlaceholders(remote_query, SQLQuotedIdentifier::ToString(catalog.MetadataSchemaName()));
	auto query = "CREATE OR REPLACE TEMP TABLE __ducklake_catalog_rows AS " + PostgresQuery(remote_query) + ";\n" +
	             local_tables + "DROP TABLE __ducklake_catalog_rows;\n";
	auto result = Query(snapshot, query);
	result->ThrowIfError("Failed to read the catalog from DuckLake: ");
	auto catalog_info = BuildCatalogForSnapshot(
	    snapshot,
	    [this](DuckLakeSnapshot s, string q) {
		    SubstituteTransactionPlaceholders(s, q);
		    SubstituteCatalogPlaceholders(q, "temp.main");
		    return transaction.ExecuteRaw(q);
	    },
	    catalog.DataPath(), catalog.Separator(), catalog.SupportsV1_1Metadata());
	string drops;
	for (auto &table : CatalogTables()) {
		drops += "DROP TABLE " + local_prefix + table + ";\n";
	}
	Query(snapshot, drops)->ThrowIfError("Failed to read the catalog from DuckLake: ");
	return catalog_info;
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
                                                      const vector<idx_t> &runtime_filter_stats_columns,
                                                      FileListType file_list_type, const string &) {
	auto schema = SQLQuotedIdentifier::ToString(transaction.GetCatalog().MetadataSchemaName());
	auto remote_query = DuckLakeMetadataManager::GenerateFileListQuery(
	    table, snapshot, filter_info, dynamic_filters, runtime_filter_stats_columns, file_list_type, schema);
	// placeholders inside the literal are not replaced later
	SubstituteSnapshotPlaceholders(snapshot, remote_query);
	return PostgresQuery(remote_query);
}

} // namespace duckdb

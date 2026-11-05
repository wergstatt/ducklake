#include "metadata_manager/postgres_metadata_manager.hpp"

#include "common/ducklake_util.hpp"
#include "duckdb/common/json_document.hpp"
#include "duckdb/common/types/blob.hpp"
#include "duckdb/main/client_context.hpp"
#include "storage/ducklake_catalog.hpp"
#include "storage/ducklake_schema_entry.hpp"
#include "storage/ducklake_stats.hpp"
#include "storage/ducklake_table_entry.hpp"
#include "storage/ducklake_transaction.hpp"
#include "storage/ducklake_transaction_changes.hpp"

namespace duckdb {

//! The latest snapshot, which is the one this commit inserts once the commit has inserted it
static constexpr const char *LATEST_SNAPSHOT =
    "(SELECT snapshot_id, next_file_id FROM {METADATA_CATALOG}.ducklake_snapshot ORDER BY snapshot_id DESC LIMIT 1)";

static bool BooleanSetting(DuckLakeTransaction &transaction, const char *name) {
	auto context = transaction.context.lock();
	Value enabled;
	return context && context->TryGetCurrentSetting(name, enabled) && !enabled.IsNull() && BooleanValue::Get(enabled);
}

static Value PipelineStatement(const string &query, vector<Value> params) {
	return Value::STRUCT({{"query", Value(query)}, {"params", Value::LIST(LogicalType::VARCHAR, std::move(params))}});
}

static Value TextOrNull(bool is_valid, const string &text) {
	return is_valid ? Value(text) : Value(LogicalType::VARCHAR);
}

static JSONMutableValue OptionalIdxOrNull(JSONWriter &json, optional_idx value) {
	return value.IsValid() ? json.CreateUnsignedInteger(value.GetIndex()) : json.CreateNull();
}

static JSONMutableValue StringOrNull(JSONWriter &json, bool is_valid, const string &value) {
	return is_valid ? json.CreateString(value) : json.CreateNull();
}

//! A JSON array of objects, sent as one parameter that jsonb_to_recordset turns into rows
struct PipelineRows {
	JSONWriter json;
	JSONMutableValue rows = json.CreateArray();
	idx_t count = 0;

	JSONMutableValue NewRow() {
		count++;
		return json.CreateObject();
	}
	Value ToValue() {
		json.SetRoot(rows);
		return Value(json.ToString());
	}
};

//! The merged min or max of a column: the bound of the new values if it is beyond the stored one
static string MergedBound(const string &stored, const string &added, const string &stored_order,
                          const string &added_order, bool is_min) {
	auto beyond = is_min ? " < " : " > ";
	return StringUtil::Format("CASE WHEN NOT n.any_valid THEN g.%s "
	                          "WHEN g.min_value IS NULL AND g.max_value IS NULL AND g.extra_stats IS NULL THEN n.%s "
	                          "WHEN n.%s IS NULL OR g.%s IS NULL OR %s IS NULL OR %s IS NULL THEN NULL "
	                          "WHEN %s%s%s THEN n.%s ELSE g.%s END",
	                          stored, added, added, stored, added_order, stored_order, added_order, beyond,
	                          stored_order, added, stored);
}

//! Whether the merged bound is exact, as DuckLakeColumnStats::MergeBound decides it, and NULL without a bound
static string MergedBoundIsExact(const string &stored, const string &added, const string &stored_order,
                                 const string &added_order, bool is_min, bool requires_value_comparison) {
	auto bound = MergedBound(stored, added, stored_order, added_order, is_min);
	if (requires_value_comparison) {
		// the bounds of values are always exact
		return "CASE WHEN (" + bound + ") IS NULL THEN NULL ELSE true END";
	}
	auto stored_exact = is_min ? "min_is_exact" : "max_is_exact";
	auto added_exact = is_min ? "min_exact" : "max_exact";
	auto beyond = is_min ? " < " : " > ";
	return StringUtil::Format("CASE WHEN (%s) IS NULL THEN NULL WHEN NOT n.any_valid THEN g.%s "
	                          "WHEN g.min_value IS NULL AND g.max_value IS NULL AND g.extra_stats IS NULL THEN n.%s "
	                          "WHEN %s%s%s THEN n.%s WHEN %s = %s THEN g.%s AND n.%s ELSE g.%s END",
	                          bound, stored_exact, added_exact, added_order, beyond, stored_order, added_exact,
	                          added_order, stored_order, stored_exact, added_exact, stored_exact);
}

string PostgresMetadataManager::BoundOrderExpression(const string &bound, const LogicalType &type) {
	if (type.IsNested() || type.id() == LogicalTypeId::TIME_TZ) {
		return string();
	}
	if (RequiresValueComparison(type)) {
		return CastStatsToTarget(bound, type, StatsCastType::ORDERING);
	}
	// the client compares the bounds of other types as bytes
	return DuckLakeUtil::WithBinaryCollation(bound);
}

//! The stats of new values that the server can merge
static bool HasBoundsWithoutNul(const DuckLakeColumnStats &stats) {
	return (!stats.has_min || stats.min.find('\0') == string::npos) &&
	       (!stats.has_max || stats.max.find('\0') == string::npos);
}

//! How an inlined value is sent: as its text, which Postgres casts to the column type, or as hex bytes for BYTEA
enum class InlinedValueEncoding { UNSUPPORTED, TEXT, HEX };

static InlinedValueEncoding GetInlinedValueEncoding(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UHUGEINT:
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
	case LogicalTypeId::DECIMAL:
	case LogicalTypeId::DATE:
	case LogicalTypeId::TIME:
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::INTERVAL:
	case LogicalTypeId::UUID:
		return InlinedValueEncoding::TEXT;
	case LogicalTypeId::VARCHAR:
	case LogicalTypeId::BLOB:
		// stored as BYTEA, which keeps the NUL bytes of a string
		return InlinedValueEncoding::HEX;
	default:
		return InlinedValueEncoding::UNSUPPORTED;
	}
}

static string ToHex(const string &bytes) {
	static constexpr const char *DIGITS = "0123456789abcdef";
	string result;
	result.reserve(bytes.size() * 2);
	for (auto byte : bytes) {
		result += DIGITS[static_cast<uint8_t>(byte) >> 4];
		result += DIGITS[static_cast<uint8_t>(byte) & 0x0F];
	}
	return result;
}

//! The inlined-data table that new rows of the table go to, empty if the commit has to create one
static Identifier CurrentInlinedTable(const DuckLakeTableEntry &table, const DuckLakeTableSchemaVersions &versions) {
	Identifier result;
	optional_idx latest;
	for (auto &inlined_table : table.GetInlinedDataTables()) {
		if (DuckLakeTableEntry::IsCurrentInlinedDataTable(inlined_table.schema_version, versions.last_change) &&
		    (!latest.IsValid() || inlined_table.schema_version > latest.GetIndex())) {
			latest = inlined_table.schema_version;
			result = inlined_table.table_name;
		}
	}
	return result;
}

//! Whether the server can merge the stats of the new values of a column
static bool ServerCanMerge(PostgresMetadataManager &manager, const DuckLakeColumnStats &stats) {
	return !stats.extra_stats && !stats.bounds_unknown && HasBoundsWithoutNul(stats) &&
	       !manager.BoundOrderExpression("x", stats.type).empty();
}

vector<Value> PostgresMetadataManager::PipelineCommitStatements(DuckLakeSnapshot transaction_snapshot,
                                                                const TransactionChangeInformation &changes,
                                                                idx_t &snapshot_statement) {
	auto &catalog = transaction.GetCatalog();
	if (transaction_snapshot.snapshot_id == DConstants::INVALID_INDEX || !catalog.SupportsV1_1Metadata() ||
	    !IsDataOnlyCommit(changes) || RequiresClientSideCommit(transaction) || transaction.ChangesConfigOptions() ||
	    !changes.tables_deleted_from.empty() || !changes.tables_deleted_inlined.empty() ||
	    !changes.tables_flushed_inlined.empty() || !changes.tables_merge_adjacent.empty() ||
	    !changes.tables_rewrite_delete.empty() || !changes.expired_snapshots.empty() ||
	    !transaction.GetDroppedFiles().empty() || !transaction.GetNewNameMaps().name_maps.empty()) {
		return {};
	}
	PipelineRows files, file_stats, partition_values, table_stats, tables;
	// the column stats to merge, by the expression that orders the bounds of their type
	map<string, pair<LogicalType, unique_ptr<PipelineRows>>> column_stats;
	PipelineRows guarded_columns;
	vector<string> involved_tables, involved_schemas;
	vector<pair<string, vector<Value>>> inlined_inserts;
	idx_t file_offset = 0;
	for (auto &entry : transaction.GetLocalChanges().Changes()) {
		auto table_id = entry.first;
		auto &table_changes = entry.second;
		if (IsTransactionLocal(table_id) || !table_changes.new_delete_files.empty() ||
		    !table_changes.new_inlined_data_deletes.empty() || table_changes.new_inlined_file_deletes ||
		    !table_changes.compactions.empty() || !table_changes.compacted_files.empty()) {
			return {};
		}
		if (table_changes.new_data_files.empty() && !table_changes.new_inlined_data) {
			continue;
		}
		auto table_entry = catalog.GetEntryById(transaction, transaction_snapshot, table_id);
		if (!table_entry) {
			return {};
		}
		auto &table = table_entry->Cast<DuckLakeTableEntry>();
		involved_tables.push_back(to_string(table_id.index));
		involved_schemas.push_back(to_string(table.ParentSchema().Cast<DuckLakeSchemaEntry>().GetSchemaId().index));
		tables.rows.Append(tables.json.CreateUnsignedInteger(table_id.index));

		DuckLakeTableStats added;
		idx_t row_offset = 0;
		for (auto &file : table_changes.new_data_files) {
			if (!file.delete_files.empty() || file.flush_row_id_start.IsValid() || file.begin_snapshot.IsValid() ||
			    file.max_partial_file_snapshot.IsValid() || file.mapping_id.IsValid() || !file.created_by_ducklake ||
			    (file.partition_id.IsValid() &&
			     file.partition_id.GetIndex() >= DuckLakeConstants::TRANSACTION_LOCAL_ID_START)) {
				return {};
			}
			for (auto &stats : file.column_stats) {
				if (!ServerCanMerge(*this, stats.second)) {
					return {};
				}
			}
			auto path = GetRelativePath(file.file_name, table.DataPath(), catalog.Separator());
			auto row = files.NewRow();
			row.Add("k", files.json.CreateUnsignedInteger(file_offset));
			row.Add("t", files.json.CreateUnsignedInteger(table_id.index));
			row.AddString("path", path.path);
			row.Add("rel", files.json.CreateBoolean(path.path_is_relative));
			row.Add("rows", files.json.CreateUnsignedInteger(file.row_count));
			row.Add("bytes", files.json.CreateUnsignedInteger(file.file_size_bytes));
			row.Add("footer", OptionalIdxOrNull(files.json, file.footer_size));
			row.Add("row_offset", files.json.CreateUnsignedInteger(row_offset));
			row.Add("part", OptionalIdxOrNull(files.json, file.partition_id));
			row.Add("key", StringOrNull(files.json, !file.encryption_key.empty(),
			                            Blob::ToBase64(string_t(file.encryption_key))));
			row.Add("rgc", OptionalIdxOrNull(files.json, file.row_group_count));
			files.rows.Append(row);
			for (auto &entry : file.column_stats) {
				auto &stats = entry.second;
				auto stats_row = file_stats.NewRow();
				auto &json = file_stats.json;
				stats_row.Add("k", json.CreateUnsignedInteger(file_offset));
				stats_row.Add("t", json.CreateUnsignedInteger(table_id.index));
				stats_row.Add("c", json.CreateUnsignedInteger(entry.first.index));
				stats_row.Add("size", json.CreateUnsignedInteger(stats.column_size_bytes));
				// as DuckLakeColumnStatsInfo::FromColumnStats writes them
				bool has_counts = stats.has_null_count && stats.has_num_values && stats.null_count <= stats.num_values;
				stats_row.Add("vals", has_counts ? json.CreateUnsignedInteger(stats.num_values - stats.null_count)
				                                 : json.CreateNull());
				stats_row.Add("nulls", has_counts ? json.CreateUnsignedInteger(stats.null_count) : json.CreateNull());
				stats_row.Add("min_value", StringOrNull(json, stats.has_min, stats.min));
				stats_row.Add("max_value", StringOrNull(json, stats.has_max, stats.max));
				stats_row.Add("nan",
				              stats.has_contains_nan ? json.CreateBoolean(stats.contains_nan) : json.CreateNull());
				stats_row.Add("min_exact",
				              stats.has_min ? json.CreateBoolean(stats.EffectiveMinIsExact()) : json.CreateNull());
				stats_row.Add("max_exact",
				              stats.has_max ? json.CreateBoolean(stats.EffectiveMaxIsExact()) : json.CreateNull());
				file_stats.rows.Append(stats_row);
			}
			for (auto &partition : file.partition_values) {
				auto partition_row = partition_values.NewRow();
				auto &json = partition_values.json;
				partition_row.Add("k", json.CreateUnsignedInteger(file_offset));
				partition_row.Add("t", json.CreateUnsignedInteger(table_id.index));
				partition_row.Add("idx", json.CreateUnsignedInteger(partition.partition_column_idx));
				partition_row.Add("val", StringOrNull(json, !partition.partition_value.IsNull(),
				                                      partition.partition_value.ToString()));
				partition_values.rows.Append(partition_row);
			}
			added.MergeFileStats(file, file.column_stats);
			row_offset += file.row_count;
			file_offset++;
		}
		if (table_changes.new_inlined_data) {
			auto &inlined = *table_changes.new_inlined_data;
			auto inlined_table = CurrentInlinedTable(table, catalog.GetTableSchemaVersions(transaction, table_id));
			auto columns = table.GetTableColumns();
			if (inlined.HasPreservedRowIds() || !inlined.data || inlined_table.empty() ||
			    inlined.data->ColumnCount() != columns.size()) {
				return {};
			}
			for (auto &stats : inlined.column_stats) {
				if (!ServerCanMerge(*this, stats.second)) {
					return {};
				}
			}
			auto &types = inlined.data->Types();
			vector<string> values;
			for (idx_t c = 0; c < columns.size(); c++) {
				auto encoding = GetInlinedValueEncoding(types[c]);
				if (encoding == InlinedValueEncoding::UNSUPPORTED) {
					return {};
				}
				auto element = StringUtil::Format("e->>%d", c + 1);
				values.push_back(encoding == InlinedValueEncoding::HEX
				                     ? "decode(" + element + ", 'hex')"
				                     : StringUtil::Format("CAST(%s AS %s)", element, GetColumnType(columns[c])));
			}
			// one array per row: its position, then its values
			PipelineRows rows;
			idx_t position = 0;
			for (auto &chunk : inlined.data->Chunks()) {
				for (idx_t r = 0; r < chunk.size(); r++) {
					auto row = rows.json.CreateArray();
					row.Append(rows.json.CreateUnsignedInteger(position++));
					for (idx_t c = 0; c < chunk.ColumnCount(); c++) {
						auto value = chunk.GetValue(c, r);
						if (value.IsNull()) {
							row.Append(rows.json.CreateNull());
						} else if (GetInlinedValueEncoding(types[c]) == InlinedValueEncoding::HEX) {
							row.AppendString(ToHex(StringValue::Get(value)));
						} else {
							row.AppendString(value.ToString());
						}
					}
					rows.rows.Append(row);
				}
			}
			inlined_inserts.emplace_back(
			    StringUtil::Format("INSERT INTO {METADATA_CATALOG}.%s "
			                       "SELECT ts.next_row_id + $2::BIGINT + (e->>0)::BIGINT, s.snapshot_id, NULL, %s "
			                       "FROM jsonb_array_elements($1::JSONB) AS e "
			                       "JOIN {METADATA_CATALOG}.ducklake_table_stats ts ON ts.table_id = $3::BIGINT "
			                       "CROSS JOIN %s s",
			                       SQLIdentifier::ToString(inlined_table), StringUtil::Join(values, ", "),
			                       LATEST_SNAPSHOT),
			    vector<Value> {rows.ToValue(), Value(to_string(row_offset)), Value(to_string(table_id.index))});
			for (auto &stats : inlined.column_stats) {
				added.MergeStats(stats.first, stats.second);
			}
			added.record_count += inlined.Count();
			added.next_row_id += inlined.Count();
			if (table_changes.new_data_files.empty()) {
				// a commit of only inlined rows takes a file id, as the client-side commit does
				file_offset++;
			}
		}
		auto stats_row = table_stats.NewRow();
		stats_row.Add("t", table_stats.json.CreateUnsignedInteger(table_id.index));
		stats_row.Add("rows", table_stats.json.CreateUnsignedInteger(added.record_count));
		stats_row.Add("bytes", table_stats.json.CreateUnsignedInteger(added.table_size_bytes));
		table_stats.rows.Append(stats_row);

		for (auto &entry : added.column_stats) {
			auto &stats = entry.second;
			auto order = BoundOrderExpression("x", stats.type);
			auto &group = column_stats[order];
			if (!group.second) {
				group.first = stats.type;
				group.second = make_uniq<PipelineRows>();
			}
			auto &rows = *group.second;
			auto row = rows.NewRow();
			row.Add("t", rows.json.CreateUnsignedInteger(table_id.index));
			row.Add("c", rows.json.CreateUnsignedInteger(entry.first.index));
			row.Add("has_nulls",
			        stats.has_null_count ? rows.json.CreateBoolean(stats.null_count > 0) : rows.json.CreateNull());
			row.Add("nan",
			        stats.has_contains_nan ? rows.json.CreateBoolean(stats.contains_nan) : rows.json.CreateNull());
			row.Add("any_valid", rows.json.CreateBoolean(stats.AnyValid()));
			row.Add("min_value", StringOrNull(rows.json, stats.has_min, stats.min));
			row.Add("max_value", StringOrNull(rows.json, stats.has_max, stats.max));
			row.Add("min_exact", rows.json.CreateBoolean(stats.min_is_exact));
			row.Add("max_exact", rows.json.CreateBoolean(stats.max_is_exact));
			rows.rows.Append(row);

			auto guarded = guarded_columns.NewRow();
			guarded.Add("t", guarded_columns.json.CreateUnsignedInteger(table_id.index));
			guarded.Add("c", guarded_columns.json.CreateUnsignedInteger(entry.first.index));
			guarded.Add("any_valid", guarded_columns.json.CreateBoolean(stats.AnyValid()));
			guarded_columns.rows.Append(guarded);
		}
	}
	if (involved_tables.empty()) {
		return {};
	}
	auto file_count = Value(to_string(file_offset));
	vector<Value> statements;
	if (BooleanSetting(transaction, "ducklake_postgres_pipeline_lock")) {
		// the pipeline commits of a catalog wait for each other, so that their ids follow the order of the commits. An
		// advisory lock does not wait for other writers of the metadata, such as an open transaction that expired
		// snapshots, whose snapshot ids the primary key checks. A lock that takes too long falls back to the client.
		// Without the lock, a commit that lost a race fails on the primary key and retries on the client
		statements.push_back(PipelineStatement("SET LOCAL idle_in_transaction_session_timeout = '10s'", {}));
		for (auto &lock_statement : PostgresCatalogLockStatements(transaction)) {
			statements.push_back(PipelineStatement(lock_statement.sql, lock_statement.params));
		}
	}
	// a concurrent change that the client-side commit checks for, or stats it would complete, abort the commit with a
	// not-null violation, after which the client-side commit runs
	auto conflicts = StringUtil::Format(
	    "(^|,)((dropped_table|altered_table|deleted_from_table|inlined_delete):(%s)|dropped_schema:(%s))(,|$)",
	    StringUtil::Join(involved_tables, "|"), StringUtil::Join(involved_schemas, "|"));
	// the first insert into a table creates its stats, as empty ones that the updates below fill. Without the lock,
	// two first inserts could both create them, so the client-side commit does
	if (BooleanSetting(transaction, "ducklake_postgres_pipeline_lock")) {
		statements.push_back(PipelineStatement(
		    "INSERT INTO {METADATA_CATALOG}.ducklake_table_column_stats (table_id, column_id, contains_null, "
		    "contains_nan) "
		    "SELECT n.t, n.c, false, false FROM jsonb_to_recordset($1::JSONB) AS n(t BIGINT, c BIGINT) "
		    "WHERE NOT EXISTS (SELECT 1 FROM {METADATA_CATALOG}.ducklake_table_stats ts WHERE ts.table_id = n.t) "
		    "AND NOT EXISTS (SELECT 1 FROM {METADATA_CATALOG}.ducklake_table_column_stats g "
		    "WHERE g.table_id = n.t AND g.column_id = n.c)",
		    {guarded_columns.ToValue()}));
		statements.push_back(
		    PipelineStatement("INSERT INTO {METADATA_CATALOG}.ducklake_table_stats (table_id, record_count, "
		                      "next_row_id, file_size_bytes) "
		                      "SELECT t.id::BIGINT, 0, 0, 0 FROM jsonb_array_elements_text($1::JSONB) AS t(id) "
		                      "WHERE NOT EXISTS (SELECT 1 FROM {METADATA_CATALOG}.ducklake_table_stats ts WHERE "
		                      "ts.table_id = t.id::BIGINT)",
		                      {tables.ToValue()}));
	}
	snapshot_statement = statements.size();
	statements.push_back(PipelineStatement(
	    "INSERT INTO {METADATA_CATALOG}.ducklake_snapshot "
	    "SELECT CASE WHEN EXISTS (SELECT 1 FROM {METADATA_CATALOG}.ducklake_snapshot_changes c "
	    "WHERE c.snapshot_id > $1::BIGINT AND c.changes_made ~ $2) "
	    "OR EXISTS (SELECT 1 FROM jsonb_array_elements_text($3::JSONB) AS t(id) WHERE NOT EXISTS "
	    "(SELECT 1 FROM {METADATA_CATALOG}.ducklake_table_stats ts WHERE ts.table_id = t.id::BIGINT "
	    "AND ts.next_row_id IS NOT NULL AND ts.file_size_bytes IS NOT NULL)) "
	    "OR EXISTS (SELECT 1 FROM jsonb_to_recordset($4::JSONB) AS n(t BIGINT, c BIGINT, any_valid BOOLEAN) "
	    "LEFT JOIN {METADATA_CATALOG}.ducklake_table_stats ts ON ts.table_id = n.t "
	    "LEFT JOIN {METADATA_CATALOG}.ducklake_table_column_stats g ON g.table_id = n.t AND g.column_id = n.c "
	    "WHERE g.table_id IS NULL OR (n.any_valid AND ts.record_count > 0 AND g.min_value IS NULL AND g.max_value IS "
	    "NULL AND g.extra_stats IS NULL)) "
	    "THEN NULL ELSE s.snapshot_id + 1 END, clock_timestamp(), s.schema_version, s.next_catalog_id, "
	    "s.next_file_id + $5::BIGINT "
	    "FROM {METADATA_CATALOG}.ducklake_snapshot s ORDER BY s.snapshot_id DESC LIMIT 1 RETURNING snapshot_id",
	    {Value(to_string(transaction_snapshot.snapshot_id)), Value(conflicts), tables.ToValue(),
	     guarded_columns.ToValue(), file_count}));
	if (files.count > 0) {
		statements.push_back(PipelineStatement(
		    StringUtil::Format(
		        "INSERT INTO {METADATA_CATALOG}.ducklake_data_file "
		        "SELECT s.next_file_id - $2::BIGINT + f.k, f.t, s.snapshot_id, NULL, NULL, f.path, f.rel, 'parquet', "
		        "f.rows, f.bytes, f.footer, ts.next_row_id + f.row_offset, f.part, f.key, NULL, NULL, f.rgc "
		        "FROM jsonb_to_recordset($1::JSONB) AS f(k BIGINT, t BIGINT, path VARCHAR, rel BOOLEAN, rows BIGINT, "
		        "bytes "
		        "BIGINT, footer BIGINT, row_offset BIGINT, part BIGINT, key VARCHAR, rgc BIGINT) "
		        "JOIN {METADATA_CATALOG}.ducklake_table_stats ts ON ts.table_id = f.t CROSS JOIN %s s",
		        LATEST_SNAPSHOT),
		    {files.ToValue(), file_count}));
		statements.push_back(PipelineStatement(
		    StringUtil::Format(
		        "INSERT INTO {METADATA_CATALOG}.ducklake_file_column_stats "
		        "SELECT s.next_file_id - $2::BIGINT + c.k, c.t, c.c, c.size, c.vals, c.nulls, c.min_value, "
		        "c.max_value, c.nan, NULL, c.min_exact, c.max_exact "
		        "FROM jsonb_to_recordset($1::JSONB) AS c(k BIGINT, t BIGINT, c BIGINT, size BIGINT, vals "
		        "BIGINT, nulls BIGINT, min_value VARCHAR, max_value VARCHAR, nan BOOLEAN, min_exact "
		        "BOOLEAN, max_exact BOOLEAN) CROSS JOIN %s s",
		        LATEST_SNAPSHOT),
		    {file_stats.ToValue(), file_count}));
	}
	if (partition_values.count > 0) {
		statements.push_back(PipelineStatement(
		    StringUtil::Format("INSERT INTO {METADATA_CATALOG}.ducklake_file_partition_value "
		                       "SELECT s.next_file_id - $2::BIGINT + p.k, p.t, p.idx, p.val "
		                       "FROM jsonb_to_recordset($1::JSONB) AS p(k BIGINT, t BIGINT, idx BIGINT, val VARCHAR) "
		                       "CROSS JOIN %s s",
		                       LATEST_SNAPSHOT),
		    {partition_values.ToValue(), file_count}));
	}
	for (auto &insert : inlined_inserts) {
		statements.push_back(PipelineStatement(insert.first, std::move(insert.second)));
	}
	for (auto &group : column_stats) {
		auto &type = group.second.first;
		auto stored_min = BoundOrderExpression("g.min_value", type);
		auto added_min = BoundOrderExpression("n.min_value", type);
		auto stored_max = BoundOrderExpression("g.max_value", type);
		auto added_max = BoundOrderExpression("n.max_value", type);
		auto value_comparison = RequiresValueComparison(type);
		statements.push_back(PipelineStatement(
		    StringUtil::Format(
		        "UPDATE {METADATA_CATALOG}.ducklake_table_column_stats g SET "
		        "contains_null = CASE WHEN n.has_nulls IS NULL OR g.contains_null IS NULL THEN NULL ELSE "
		        "g.contains_null OR n.has_nulls END, "
		        "contains_nan = CASE WHEN n.nan IS NULL OR g.contains_nan IS NULL THEN NULL ELSE g.contains_nan OR "
		        "n.nan END, "
		        "min_value = %s, max_value = %s, min_is_exact = %s, max_is_exact = %s "
		        "FROM jsonb_to_recordset($1::JSONB) AS n(t BIGINT, c BIGINT, has_nulls BOOLEAN, nan BOOLEAN, "
		        "any_valid BOOLEAN, min_value VARCHAR, max_value VARCHAR, min_exact BOOLEAN, max_exact BOOLEAN) "
		        "WHERE g.table_id = n.t AND g.column_id = n.c",
		        MergedBound("min_value", "min_value", stored_min, added_min, true),
		        MergedBound("max_value", "max_value", stored_max, added_max, false),
		        MergedBoundIsExact("min_value", "min_value", stored_min, added_min, true, value_comparison),
		        MergedBoundIsExact("max_value", "max_value", stored_max, added_max, false, value_comparison)),
		    {group.second.second->ToValue()}));
	}
	statements.push_back(PipelineStatement(
	    "UPDATE {METADATA_CATALOG}.ducklake_table_stats ts SET record_count = ts.record_count + n.rows, "
	    "next_row_id = ts.next_row_id + n.rows, file_size_bytes = ts.file_size_bytes + n.bytes "
	    "FROM jsonb_to_recordset($1::JSONB) AS n(t BIGINT, rows BIGINT, bytes BIGINT) WHERE ts.table_id = n.t",
	    {table_stats.ToValue()}));
	vector<string> changes_made;
	for (auto &table_id : changes.tables_inserted_into) {
		changes_made.push_back("inserted_into_table:" + to_string(table_id.index));
	}
	for (auto &table_id : changes.tables_inserted_inlined) {
		changes_made.push_back("inlined_insert:" + to_string(table_id.index));
	}
	auto &commit_info = transaction.GetCommitInfo();
	statements.push_back(PipelineStatement(
	    StringUtil::Format("INSERT INTO {METADATA_CATALOG}.ducklake_snapshot_changes "
	                       "SELECT s.snapshot_id, $1, $2, $3, $4 FROM %s s",
	                       LATEST_SNAPSHOT),
	    {Value(StringUtil::Join(changes_made, ",")),
	     TextOrNull(!commit_info.author.IsNull(), commit_info.author.ToString()),
	     TextOrNull(!commit_info.commit_message.IsNull(), commit_info.commit_message.ToString()),
	     TextOrNull(!commit_info.commit_extra_info.IsNull(), commit_info.commit_extra_info.ToString())}));
	for (auto &statement : statements) {
		for (auto &param : ListValue::GetChildren(StructValue::GetChildren(statement)[1])) {
			// Postgres cannot store a NUL byte in text, the client-side commit reports it
			if (!param.IsNull() && (StringValue::Get(param).find('\0') != string::npos ||
			                        StringValue::Get(param).find("\\u0000") != string::npos)) {
				return {};
			}
		}
	}
	auto schema = SQLQuotedIdentifier::ToString(catalog.MetadataSchemaName());
	for (auto &statement : statements) {
		auto fields = StructValue::GetChildren(statement);
		auto query = StringValue::Get(fields[0]);
		SubstituteCatalogPlaceholders(query, schema);
		statement = Value::STRUCT({{"query", Value(query)}, {"params", fields[1]}});
	}
	return statements;
}

//! Whether the pipeline failed in a way that the client-side commit resolves: a concurrent change that the guard
//! found, or a lock, serialization or primary key conflict. A failed statement aborted the pipeline before its COMMIT
bool PostgresMetadataManager::RetriesOnClient(const string &message) const {
	for (auto sqlstate : {"23502", "55P03", "40001", "40P01"}) {
		if (message.find(StringUtil::Format("(SQLSTATE %s)", sqlstate)) != string::npos) {
			return true;
		}
	}
	return message.find("(SQLSTATE 23505)") != string::npos && IsRetryableCommitError(message);
}

bool PostgresMetadataManager::CanSkipSnapshotFetch(const TransactionChangeInformation &changes) const {
	return BooleanSetting(transaction, "ducklake_postgres_pipeline_commit") && IsDataOnlyCommit(changes) &&
	       !RequiresClientSideCommit(transaction);
}

void PostgresMetadataManager::FlushChangesServerSide(DuckLakeTransaction &flush_transaction,
                                                     DuckLakeSnapshot transaction_snapshot,
                                                     const TransactionChangeInformation &transaction_changes,
                                                     const DuckLakeRetryConfig &retry_config) {
	idx_t snapshot_statement = 0;
	auto statements = PipelineCommitStatements(transaction_snapshot, transaction_changes, snapshot_statement);
	if (statements.empty()) {
		flush_transaction.RunCommitLoop(transaction_snapshot, transaction_changes, retry_config);
		return;
	}
	auto &catalog = flush_transaction.GetCatalog();
	try {
		catalog.EnsureCommitInfoProvided(flush_transaction.GetCommitInfo());
	} catch (std::exception &) {
		// the commit never reaches the server, so its files are only removed here
		flush_transaction.CleanupFiles();
		throw;
	}
	auto catalog_literal = SQLString::ToString(catalog.MetadataDatabaseName().GetIdentifierName());
	auto result = flush_transaction.ExecuteRaw(
	    StringUtil::Format("FROM postgres_execute_pipeline(%s, ?, commit := true)", catalog_literal),
	    {Value::LIST(statements)});
	if (result->HasError()) {
		auto message = result->GetError();
		if (RetriesOnClient(message)) {
			flush_transaction.RestartMetadataTransaction();
			flush_transaction.RunCommitLoop(transaction_snapshot, transaction_changes, retry_config);
			return;
		}
		// as the client-side commit fails
		try {
			flush_transaction.GetConnection().Rollback();
		} catch (std::exception &) { // NOLINT
		}
		flush_transaction.CleanupFiles();
		ErrorData error(result->GetErrorObject());
		error.Throw("Failed to commit DuckLake transaction.\n" + CommitErrorHint(message));
	}
	optional_idx snapshot_id;
	idx_t statement = 0;
	for (auto &row : *result) {
		if (statement++ == snapshot_statement) {
			snapshot_id = std::stoull(row.GetValue<string>(1));
		}
	}
	if (!snapshot_id.IsValid()) {
		throw IOException("The pipeline commit returned no snapshot id");
	}
	catalog.SetCommittedSnapshotId(snapshot_id.GetIndex());
	catalog.AdvanceColumnsWithValues(snapshot_id.GetIndex());
	flush_transaction.ApplyServerSideCommit(transaction_snapshot.schema_version);
}

} // namespace duckdb

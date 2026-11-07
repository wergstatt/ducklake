//===----------------------------------------------------------------------===//
//                         DuckDB
//
// metadata_manager/postgres_metadata_manager.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "storage/ducklake_metadata_manager.hpp"

namespace duckdb {

class PostgresMetadataManager : public DuckLakeMetadataManager {
public:
	explicit PostgresMetadataManager(DuckLakeTransaction &transaction);

	static unique_ptr<DuckLakeMetadataManager> Create(DuckLakeTransaction &transaction) {
		return make_uniq<PostgresMetadataManager>(transaction);
	}

	bool TypeIsNativelySupported(const LogicalType &type) override;
	bool CanInlineValues(DataChunk &chunk) override;
	bool SupportsAppender() const override {
		return false;
	}
	bool DroppedTablesStayReadable() const override {
		return false;
	}
	bool ShowsConcurrentCommits() const override;
	bool InsertsInlinedRowsAfterBatch(idx_t row_count, idx_t column_count) const override;
	idx_t MaxIdentifierLength() const override {
		return 63;
	}
	idx_t MaxColumnCount() const override {
		return 1600;
	}
	vector<string> SystemColumnNames() const override {
		return {"tableoid", "xmin", "cmin", "xmax", "cmax", "ctid"};
	}

	string GetColumnTypeInternal(const LogicalType &type) override;
	string GetSnapshotAndStatsAndChangesQuery(DuckLakeSnapshot snapshot, bool include_exactness) const override;
	bool InlinedDeletionTableExists(const Identifier &table_name) override;
	void MigrateInlinedDataTypes() override;
	bool IsRetryableCommitError(const string &message) const override;
	string CommitErrorWithoutQuery(const string &message) const override;
	string ConfigOptionLockSql() const override;

	unique_ptr<QueryResult> Execute(DuckLakeSnapshot snapshot, string &query) override;
	unique_ptr<QueryResult> Execute(DuckLakeSnapshot snapshot, const DuckLakeBatch &batch) override;
	unique_ptr<QueryResult> ExecuteDropInTransaction(string &query) override;
	using DuckLakeMetadataManager::Query;
	vector<unique_ptr<QueryResult>> Query(DuckLakeSnapshot snapshot, const DuckLakeReadBatch &batch) override;
	DuckLakeCatalogInfo GetCatalogForSnapshot(DuckLakeSnapshot snapshot) override;
	vector<DuckLakeGlobalStatsInfo> GetGlobalTableStats(DuckLakeSnapshot snapshot,
	                                                    const vector<TableIndex> &table_ids) override;
	bool HoldsGlobalTableStats(DuckLakeSnapshot snapshot) override;
	using DuckLakeMetadataManager::GetSnapshot;
	//! Reads the latest snapshot together with each read that the caches miss for it, in one round trip
	unique_ptr<DuckLakeSnapshot> GetSnapshot() override;

	void ClearCache() override;
	//! postgres_scanner looks up a table it has not cached
	bool NewTablesRequireCacheClear() const override {
		return false;
	}

protected:
	string GetLatestSnapshotQuery() const override;
	string SnapshotTimeSql() const override {
		// the server's clock, as NOW() is the start of the metadata transaction
		return "clock_timestamp()";
	}
	string GenerateFileListQuery(DuckLakeTableEntry &table, DuckLakeSnapshot snapshot,
	                             const FilterPushdownInfo *filter_info,
	                             const vector<DuckLakeFileListDynamicFilter> &dynamic_filters,
	                             vector<idx_t> &runtime_filter_stats_columns, FileListType file_list_type,
	                             const string &metadata_table_prefix) override;
	bool MaterializeStatsCTEs() const override;
	string CastValueToTarget(const Value &value, const LogicalType &type) override;
	string CastStatsToTarget(const string &stats, const LogicalType &type, StatsCastType cast_type) override;

private:
	//! Where the server error starts in the error of the last Execute
	optional_idx BatchServerErrorStart(const string &message) const;
	//! Runs statements in one round trip, with their values as parameters, keeping the results of reads if asked
	unique_ptr<QueryResult> ExecutePipeline(DuckLakeSnapshot snapshot, const vector<DuckLakeStatement> &batch,
	                                        bool keep_results = false);
	//! Runs the reads in one round trip, and returns a query that scans the kept result of each
	vector<string> KeepResults(DuckLakeSnapshot snapshot, const DuckLakeReadBatch &batch);
	//! Builds the catalog from the kept results of the reads of its tables
	DuckLakeCatalogInfo BuildCatalog(DuckLakeSnapshot snapshot, const vector<string> &tables,
	                                 const vector<string> &scans);
	//! The query that lists the inlined deletion tables
	string InlinedDeletionTablesQuery() const;

	//! The queries of the last Execute, one of which the scanner repeats in front of a server error
	vector<string> executed_queries;
	//! The stats of every table are read with the snapshot for a catalog of at most this many tables
	static constexpr idx_t MAX_TABLES_WITH_STATS_READ_AHEAD = 100;
	mutex read_ahead_lock;
	//! The catalog read with the snapshot, until it is loaded, and its schema version
	unique_ptr<pair<idx_t, DuckLakeCatalogInfo>> read_ahead_catalog;
	//! The stats of every table read with the snapshot, and its snapshot id
	unique_ptr<pair<idx_t, vector<DuckLakeGlobalStatsInfo>>> read_ahead_stats;
};

} // namespace duckdb

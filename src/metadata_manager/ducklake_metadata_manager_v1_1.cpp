#include "metadata_manager/ducklake_metadata_manager_v1_1.hpp"
#include "metadata_manager/postgres_metadata_manager.hpp"
#include "metadata_manager/quack_metadata_manager.hpp"
#include "common/ducklake_version.hpp"

namespace duckdb {

template <typename Base>
DuckLakeVersion DuckLakeMetadataManagerV1_1<Base>::GetVersion() {
	return DuckLakeVersion::V1_1;
}

// explicit instantiations for all backends
template class DuckLakeMetadataManagerV1_1<DuckLakeMetadataManager>;
template class DuckLakeMetadataManagerV1_1<PostgresMetadataManager>;
template class DuckLakeMetadataManagerV1_1<QuackMetadataManager>;

} // namespace duckdb

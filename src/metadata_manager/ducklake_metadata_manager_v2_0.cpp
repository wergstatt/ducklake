#include "metadata_manager/ducklake_metadata_manager_v2_0.hpp"
#include "metadata_manager/postgres_metadata_manager.hpp"
#include "metadata_manager/quack_metadata_manager.hpp"
#include "common/ducklake_version.hpp"

namespace duckdb {

template <typename Base>
DuckLakeVersion DuckLakeMetadataManagerV2_0<Base>::GetVersion() {
	return DuckLakeVersion::V2_0_DEV1;
}

// explicit instantiations for all backends
template class DuckLakeMetadataManagerV2_0<DuckLakeMetadataManager>;
template class DuckLakeMetadataManagerV2_0<PostgresMetadataManager>;
template class DuckLakeMetadataManagerV2_0<QuackMetadataManager>;

} // namespace duckdb

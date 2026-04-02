#pragma once

#include <arrow/api.h>
#include <memory>
#include <string>
#include <vector>

namespace charmvz::schema {

/**
 * Returns the formal Arrow schema for the ProcessingElement entity.
 */
auto processing_element() -> std::shared_ptr<arrow::Schema>;

/**
 * Returns the formal Arrow schema for the ChareCollection entity.
 */
auto chare_collection() -> std::shared_ptr<arrow::Schema>;

/**
 * Returns the formal Arrow schema for the EntryMethod entity.
 */
auto entry_method() -> std::shared_ptr<arrow::Schema>;

/**
 * Returns the formal Arrow schema for the ChareInstance entity.
 */
auto chare_instance() -> std::shared_ptr<arrow::Schema>;

/**
 * Returns the formal Arrow schema for the Execution entity.
 */
auto execution(const std::vector<std::string>& papi_event_names = {}) -> std::shared_ptr<arrow::Schema>;

/**
 * Returns the formal Arrow schema for the Message entity.
 */
auto message() -> std::shared_ptr<arrow::Schema>;

/**
 * Returns the formal Arrow schema for the IdleInterval entity.
 */
auto idle_interval() -> std::shared_ptr<arrow::Schema>;

/**
 * Returns the formal Arrow schema for the MigrationEpisode entity.
 */
auto migration_episode() -> std::shared_ptr<arrow::Schema>;

} // namespace charmvz::schema

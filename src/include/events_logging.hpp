#pragma once

#include "duckdb/logging/logging.hpp"
#include "duckdb/logging/log_type.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {

struct EventsLogType : public LogType {
	static constexpr const char *NAME = "Events";
	static constexpr LogLevel LEVEL = LogLevel::LOG_INFO;

	//! Construct the log type
	EventsLogType();

	static LogicalType GetLogType();

	static string ConstructLogMessage(const string &event, const vector<pair<string, string>> &info);
};
} // namespace duckdb

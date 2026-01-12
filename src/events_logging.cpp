#include "events_logging.hpp"
#include "duckdb.hpp"

namespace duckdb {

constexpr LogLevel EventsLogType::LEVEL;

EventsLogType::EventsLogType() : LogType(NAME, LEVEL, GetLogType()) {
}

template <class ITERABLE>
static Value StringPairIterableToMap(const ITERABLE &iterable) {
	vector<Value> keys;
	vector<Value> values;
	for (const auto &kv : iterable) {
		keys.emplace_back(kv.first);
		values.emplace_back(kv.second);
	}
	return Value::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR, std::move(keys), std::move(values));
}

LogicalType EventsLogType::GetLogType() {
	child_list_t<LogicalType> child_list = {
	    {"event", LogicalType::VARCHAR},
	    {"info", LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR)},
	};
	return LogicalType::STRUCT(child_list);
}

string EventsLogType::ConstructLogMessage(const string &event, const vector<pair<string, string>> &info) {
	child_list_t<Value> child_list = {
	    {"event", event},
	    {"info", StringPairIterableToMap(info)},
	};

	return Value::STRUCT(std::move(child_list)).ToString();
}

} // namespace duckdb

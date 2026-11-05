#define DUCKDB_EXTENSION_MAIN

#include "events_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/identifier.hpp"
#include "duckdb/planner/extension_callback.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/prepared_statement_data.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/connection_manager.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/logging/logger.hpp"
#include "duckdb/logging/log_manager.hpp"
#include "duckdb/transaction/meta_transaction.hpp"
#include "yyjson.hpp"

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <string>

#include "events_logging.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#endif

#include "query_farm_telemetry.hpp"

#define EVENTS_VERSION "2026110501"

using namespace duckdb_yyjson; // NOLINT

namespace duckdb {

// Keys for storing settings in client config set_variables
static const Identifier EVENTS_DESTINATION_KEY = "events_destination";
static const Identifier EVENTS_ASYNC_KEY = "events_async";
static const Identifier EVENTS_TYPES_KEY = "events_types";
static const Identifier EVENTS_SESSION_NAME_KEY = "events_session_name";

// Get the events destination from context
static Value GetEventsDestination(ClientContext &context) {
	Value destination;
	context.TryGetCurrentSetting(EVENTS_DESTINATION_KEY, destination);
	return destination;
}

// Get whether events should be delivered asynchronously (default: false = synchronous)
static bool GetEventsAsync(ClientContext &context) {
	Value async_value;
	context.TryGetCurrentSetting(EVENTS_ASYNC_KEY, async_value);
	if (async_value.IsNull()) {
		return false; // Default to synchronous
	}
	return async_value.GetValue<bool>();
}

// Get the session name from context (default: empty string)
static string GetEventsSessionName(ClientContext &context) {
	Value session_name;
	context.TryGetCurrentSetting(EVENTS_SESSION_NAME_KEY, session_name);
	if (session_name.IsNull()) {
		return "";
	}
	return session_name.GetValue<string>();
}

// Check if an event type is enabled
static bool IsEventTypeEnabled(ClientContext &context, const string &event_type) {
	Value types_value;
	context.TryGetCurrentSetting(EVENTS_TYPES_KEY, types_value);
	if (types_value.IsNull()) {
		// Default: only query_begin and query_end
		return event_type == "query_begin" || event_type == "query_end";
	}

	// Check if event_type is in the list
	auto &list_children = ListValue::GetChildren(types_value);
	for (auto &child : list_children) {
		if (!child.IsNull() && child.GetValue<string>() == event_type) {
			return true;
		}
	}
	return false;
}

// Get current timestamp as ISO 8601 string with zero-padded milliseconds
static string GetCurrentTimestamp() {
	auto now = std::chrono::system_clock::now();
	auto time_t_now = std::chrono::system_clock::to_time_t(now);
	auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
	char buffer[32];
	std::tm utc_time;
#ifdef _WIN32
	gmtime_s(&utc_time, &time_t_now);
#else
	gmtime_r(&time_t_now, &utc_time);
#endif
	std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &utc_time);
	char ms_buffer[8];
	snprintf(ms_buffer, sizeof(ms_buffer), ".%03d", static_cast<int>(ms.count()));
	return string(buffer) + ms_buffer + "Z";
}

// Get database path from context
static string GetDatabasePath(ClientContext &context) {
	auto &db_config = DBConfig::GetConfig(context);
	if (db_config.options.database_path.empty()) {
		return ":memory:";
	}
	return db_config.options.database_path;
}

// Parse a command line string into tokens, handling quotes
static vector<string> ParseCommandLine(const string &cmd) {
	vector<string> tokens;
	string current;
	bool in_single_quote = false;
	bool in_double_quote = false;
	bool escaped = false;
	bool token_started = false;

	for (size_t i = 0; i < cmd.size(); i++) {
		char c = cmd[i];

		if (escaped) {
			current += c;
			escaped = false;
			continue;
		}

		if (c == '\\' && !in_single_quote) {
			escaped = true;
			token_started = true;
			continue;
		}

		if (c == '\'' && !in_double_quote) {
			in_single_quote = !in_single_quote;
			token_started = true;
			continue;
		}

		if (c == '"' && !in_single_quote) {
			in_double_quote = !in_double_quote;
			token_started = true;
			continue;
		}

		if (std::isspace(static_cast<unsigned char>(c)) && !in_single_quote && !in_double_quote) {
			if (token_started) {
				tokens.push_back(current);
				current.clear();
				token_started = false;
			}
			continue;
		}

		current += c;
		token_started = true;
	}

	if (escaped || in_single_quote || in_double_quote) {
		throw InvalidInputException("events_destination contains an unfinished quote or escape");
	}
	if (token_started) {
		tokens.push_back(current);
	}

	return tokens;
}

static void ValidateDestination(ClientContext &context, SetScope scope, Value &value) {
	if (value.IsNull() || value.GetValue<string>().empty()) {
		return;
	}
	if (!Settings::Get<EnableExternalAccessSetting>(context)) {
		throw PermissionException("events_destination requires enable_external_access=true");
	}
	auto destination = value.GetValue<string>();
	if (destination.find('\0') != string::npos) {
		throw InvalidInputException("events_destination cannot contain NUL bytes");
	}
#ifndef _WIN32
	auto tokens = ParseCommandLine(destination);
	if (tokens.empty() || tokens[0].empty()) {
		throw InvalidInputException("events_destination must name an executable");
	}
#endif
}

static void ValidateEventTypes(ClientContext &context, SetScope scope, Value &value) {
	if (value.IsNull()) {
		return;
	}
	static const vector<string> supported = {
	    "connection_opened",        "connection_closed",    "query_begin",    "query_end",        "transaction_begin",
	    "transaction_commit",       "transaction_rollback", "planning_error", "finalize_prepare", "execute_prepared",
	    "rebind_prepared_statement"};
	for (auto &type : ListValue::GetChildren(value)) {
		if (type.IsNull()) {
			throw InvalidInputException("events_types cannot contain NULL entries");
		}
		auto name = type.GetValue<string>();
		if (std::find(supported.begin(), supported.end(), name) == supported.end()) {
			throw InvalidInputException("Unknown event type '%s' in events_types", name);
		}
	}
}

#ifndef _WIN32
// A socket pair supplies stdin as a byte stream and lets us suppress SIGPIPE
// per descriptor/send, without changing the embedding application's signals.
static bool WriteEvent(int fd, const string &data) {
#ifdef MSG_NOSIGNAL
	constexpr int flags = MSG_NOSIGNAL;
#else
	constexpr int flags = 0; // SO_NOSIGPIPE is set on the socket below (macOS/BSD).
#endif
	size_t offset = 0;
	while (offset < data.size()) {
		auto count = send(fd, data.data() + offset, data.size() - offset, flags);
		if (count < 0 && errno == EINTR) {
			continue;
		}
		if (count <= 0) {
			return false;
		}
		offset += static_cast<size_t>(count);
	}
	return true;
}

static int WaitForEventProcess(pid_t pid) {
	int status;
	pid_t result;
	do {
		result = waitpid(pid, &status, 0);
	} while (result < 0 && errno == EINTR);
	return result == pid && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
#endif

// Call external program with JSON input via stdin and log exit code
static void CallEventsDestination(ClientContext &context, const string &event_type, const string &json_data) {
	if (json_data.empty()) {
		return;
	}
	// Check if this event type is enabled
	if (!IsEventTypeEnabled(context, event_type)) {
		return;
	}

	auto destination_value = GetEventsDestination(context);
	if (destination_value.IsNull()) {
		return; // No destination configured, silently skip
	}
	string destination = destination_value.GetValue<string>();
	if (destination.empty()) {
		return; // Empty destination, silently skip
	}
	bool async_mode = GetEventsAsync(context);
	string json_with_newline = json_data + "\n";

#ifdef _WIN32
	// Windows implementation using CreateProcess
	SECURITY_ATTRIBUTES sa;
	sa.nLength = sizeof(SECURITY_ATTRIBUTES);
	sa.bInheritHandle = TRUE;
	sa.lpSecurityDescriptor = NULL;

	HANDLE stdin_read, stdin_write;
	if (!CreatePipe(&stdin_read, &stdin_write, &sa, 0)) {
		DUCKDB_LOG(context, EventsLogType, "Failed to create pipe for events destination", {});
		return;
	}

	if (!SetHandleInformation(stdin_write, HANDLE_FLAG_INHERIT, 0)) {
		CloseHandle(stdin_read);
		CloseHandle(stdin_write);
		DUCKDB_LOG(context, EventsLogType, "Failed to configure events pipe", {});
		return;
	}

	STARTUPINFOA si;
	PROCESS_INFORMATION pi;
	ZeroMemory(&si, sizeof(si));
	si.cb = sizeof(si);
	si.hStdInput = stdin_read;
	si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
	si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
	si.dwFlags |= STARTF_USESTDHANDLES;
	ZeroMemory(&pi, sizeof(pi));

	string cmd = destination;
	if (!CreateProcessA(NULL, &cmd[0], NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
		DUCKDB_LOG(context, EventsLogType, "Failed to start events destination", {{"destination", destination}});
		CloseHandle(stdin_read);
		CloseHandle(stdin_write);
		return;
	}

	CloseHandle(stdin_read);

	// Write JSON with trailing newline for JSONL format
	size_t offset = 0;
	while (offset < json_with_newline.size()) {
		DWORD written = 0;
		auto remaining = json_with_newline.size() - offset;
		auto count = static_cast<DWORD>(remaining > MAXDWORD ? MAXDWORD : remaining);
		if (!WriteFile(stdin_write, json_with_newline.data() + offset, count, &written, NULL) || written == 0) {
			DUCKDB_LOG(context, EventsLogType, "Failed to write events destination", {{"destination", destination}});
			break;
		}
		offset += written;
	}
	CloseHandle(stdin_write);

	if (async_mode) {
		// Async mode: don't wait for process, just close handles and return
		DUCKDB_LOG(context, EventsLogType, "Events destination started (async)",
		           {{"destination", destination}, {"pid", std::to_string(pi.dwProcessId)}});
		CloseHandle(pi.hProcess);
		CloseHandle(pi.hThread);
	} else {
		// Sync mode: wait for process to complete
		WaitForSingleObject(pi.hProcess, INFINITE);

		DWORD exit_code = static_cast<DWORD>(-1);
		GetExitCodeProcess(pi.hProcess, &exit_code);

		DUCKDB_LOG(context, EventsLogType, "Events destination exited",
		           {{"destination", destination}, {"exit_code", std::to_string((int)exit_code)}});

		CloseHandle(pi.hProcess);
		CloseHandle(pi.hThread);
	}
#else
	// Prepare all C++ allocations before fork: another DuckDB thread may hold
	// an allocator lock in the child, where only async-signal-safe calls are safe.
	auto tokens = ParseCommandLine(destination);
	if (tokens.empty() || tokens[0].empty()) {
		return;
	}
	vector<char *> argv;
	for (auto &token : tokens) {
		argv.push_back(&token[0]);
	}
	argv.push_back(nullptr);
	// POSIX implementation using fork/exec
	int stdin_pipe[2];
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, stdin_pipe) == -1) {
		DUCKDB_LOG(context, EventsLogType, "Failed to create pipe for events destination", {});
		return;
	}
	if (fcntl(stdin_pipe[0], F_SETFD, FD_CLOEXEC) == -1 || fcntl(stdin_pipe[1], F_SETFD, FD_CLOEXEC) == -1) {
		close(stdin_pipe[0]);
		close(stdin_pipe[1]);
		DUCKDB_LOG(context, EventsLogType, "Failed to configure events pipe", {});
		return;
	}

#ifdef SO_NOSIGPIPE
	int no_sigpipe = 1;
	if (setsockopt(stdin_pipe[1], SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe)) == -1) {
		close(stdin_pipe[0]);
		close(stdin_pipe[1]);
		DUCKDB_LOG(context, EventsLogType, "Failed to configure events pipe", {});
		return;
	}
#endif

	pid_t pid = fork();
	if (pid == -1) {
		DUCKDB_LOG(context, EventsLogType, "Failed to fork for events destination", {});
		close(stdin_pipe[0]);
		close(stdin_pipe[1]);
		return;
	}

	if (pid == 0) {
		// Child process
		close(stdin_pipe[1]); // Close write end

		if (async_mode) {
			// Double-fork to detach: grandchild will be adopted by init, avoiding zombies
			pid_t grandchild = fork();
			if (grandchild == -1) {
				_exit(127);
			}
			if (grandchild > 0) {
				// Intermediate child exits immediately
				_exit(0);
			}
			// Grandchild continues to exec
		}

		if (stdin_pipe[0] == STDIN_FILENO) {
			if (fcntl(STDIN_FILENO, F_SETFD, 0) == -1) {
				_exit(127);
			}
		} else if (dup2(stdin_pipe[0], STDIN_FILENO) == -1) {
			_exit(127);
		} else {
			close(stdin_pipe[0]);
		}

		execvp(argv[0], argv.data());
		_exit(127); // exec failed
	}

	// Parent process
	close(stdin_pipe[0]); // Close read end

	// Write JSON to child's stdin with trailing newline for JSONL format
	if (!WriteEvent(stdin_pipe[1], json_with_newline)) {
		DUCKDB_LOG(context, EventsLogType, "Failed to write events destination", {{"destination", destination}});
	}
	close(stdin_pipe[1]); // Close to signal EOF

	if (async_mode) {
		// Wait for intermediate child (exits immediately after double-fork)
		// Grandchild is adopted by init and won't become a zombie
		WaitForEventProcess(pid);
		DUCKDB_LOG(context, EventsLogType, "Events destination started (async)", {{"destination", destination}});
	} else {
		// Sync mode: wait for child and get exit status
		int exit_code = WaitForEventProcess(pid);
		DUCKDB_LOG(context, EventsLogType, "Events destination exited",
		           {{"destination", destination}, {"exit_code", std::to_string(exit_code)}});
	}
#endif
}

// Get the current process ID
static uint64_t GetProcessId() {
#ifdef _WIN32
	return static_cast<uint64_t>(GetCurrentProcessId());
#else
	return static_cast<uint64_t>(getpid());
#endif
}

// Helper to add common fields to all events
static void AddCommonFields(yyjson_mut_doc *doc, yyjson_mut_val *root, ClientContext &context) {
	yyjson_mut_obj_add_strcpy(doc, root, "timestamp", GetCurrentTimestamp().c_str());
	yyjson_mut_obj_add_strcpy(doc, root, "database_path", GetDatabasePath(context).c_str());
	yyjson_mut_obj_add_strcpy(doc, root, "session_name", GetEventsSessionName(context).c_str());
	yyjson_mut_obj_add_uint(doc, root, "connection_id", context.GetConnectionId());
	yyjson_mut_obj_add_uint(doc, root, "process_id", GetProcessId());
}

// Helper to add transaction information
static void AddTransactionInfo(yyjson_mut_doc *doc, yyjson_mut_val *root, MetaTransaction &transaction) {
	yyjson_mut_obj_add_uint(doc, root, "transaction_id", transaction.global_transaction_id);
	yyjson_mut_obj_add_int(doc, root, "start_timestamp", static_cast<int64_t>(transaction.start_timestamp.value));
	yyjson_mut_obj_add_bool(doc, root, "is_read_only", transaction.IsReadOnly());
}

// Helper to add error information
static void AddErrorInfo(yyjson_mut_doc *doc, yyjson_mut_val *root, optional_ptr<ErrorData> error) {
	if (error) {
		yyjson_mut_obj_add_bool(doc, root, "has_error", true);
		yyjson_mut_obj_add_strcpy(doc, root, "error_message", error->Message().c_str());
		yyjson_mut_obj_add_strcpy(doc, root, "error_type", Exception::ExceptionTypeToString(error->Type()).c_str());
	} else {
		yyjson_mut_obj_add_bool(doc, root, "has_error", false);
	}
}

// Helper to get current transaction ID (0 if no active transaction)
static uint64_t GetCurrentTransactionId(ClientContext &context) {
	if (context.transaction.HasActiveTransaction()) {
		auto &transaction = MetaTransaction::Get(context);
		return transaction.global_transaction_id;
	}
	return 0;
}

// Helper to add named parameters to JSON object
template <typename T>
static void AddParameters(yyjson_mut_doc *doc, yyjson_mut_val *root, T &parameters) {
	if (!parameters) {
		return;
	}
	yyjson_mut_val *params = yyjson_mut_obj(doc);
	for (auto &kv : *parameters) {
		auto &param_name = kv.first;
		auto &param_data = kv.second;
		yyjson_mut_obj_add_strcpy(doc, params, param_name.c_str(), param_data.GetValue().ToString().c_str());
	}
	yyjson_mut_obj_add_val(doc, root, "parameters", params);
}

// Helper to add attached databases information
static void AddAttachedDatabases(yyjson_mut_doc *doc, yyjson_mut_val *root, ClientContext &context) {
	auto &db_manager = DatabaseManager::Get(context);
	auto databases = db_manager.GetDatabases(context);

	yyjson_mut_val *dbs_array = yyjson_mut_arr(doc);

	for (auto &db : databases) {
		// Skip system database
		if (db->IsSystem()) {
			continue;
		}

		yyjson_mut_val *db_obj = yyjson_mut_obj(doc);
		yyjson_mut_obj_add_strcpy(doc, db_obj, "name", db->GetName().c_str());
		yyjson_mut_obj_add_strcpy(doc, db_obj, "path", db->StoredPath().c_str());

		// Get database type from catalog
		auto &catalog = db->GetCatalog();
		yyjson_mut_obj_add_strcpy(doc, db_obj, "type", catalog.GetCatalogType().c_str());

		yyjson_mut_obj_add_bool(doc, db_obj, "read_only", db->IsReadOnly());
		yyjson_mut_obj_add_bool(doc, db_obj, "temporary", db->IsTemporary());

		yyjson_mut_arr_append(dbs_array, db_obj);
	}

	yyjson_mut_obj_add_val(doc, root, "attached_databases", dbs_array);
}

// Helper to build JSON for an event
static string BuildEventJson(const char *event_type, ClientContext &context,
                             std::function<void(yyjson_mut_doc *, yyjson_mut_val *)> add_fields = nullptr) {
	// Avoid serializing or enumerating databases when this event has no receiver.
	if (!Settings::Get<EnableExternalAccessSetting>(context) || !IsEventTypeEnabled(context, event_type)) {
		return "";
	}
	auto destination = GetEventsDestination(context);
	if (destination.IsNull() || destination.GetValue<string>().empty()) {
		return "";
	}
	std::unique_ptr<yyjson_mut_doc, decltype(&yyjson_mut_doc_free)> owner(yyjson_mut_doc_new(nullptr),
	                                                                      yyjson_mut_doc_free);
	auto doc = owner.get();
	if (!doc) {
		throw OutOfMemoryException("Failed to allocate event JSON");
	}
	yyjson_mut_val *root = yyjson_mut_obj(doc);
	yyjson_mut_doc_set_root(doc, root);

	yyjson_mut_obj_add_str(doc, root, "event", event_type);
	AddCommonFields(doc, root, context);

	if (add_fields) {
		add_fields(doc, root);
	}

	std::unique_ptr<char, decltype(&free)> json_str(yyjson_mut_write(doc, 0, nullptr), free);
	if (!json_str) {
		throw SerializationException("Failed to serialize event JSON");
	}
	return string(json_str.get());
}

class EventsHook : public ClientContextState {
public:
	explicit EventsHook(ClientContext &context) : context(context) {
	}

	bool CanRequestRebind() override {
		// DuckDB only dispatches planning/finalize callbacks to states when at
		// least one opts in. Our callbacks observe but never request a rebind.
		auto destination = GetEventsDestination(context);
		return !destination.IsNull() && !destination.GetValue<string>().empty() &&
		       Settings::Get<EnableExternalAccessSetting>(context) &&
		       (IsEventTypeEnabled(context, "planning_error") || IsEventTypeEnabled(context, "finalize_prepare"));
	}

private:
	ClientContext &context;

public:
	// Query ID counter - incremented for each new query
	uint64_t query_id_counter = 0;
	// Current query ID - set in QueryBegin, used in QueryEnd
	uint64_t current_query_id = 0;

	void QueryBegin(ClientContext &context) override {
		current_query_id = ++query_id_counter;
		uint64_t qid = current_query_id;
		uint64_t txn_id = GetCurrentTransactionId(context);

		string json =
		    BuildEventJson("query_begin", context, [qid, txn_id, &context](yyjson_mut_doc *doc, yyjson_mut_val *root) {
			    yyjson_mut_obj_add_uint(doc, root, "query_id", qid);
			    yyjson_mut_obj_add_uint(doc, root, "transaction_id", txn_id);
			    AddAttachedDatabases(doc, root, context);
		    });
		CallEventsDestination(context, "query_begin", json);
	}

	void QueryEnd(ClientContext &context) override {
		QueryEnd(context, nullptr);
	}

	void QueryEnd(ClientContext &context, optional_ptr<ErrorData> error) override {
		uint64_t qid = current_query_id;
		uint64_t txn_id = GetCurrentTransactionId(context);

		string json = BuildEventJson("query_end", context,
		                             [qid, txn_id, &context, &error](yyjson_mut_doc *doc, yyjson_mut_val *root) {
			                             yyjson_mut_obj_add_uint(doc, root, "query_id", qid);
			                             yyjson_mut_obj_add_uint(doc, root, "transaction_id", txn_id);
			                             AddErrorInfo(doc, root, error);
			                             AddAttachedDatabases(doc, root, context);
		                             });
		CallEventsDestination(context, "query_end", json);
	}

	void TransactionBegin(MetaTransaction &transaction, ClientContext &context) override {
		string json = BuildEventJson("transaction_begin", context, [&](yyjson_mut_doc *doc, yyjson_mut_val *root) {
			AddTransactionInfo(doc, root, transaction);
		});
		CallEventsDestination(context, "transaction_begin", json);
	}

	void TransactionCommit(MetaTransaction &transaction, ClientContext &context) override {
		string json = BuildEventJson("transaction_commit", context, [&](yyjson_mut_doc *doc, yyjson_mut_val *root) {
			AddTransactionInfo(doc, root, transaction);
		});
		CallEventsDestination(context, "transaction_commit", json);
	}

	void TransactionRollback(MetaTransaction &transaction, ClientContext &context) override {
		TransactionRollback(transaction, context, nullptr);
	}

	void TransactionRollback(MetaTransaction &transaction, ClientContext &context,
	                         optional_ptr<ErrorData> error) override {
		string json = BuildEventJson("transaction_rollback", context, [&](yyjson_mut_doc *doc, yyjson_mut_val *root) {
			AddTransactionInfo(doc, root, transaction);
			AddErrorInfo(doc, root, error);
		});
		CallEventsDestination(context, "transaction_rollback", json);
	}

	RebindQueryInfo OnPlanningError(ClientContext &context, SQLStatement &statement, ErrorData &error) override {
		string json = BuildEventJson("planning_error", context, [&](yyjson_mut_doc *doc, yyjson_mut_val *root) {
			yyjson_mut_obj_add_strcpy(doc, root, "error_message", error.Message().c_str());
			yyjson_mut_obj_add_strcpy(doc, root, "error_type", Exception::ExceptionTypeToString(error.Type()).c_str());
			yyjson_mut_obj_add_strcpy(doc, root, "statement_type", StatementTypeToString(statement.type).c_str());
		});
		CallEventsDestination(context, "planning_error", json);
		return RebindQueryInfo::DO_NOT_REBIND;
	}

	RebindQueryInfo OnFinalizePrepare(ClientContext &context, PreparedStatementData &prepared_statement,
	                                  PreparedStatementMode mode) override {
		string json = BuildEventJson("finalize_prepare", context, [&](yyjson_mut_doc *doc, yyjson_mut_val *root) {
			yyjson_mut_obj_add_strcpy(doc, root, "statement_type",
			                          StatementTypeToString(prepared_statement.statement_type).c_str());
		});
		CallEventsDestination(context, "finalize_prepare", json);
		return RebindQueryInfo::DO_NOT_REBIND;
	}

	void EmitPreparedEvent(const char *event_type, ClientContext &context, BindPreparedStatementCallbackInfo &info) {
		uint64_t qid = current_query_id;
		uint64_t txn_id = GetCurrentTransactionId(context);

		string json = BuildEventJson(event_type, context, [&](yyjson_mut_doc *doc, yyjson_mut_val *root) {
			yyjson_mut_obj_add_uint(doc, root, "query_id", qid);
			yyjson_mut_obj_add_uint(doc, root, "transaction_id", txn_id);
			yyjson_mut_obj_add_strcpy(doc, root, "statement_type",
			                          StatementTypeToString(info.prepared_statement.statement_type).c_str());
			AddParameters(doc, root, info.parameters);
		});
		CallEventsDestination(context, event_type, json);
	}

	RebindQueryInfo OnRebindPreparedStatement(ClientContext &context, BindPreparedStatementCallbackInfo &info,
	                                          RebindQueryInfo current_rebind) override {
		// DuckDB 2.0 removed OnExecutePrepared. SQL EXECUTE dispatches this
		// callback with bound parameters before executing the prepared statement.
		EmitPreparedEvent("execute_prepared", context, info);
		EmitPreparedEvent("rebind_prepared_statement", context, info);
		return RebindQueryInfo::DO_NOT_REBIND;
	}
};

// Helper to register EventsHook for a connection if not already registered
static void RegisterEventsHookIfNeeded(ClientContext &context) {
	auto &register_state = context.registered_state;
	if (!register_state->Get<EventsHook>("events_hook_state")) {
		auto state = make_shared_ptr<EventsHook>(context);
		register_state->Insert("events_hook_state", state);
	}
}

class EventHooks : public ExtensionCallback {
public:
	void OnConnectionOpened(ClientContext &context) override;
	void OnConnectionClosed(ClientContext &context) override;
};

void EventHooks::OnConnectionOpened(ClientContext &context) {
	RegisterEventsHookIfNeeded(context);

	string json = BuildEventJson("connection_opened", context, nullptr);
	CallEventsDestination(context, "connection_opened", json);
}

void EventHooks::OnConnectionClosed(ClientContext &context) {
	string json = BuildEventJson("connection_closed", context, nullptr);
	CallEventsDestination(context, "connection_closed", json);
}

static void LoadInternal(ExtensionLoader &loader) {
	auto &db = loader.GetDatabaseInstance();
	auto &config = DBConfig::GetConfig(db);

	auto &log_manager = loader.GetDatabaseInstance().GetLogManager();
	log_manager.RegisterLogType(make_uniq<EventsLogType>());

	// Register the events_destination setting
	config.AddExtensionOption(EVENTS_DESTINATION_KEY, "Path to the program that receives event notifications via stdin",
	                          LogicalType::VARCHAR, Value(), ValidateDestination);

	// Register the events_async setting (default: false = synchronous)
	config.AddExtensionOption(
	    EVENTS_ASYNC_KEY,
	    "If true, events are delivered asynchronously (fire and forget). Default is false (synchronous).",
	    LogicalType::BOOLEAN, Value(false));

	// Register the events_types setting (default: query_begin and query_end)
	vector<Value> default_types = {Value("query_begin"), Value("query_end")};
	config.AddExtensionOption(
	    EVENTS_TYPES_KEY,
	    "List of event types to send to the destination. Valid types: connection_opened, connection_closed, "
	    "query_begin, query_end, transaction_begin, transaction_commit, transaction_rollback, "
	    "planning_error, finalize_prepare, execute_prepared, rebind_prepared_statement",
	    LogicalType::LIST(LogicalType::VARCHAR), Value::LIST(LogicalType::VARCHAR, default_types), ValidateEventTypes);

	// Register the events_session_name setting (default: empty string)
	config.AddExtensionOption(EVENTS_SESSION_NAME_KEY, "Optional name for this session, included in all events",
	                          LogicalType::VARCHAR, Value(""));

	// Register hooks for connection events (for new connections)
	ExtensionCallback::Register(config, make_shared_ptr<EventHooks>());

	// Also register hooks for existing connections (e.g., when loaded via LOAD or require in tests)
	auto &connection_manager = db.GetConnectionManager();
	for (auto &context : connection_manager.GetConnectionList()) {
		RegisterEventsHookIfNeeded(*context);
	}

	QueryFarmSendTelemetry(loader, "events", EVENTS_VERSION);
}

void EventsExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string EventsExtension::Name() {
	return "events";
}

std::string EventsExtension::Version() const {
	return EVENTS_VERSION;
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(events, loader) {
	duckdb::LoadInternal(loader);
}
}

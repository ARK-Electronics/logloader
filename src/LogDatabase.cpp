#include "LogDatabase.hpp"
#include "Config.hpp"
#include "Log.hpp"
#include "Sqlite.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <sstream>

namespace fs = std::filesystem;

namespace
{

// Every SELECT reads the same columns in the same order, so row_to_entry only
// has to be written once.
const std::string kLogColumns =
	"l.id, l.path, l.size_bytes, l.time_utc, l.local_path, l.downloaded, "
	"l.download_requested, l.present, l.download_failures, l.last_error, l.discovered_at";

// Newest first. Logs whose time the vehicle cannot report sort after the ones
// it can, and fall back to the path: ArduPilot names logs with a bare counter,
// so the longer name is the later one (100.BIN comes after 99.BIN).
const std::string kNewestFirst =
	"(l.time_utc IS NULL) ASC, l.time_utc DESC, LENGTH(l.path) DESC, l.path DESC";

int64_t now_epoch()
{
	return static_cast<int64_t>(std::time(nullptr));
}

std::optional<int64_t> parse_iso8601_utc(const std::string& text)
{
	std::tm tm {};

	if (sscanf(text.c_str(), "%4d-%2d-%2dT%2d:%2d:%2dZ",
		   &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6) {
		return std::nullopt;
	}

	tm.tm_year -= 1900;
	tm.tm_mon -= 1;

	return static_cast<int64_t>(timegm(&tm));
}

LogDatabase::Entry row_to_entry(const sqlite::Statement& stmt)
{
	LogDatabase::Entry entry;
	entry.id = stmt.column_int(0);
	entry.path = stmt.column_text(1);
	entry.size_bytes = static_cast<uint32_t>(stmt.column_int(2));
	entry.time_utc = stmt.column_optional_int(3);
	entry.local_path = stmt.column_text(4);
	entry.downloaded = stmt.column_int(5) != 0;
	entry.download_requested = stmt.column_int(6) != 0;
	entry.present = stmt.column_int(7) != 0;
	entry.download_failures = static_cast<int>(stmt.column_int(8));
	entry.last_error = stmt.column_text(9);
	entry.discovered_at = stmt.column_int(10);
	return entry;
}

} // namespace

LogDatabase::LogDatabase(const std::string& db_path, const std::string& logs_directory,
			 const std::vector<std::string>& targets)
	: _logs_directory(logs_directory)
	, _targets(targets)
{
	if (!initialize(db_path)) {
		LOG_ERROR("Could not open the log database at " << db_path);
		return;
	}

	import_legacy_databases(db_path);
	_has_legacy_rows = sqlite::table_exists(_db, "legacy_logs");
}

LogDatabase::~LogDatabase()
{
	if (_db != nullptr) {
		sqlite3_close(_db);
	}
}

bool LogDatabase::initialize(const std::string& db_path)
{
	if (sqlite3_open(db_path.c_str(), &_db) != SQLITE_OK) {
		LOG_ERROR("Cannot open database: " << sqlite3_errmsg(_db));
		sqlite3_close(_db);
		_db = nullptr;
		return false;
	}

	// WAL lets the API server read while the download loop writes.
	sqlite::execute(_db, "PRAGMA journal_mode = WAL");
	sqlite::execute(_db, "PRAGMA foreign_keys = ON");
	sqlite::execute(_db, "PRAGMA busy_timeout = 5000");

	return create_schema();
}

bool LogDatabase::create_schema()
{
	const bool logs = sqlite::execute(_db,
					  "CREATE TABLE IF NOT EXISTS logs ("
					  "  id                 INTEGER PRIMARY KEY,"
					  "  path               TEXT    NOT NULL,"
					  "  size_bytes         INTEGER NOT NULL,"
					  "  time_utc           INTEGER,"
					  "  local_path         TEXT    NOT NULL DEFAULT '',"
					  "  downloaded         INTEGER NOT NULL DEFAULT 0,"
					  "  download_requested INTEGER NOT NULL DEFAULT 0,"
					  "  present            INTEGER NOT NULL DEFAULT 1,"
					  "  download_failures  INTEGER NOT NULL DEFAULT 0,"
					  "  last_error         TEXT    NOT NULL DEFAULT '',"
					  "  discovered_at      INTEGER NOT NULL DEFAULT 0,"
					  "  UNIQUE (path, size_bytes))");

	const bool uploads = sqlite::execute(_db,
					     "CREATE TABLE IF NOT EXISTS uploads ("
					     "  log_id     INTEGER NOT NULL REFERENCES logs(id) ON DELETE CASCADE,"
					     "  target     TEXT    NOT NULL,"
					     "  uploaded   INTEGER NOT NULL DEFAULT 0,"
					     "  requested  INTEGER NOT NULL DEFAULT 0,"
					     "  rejected   INTEGER NOT NULL DEFAULT 0,"
					     "  location   TEXT    NOT NULL DEFAULT '',"
					     "  message    TEXT    NOT NULL DEFAULT '',"
					     "  updated_at INTEGER NOT NULL DEFAULT 0,"
					     "  PRIMARY KEY (log_id, target))");

	const bool meta = sqlite::execute(_db,
					  "CREATE TABLE IF NOT EXISTS meta ("
					  "  key   TEXT PRIMARY KEY,"
					  "  value TEXT NOT NULL)");

	return logs && uploads && meta;
}

// -------------------------------------------------------------------------
// Index
// -------------------------------------------------------------------------

std::vector<int64_t> LogDatabase::sync_index(const std::vector<Discovered>& logs)
{
	std::lock_guard<std::mutex> lock(_mutex);
	std::vector<int64_t> inserted;

	sqlite::Transaction transaction(_db);

	// The listing is complete (a partial one fails the whole refresh), so
	// anything it does not mention is gone from the vehicle.
	sqlite::execute(_db, "UPDATE logs SET present = 0");

	const int64_t discovered_at = now_epoch();

	for (const auto& log : logs) {
		int64_t id = 0;
		bool is_new = false;

		{
			sqlite::Statement insert(_db,
						 "INSERT OR IGNORE INTO logs (path, size_bytes, time_utc, discovered_at) "
						 "VALUES (?, ?, ?, ?)");
			insert.bind(1, log.path)
			.bind(2, static_cast<int64_t>(log.size_bytes))
			.bind(3, log.time_utc)
			.bind(4, discovered_at);
			insert.execute();

			if (sqlite3_changes(_db) > 0) {
				id = sqlite3_last_insert_rowid(_db);
				is_new = true;
			}
		}

		if (!is_new) {
			sqlite::Statement find(_db, "SELECT id FROM logs WHERE path = ? AND size_bytes = ?");
			find.bind(1, log.path).bind(2, static_cast<int64_t>(log.size_bytes));

			if (!find.step()) {
				continue;
			}

			id = find.column_int(0);
		}

		// A firmware upgrade can start reporting modification times for logs
		// that were indexed without one.
		sqlite::Statement update(_db,
					 "UPDATE logs SET present = 1, time_utc = COALESCE(time_utc, ?) WHERE id = ?");
		update.bind(1, log.time_utc).bind(2, id);
		update.execute();

		if (is_new) {
			ensure_upload_rows(id);

			if (_has_legacy_rows) {
				grandfather(id, log);
			}

			inserted.push_back(id);
		}
	}

	transaction.commit();

	return inserted;
}

void LogDatabase::ensure_upload_rows(int64_t id)
{
	for (const auto& target : _targets) {
		sqlite::Statement stmt(_db, "INSERT OR IGNORE INTO uploads (log_id, target) VALUES (?, ?)");
		stmt.bind(1, id).bind(2, target);
		stmt.execute();
	}
}

bool LogDatabase::first_index_seen() const
{
	std::lock_guard<std::mutex> lock(_mutex);
	sqlite::Statement stmt(_db, "SELECT value FROM meta WHERE key = 'first_index_seen'");
	return stmt.step() && stmt.column_text(0) == "1";
}

void LogDatabase::set_first_index_seen()
{
	std::lock_guard<std::mutex> lock(_mutex);
	sqlite::Statement stmt(_db, "INSERT OR REPLACE INTO meta (key, value) VALUES ('first_index_seen', '1')");
	stmt.execute();
}

// -------------------------------------------------------------------------
// Intent
// -------------------------------------------------------------------------

void LogDatabase::request_download(const std::vector<int64_t>& ids, const std::vector<std::string>& targets)
{
	std::lock_guard<std::mutex> lock(_mutex);
	sqlite::Transaction transaction(_db);

	for (int64_t id : ids) {
		// Clearing the failure count gives an explicitly re-requested log a
		// fresh place in the queue instead of the back of it.
		sqlite::Statement stmt(_db,
				       "UPDATE logs SET download_requested = 1, download_failures = 0, last_error = '' "
				       "WHERE id = ? AND downloaded = 0");
		stmt.bind(1, id);
		stmt.execute();

		for (const auto& target : targets) {
			sqlite::Statement upload(_db,
						 "UPDATE uploads SET requested = 1, rejected = 0 "
						 "WHERE log_id = ? AND target = ? AND uploaded = 0");
			upload.bind(1, id).bind(2, target);
			upload.execute();
		}
	}

	transaction.commit();
}

void LogDatabase::request_upload(const std::vector<int64_t>& ids, const std::vector<std::string>& targets)
{
	std::lock_guard<std::mutex> lock(_mutex);
	sqlite::Transaction transaction(_db);

	for (int64_t id : ids) {
		// An upload needs the file, so asking for one implies fetching it.
		sqlite::Statement stmt(_db,
				       "UPDATE logs SET download_requested = 1 WHERE id = ? AND downloaded = 0");
		stmt.bind(1, id);
		stmt.execute();

		for (const auto& target : targets) {
			sqlite::Statement upload(_db,
						 "UPDATE uploads SET requested = 1, rejected = 0 "
						 "WHERE log_id = ? AND target = ? AND uploaded = 0");
			upload.bind(1, id).bind(2, target);
			upload.execute();
		}
	}

	transaction.commit();
}

void LogDatabase::cancel_requests(const std::vector<int64_t>& ids)
{
	std::lock_guard<std::mutex> lock(_mutex);
	sqlite::Transaction transaction(_db);

	for (int64_t id : ids) {
		sqlite::Statement stmt(_db, "UPDATE logs SET download_requested = 0 WHERE id = ?");
		stmt.bind(1, id);
		stmt.execute();

		sqlite::Statement upload(_db, "UPDATE uploads SET requested = 0 WHERE log_id = ?");
		upload.bind(1, id);
		upload.execute();
	}

	transaction.commit();
}

// -------------------------------------------------------------------------
// Work queues
// -------------------------------------------------------------------------

std::vector<LogDatabase::Entry> LogDatabase::logs_to_download() const
{
	std::lock_guard<std::mutex> lock(_mutex);

	sqlite::Statement stmt(_db,
			       "SELECT " + kLogColumns + " FROM logs l "
			       "WHERE l.download_requested = 1 AND l.downloaded = 0 AND l.present = 1 "
			       "ORDER BY l.download_failures ASC, " + kNewestFirst);

	std::vector<Entry> entries;

	while (stmt.step()) {
		entries.push_back(row_to_entry(stmt));
	}

	attach_uploads(entries);
	return entries;
}

std::vector<LogDatabase::Entry> LogDatabase::logs_to_upload(const std::string& target) const
{
	std::lock_guard<std::mutex> lock(_mutex);

	sqlite::Statement stmt(_db,
			       "SELECT " + kLogColumns + " FROM logs l "
			       "JOIN uploads u ON u.log_id = l.id "
			       "WHERE u.target = ? AND u.requested = 1 AND u.uploaded = 0 AND u.rejected = 0 "
			       "AND l.downloaded = 1 AND l.local_path != '' "
			       "ORDER BY " + kNewestFirst);
	stmt.bind(1, target);

	std::vector<Entry> entries;

	while (stmt.step()) {
		entries.push_back(row_to_entry(stmt));
	}

	attach_uploads(entries);
	return entries;
}

// -------------------------------------------------------------------------
// State updates
// -------------------------------------------------------------------------

void LogDatabase::mark_downloaded(int64_t id, const std::string& local_path)
{
	std::lock_guard<std::mutex> lock(_mutex);
	sqlite::Statement stmt(_db,
			       "UPDATE logs SET downloaded = 1, download_requested = 0, local_path = ?, "
			       "download_failures = 0, last_error = '' WHERE id = ?");
	stmt.bind(1, local_path).bind(2, id);
	stmt.execute();
}

void LogDatabase::record_download_failure(int64_t id, const std::string& error)
{
	std::lock_guard<std::mutex> lock(_mutex);
	sqlite::Statement stmt(_db,
			       "UPDATE logs SET download_failures = download_failures + 1, last_error = ? WHERE id = ?");
	stmt.bind(1, error).bind(2, id);
	stmt.execute();
}

void LogDatabase::mark_uploaded(int64_t id, const std::string& target, const std::string& location)
{
	std::lock_guard<std::mutex> lock(_mutex);
	sqlite::Statement stmt(_db,
			       "UPDATE uploads SET uploaded = 1, requested = 0, rejected = 0, location = ?, message = '', "
			       "updated_at = ? WHERE log_id = ? AND target = ?");
	stmt.bind(1, location).bind(2, now_epoch()).bind(3, id).bind(4, target);
	stmt.execute();
}

void LogDatabase::mark_upload_rejected(int64_t id, const std::string& target, const std::string& message)
{
	std::lock_guard<std::mutex> lock(_mutex);
	sqlite::Statement stmt(_db,
			       "UPDATE uploads SET rejected = 1, requested = 0, message = ?, updated_at = ? "
			       "WHERE log_id = ? AND target = ?");
	stmt.bind(1, message).bind(2, now_epoch()).bind(3, id).bind(4, target);
	stmt.execute();
}

void LogDatabase::record_upload_failure(int64_t id, const std::string& target, const std::string& message)
{
	std::lock_guard<std::mutex> lock(_mutex);
	sqlite::Statement stmt(_db,
			       "UPDATE uploads SET message = ?, updated_at = ? WHERE log_id = ? AND target = ?");
	stmt.bind(1, message).bind(2, now_epoch()).bind(3, id).bind(4, target);
	stmt.execute();
}

void LogDatabase::clear_local_file(int64_t id)
{
	std::lock_guard<std::mutex> lock(_mutex);
	sqlite::Statement stmt(_db,
			       "UPDATE logs SET downloaded = 0, local_path = '', download_requested = 0 WHERE id = ?");
	stmt.bind(1, id);
	stmt.execute();
}

// -------------------------------------------------------------------------
// Queries
// -------------------------------------------------------------------------

std::vector<LogDatabase::Entry> LogDatabase::all_logs() const
{
	std::lock_guard<std::mutex> lock(_mutex);

	sqlite::Statement stmt(_db, "SELECT " + kLogColumns + " FROM logs l ORDER BY " + kNewestFirst);

	std::vector<Entry> entries;

	while (stmt.step()) {
		entries.push_back(row_to_entry(stmt));
	}

	attach_uploads(entries);
	return entries;
}

std::optional<LogDatabase::Entry> LogDatabase::log_by_id(int64_t id) const
{
	std::lock_guard<std::mutex> lock(_mutex);

	sqlite::Statement stmt(_db, "SELECT " + kLogColumns + " FROM logs l WHERE l.id = ?");
	stmt.bind(1, id);

	if (!stmt.step()) {
		return std::nullopt;
	}

	std::vector<Entry> entries {row_to_entry(stmt)};
	attach_uploads(entries);
	return entries.front();
}

std::optional<int64_t> LogDatabase::newest_log_id(const std::vector<int64_t>& among) const
{
	std::lock_guard<std::mutex> lock(_mutex);

	std::string filter = "l.present = 1";

	if (!among.empty()) {
		filter += " AND l.id IN (?";

		for (size_t i = 1; i < among.size(); i++) {
			filter += ",?";
		}

		filter += ")";
	}

	sqlite::Statement stmt(_db,
			       "SELECT l.id FROM logs l WHERE " + filter + " ORDER BY " + kNewestFirst + " LIMIT 1");

	for (size_t i = 0; i < among.size(); i++) {
		stmt.bind(static_cast<int>(i) + 1, among[i]);
	}

	if (!stmt.step()) {
		return std::nullopt;
	}

	return stmt.column_int(0);
}

bool LogDatabase::local_path_in_use(const std::string& local_path, int64_t excluding_id) const
{
	std::lock_guard<std::mutex> lock(_mutex);

	sqlite::Statement stmt(_db, "SELECT COUNT(*) FROM logs WHERE local_path = ? AND id != ?");
	stmt.bind(1, local_path).bind(2, excluding_id);

	return stmt.step() && stmt.column_int(0) > 0;
}

void LogDatabase::attach_uploads(std::vector<Entry>& entries) const
{
	if (entries.empty()) {
		return;
	}

	std::map<int64_t, Entry*> by_id;

	for (auto& entry : entries) {
		for (const auto& target : _targets) {
			entry.uploads[target] = {};
		}

		by_id[entry.id] = &entry;
	}

	sqlite::Statement stmt(_db, "SELECT log_id, target, uploaded, requested, rejected, location, message FROM uploads");

	while (stmt.step()) {
		auto it = by_id.find(stmt.column_int(0));

		if (it == by_id.end()) {
			continue;
		}

		UploadState state;
		state.uploaded = stmt.column_int(2) != 0;
		state.requested = stmt.column_int(3) != 0;
		state.rejected = stmt.column_int(4) != 0;
		state.location = stmt.column_text(5);
		state.message = stmt.column_text(6);

		it->second->uploads[stmt.column_text(1)] = state;
	}
}

// -------------------------------------------------------------------------
// Migration from the pre-overhaul per-server databases
// -------------------------------------------------------------------------

void LogDatabase::import_legacy_databases(const std::string& db_path)
{
	const fs::path directory = fs::path(db_path).parent_path();

	const std::pair<const char*, const char*> sources[] = {
		{"local_server.db", kTargetLocal},
		{"remote_server.db", kTargetRemote},
	};

	for (const auto& [file, target] : sources) {
		const fs::path source = directory / file;

		if (fs::exists(source)) {
			import_legacy_database(source.string(), target);
		}
	}
}

void LogDatabase::import_legacy_database(const std::string& file, const std::string& target)
{
	// Importing twice would resurrect rows the user has since dealt with.
	{
		sqlite::Statement seen(_db, "SELECT value FROM meta WHERE key = ?");
		seen.bind(1, "imported_" + target);

		if (seen.step()) {
			return;
		}
	}

	sqlite::execute(_db,
			"CREATE TABLE IF NOT EXISTS legacy_logs ("
			"  target     TEXT    NOT NULL,"
			"  uuid       TEXT    NOT NULL,"
			"  legacy_id  INTEGER NOT NULL DEFAULT 0,"
			"  date       TEXT    NOT NULL DEFAULT '',"
			"  size_bytes INTEGER NOT NULL DEFAULT 0,"
			"  downloaded INTEGER NOT NULL DEFAULT 0,"
			"  uploaded   INTEGER NOT NULL DEFAULT 0,"
			"  rejected   INTEGER NOT NULL DEFAULT 0,"
			"  matched    INTEGER NOT NULL DEFAULT 0,"
			"  PRIMARY KEY (target, uuid))");

	sqlite::Statement attach(_db, "ATTACH DATABASE ? AS legacy");
	attach.bind(1, file);

	if (!attach.execute()) {
		LOG_WARN("Could not read the previous database " << file);
		return;
	}

	// Only the pre-FTP schema is ever imported: it keyed logs on a LOG_ENTRY
	// index and timestamp, which is the state this class cannot reconstruct.
	sqlite::Statement shape(_db, "SELECT COUNT(*) FROM pragma_table_info('logs', 'legacy') WHERE name = 'id'");

	if (!shape.step() || shape.column_int(0) == 0) {
		LOG_DEBUG("Previous database " << file << " has no pre-FTP log table, nothing to import");
		sqlite::execute(_db, "DETACH DATABASE legacy");
		return;
	}

	int imported = 0;

	{
		sqlite::Transaction transaction(_db);

		sqlite::Statement read(_db,
				       "SELECT uuid, id, date, size_bytes, downloaded, uploaded FROM legacy.logs");

		while (read.step()) {
			sqlite::Statement row(_db,
					      "INSERT OR IGNORE INTO legacy_logs "
					      "(target, uuid, legacy_id, date, size_bytes, downloaded, uploaded, rejected) "
					      "VALUES (?, ?, ?, ?, ?, ?, ?, 0)");
			row.bind(1, target)
			.bind(2, read.column_text(0))
			.bind(3, read.column_int(1))
			.bind(4, read.column_text(2))
			.bind(5, read.column_int(3))
			.bind(6, read.column_int(4))
			.bind(7, read.column_int(5));
			row.execute();
			imported++;
		}

		sqlite::Statement blacklist(_db,
					    "UPDATE legacy_logs SET rejected = 1 "
					    "WHERE target = ? AND uuid IN (SELECT uuid FROM legacy.blacklist)");
		blacklist.bind(1, target);
		blacklist.execute();

		sqlite::Statement mark(_db, "INSERT OR REPLACE INTO meta (key, value) VALUES (?, '1')");
		mark.bind(1, "imported_" + target);
		mark.execute();

		transaction.commit();
	}

	sqlite::execute(_db, "DETACH DATABASE legacy");

	if (imported > 0) {
		LOG("Imported " << imported << " log records from the previous " << target << " database");
	}
}

void LogDatabase::grandfather(int64_t id, const Discovered& log)
{
	struct Candidate {
		std::string target;
		std::string uuid;
		int64_t legacy_id {0};
		std::string date;
		bool downloaded {false};
		bool uploaded {false};
		bool rejected {false};
	};

	for (const auto& target : _targets) {
		std::vector<Candidate> candidates;

		{
			sqlite::Statement stmt(_db,
					       "SELECT uuid, legacy_id, date, downloaded, uploaded, rejected FROM legacy_logs "
					       "WHERE target = ? AND matched = 0 AND size_bytes = ?");
			stmt.bind(1, target).bind(2, static_cast<int64_t>(log.size_bytes));

			while (stmt.step()) {
				Candidate candidate;
				candidate.target = target;
				candidate.uuid = stmt.column_text(0);
				candidate.legacy_id = stmt.column_int(1);
				candidate.date = stmt.column_text(2);
				candidate.downloaded = stmt.column_int(3) != 0;
				candidate.uploaded = stmt.column_int(4) != 0;
				candidate.rejected = stmt.column_int(5) != 0;
				candidates.push_back(candidate);
			}
		}

		if (candidates.empty()) {
			continue;
		}

		// The old database recorded the log's modification time; the FTP index
		// reports either that or the start time in the path, so the same log is
		// at most a flight apart. With no time to compare, only a single
		// unambiguous candidate is safe to claim.
		const Candidate* match = nullptr;

		if (log.time_utc.has_value()) {
			constexpr int64_t kMaxDelta = 48 * 3600;
			int64_t best = kMaxDelta;

			for (const auto& candidate : candidates) {
				const auto legacy_time = parse_iso8601_utc(candidate.date);

				if (!legacy_time.has_value()) {
					continue;
				}

				const int64_t delta = std::abs(legacy_time.value() - log.time_utc.value());

				if (delta <= best) {
					best = delta;
					match = &candidate;
				}
			}

		} else if (candidates.size() == 1) {
			match = &candidates.front();
		}

		if (match == nullptr) {
			continue;
		}

		// The name the old download used, so an already-fetched log is not
		// fetched again.
		std::ostringstream legacy_name;
		legacy_name << _logs_directory << "LOG" << std::setfill('0') << std::setw(4)
			    << match->legacy_id << "_" << match->date << ".ulg";

		const std::string legacy_path = legacy_name.str();
		const bool file_exists = fs::exists(legacy_path);
		// A log whose file went missing but which was already uploaded does not
		// need fetching again; one that was never uploaded does.
		const bool downloaded = match->downloaded && (file_exists || match->uploaded);

		sqlite::Statement update(_db,
					 "UPDATE logs SET downloaded = ?, local_path = ? WHERE id = ? AND downloaded = 0");
		update.bind(1, downloaded)
		.bind(2, (match->downloaded && file_exists) ? legacy_path : std::string {})
		.bind(3, id);
		update.execute();

		sqlite::Statement upload(_db,
					 "UPDATE uploads SET uploaded = ?, rejected = ? WHERE log_id = ? AND target = ?");
		upload.bind(1, match->uploaded).bind(2, match->rejected).bind(3, id).bind(4, target);
		upload.execute();

		sqlite::Statement mark(_db, "UPDATE legacy_logs SET matched = 1 WHERE target = ? AND uuid = ?");
		mark.bind(1, target).bind(2, match->uuid);
		mark.execute();

		LOG_DEBUG("Carried over " << target << " state for " << log.path << " from the previous database");
	}
}

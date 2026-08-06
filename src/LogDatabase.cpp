#include "LogDatabase.hpp"
#include "LegacyImport.hpp"
#include "Log.hpp"
#include "Sqlite.hpp"

#include <algorithm>
#include <ctime>

namespace
{

// Every SELECT reads the same columns in the same order, so row_to_entry only
// has to be written once.
const std::string kLogColumns =
	"l.id, l.path, l.size_bytes, l.time_utc, l.local_path, l.downloaded, "
	"l.download_requested, l.present, l.download_failures, l.last_error, l.discovered_at";

// Newest first. Logs whose time the vehicle cannot report sort after the ones
// it can; among those, a log we watched appear is newer than one that was
// already on the card. Only within a single listing, where there is no
// discovery order to go on, does the name decide: ArduPilot numbers its logs,
// so the longer name is the later one (100.BIN after 99.BIN) -- right up until
// the counter wraps at LOG_MAX_FILES, which is why it is the last resort.
const std::string kNewestFirst =
	"(l.time_utc IS NULL) ASC, l.time_utc DESC, l.discovered_seq DESC, LENGTH(l.path) DESC, l.path DESC";

int64_t now_epoch()
{
	return static_cast<int64_t>(std::time(nullptr));
}

// A failed commit must not look like a successful one: the caller would go on
// to act on rows that were rolled out from under it.
bool commit_or_warn(sqlite::Transaction& transaction, const char* what)
{
	if (transaction.commit()) {
		return true;
	}

	LOG_ERROR("Could not commit " << what << "; the change was rolled back");
	return false;
}

LogDatabase::Entry row_to_entry(const sqlite::Statement& stmt)
{
	LogDatabase::Entry entry;
	entry.id = stmt.column_int(0);
	entry.path = stmt.column_text(1);
	entry.size_bytes = static_cast<uint32_t>(stmt.column_int(2));
	entry.time_utc = stmt.column_optional_int(3);
	entry.local_path = stmt.column_text(4);
	// The queues treat a row with no file as not downloaded; so must anything
	// that reports it, or the API and the queue disagree about the same row.
	entry.downloaded = stmt.column_int(5) != 0 && !entry.local_path.empty();
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
	: _targets(targets)
{
	if (!initialize(db_path)) {
		LOG_ERROR("Could not open the log database at " << db_path);
		return;
	}

	_legacy = std::make_unique<legacy::Importer>(_db, db_path, logs_directory, _targets);
	_ok = true;
}

LogDatabase::~LogDatabase()
{
	if (_db != nullptr) {
		// _v2 so a straggling statement leaks nothing and the handle still closes.
		sqlite3_close_v2(_db);
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
					  "  discovered_seq     INTEGER NOT NULL DEFAULT 0,"
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

	return logs && uploads && meta && migrate_schema();
}

bool LogDatabase::migrate_schema()
{
	if (!sqlite::column_exists(_db, "logs", "discovered_seq")) {
		if (!sqlite::execute(_db, "ALTER TABLE logs ADD COLUMN discovered_seq INTEGER NOT NULL DEFAULT 0")) {
			return false;
		}

		// discovered_at is the same ordering at coarser resolution, which is
		// the best that can be reconstructed for rows that predate the column.
		sqlite::execute(_db, "UPDATE logs SET discovered_seq = discovered_at");
	}

	return true;
}

// -------------------------------------------------------------------------
// Index
// -------------------------------------------------------------------------

LogDatabase::SyncResult LogDatabase::sync_index(const std::vector<Discovered>& logs)
{
	std::lock_guard<std::mutex> lock(_mutex);
	SyncResult result;

	for (const auto& log : logs) {
		if (log.stable) {
			result.present_count++;
		}
	}

	sqlite::Transaction transaction(_db);

	if (!transaction) {
		LOG_ERROR("Could not begin a transaction; skipping this index");
		return result;
	}

	// Counted before the wipe below, or it is always zero and the changed-index
	// check at the bottom degenerates: every identical listing looks like a
	// change and an emptied card looks like no change at all.
	int64_t present_before = 0;
	{
		sqlite::Statement stmt(_db, "SELECT COUNT(*) FROM logs WHERE present = 1");

		if (stmt.step()) {
			present_before = stmt.column_int(0);
		}
	}

	// The listing is complete (a partial one fails the whole refresh), so
	// anything it does not mention is gone from the vehicle.
	sqlite::execute(_db, "UPDATE logs SET present = 0");

	const int64_t discovered_at = now_epoch();

	// A wall clock at one-second resolution cannot separate two listings, and
	// on a companion with no RTC it can run backwards. The sequence can do
	// neither.
	int64_t discovered_seq = 1;
	{
		sqlite::Statement stmt(_db, "SELECT COALESCE(MAX(discovered_seq), 0) + 1 FROM logs");

		if (stmt.step()) {
			discovered_seq = stmt.column_int(0);
		}
	}

	for (const auto& log : logs) {
		int64_t id = 0;
		bool is_new = false;

		if (log.stable) {
			sqlite::Statement insert(_db,
						 "INSERT OR IGNORE INTO logs "
						 "(path, size_bytes, time_utc, discovered_at, discovered_seq) "
						 "VALUES (?, ?, ?, ?, ?)");
			insert.bind(1, log.path)
			.bind(2, static_cast<int64_t>(log.size_bytes))
			.bind(3, log.time_utc)
			.bind(4, discovered_at)
			.bind(5, discovered_seq);
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
				// A log we have never recorded, still being written. Nothing to
				// mark present; it becomes a row once it stops changing.
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

			if (_legacy->has_rows()) {
				_legacy->adopt(id, log.size_bytes, log.time_utc, log.path);
			}

			result.inserted.push_back(id);
		}
	}

	// A log only counts once two listings agree on its size, so the first
	// listing after startup reconciles nothing. Recording that as the first
	// index would make the next one -- the vehicle's whole history -- look like
	// logs that appeared while we were watching.
	if (result.present_count > 0) {
		sqlite::Statement seen(_db, "SELECT value FROM meta WHERE key = 'first_index_seen'");
		result.first_ever = !seen.step();

		if (result.first_ever) {
			sqlite::Statement mark(_db,
					       "INSERT OR REPLACE INTO meta (key, value) VALUES ('first_index_seen', '1')");
			mark.execute();
		}
	}

	int64_t present_after = 0;
	{
		sqlite::Statement stmt(_db, "SELECT COUNT(*) FROM logs WHERE present = 1");

		if (stmt.step()) {
			present_after = stmt.column_int(0);
		}
	}

	if (!commit_or_warn(transaction, "the vehicle index")) {
		return {};
	}

	// An identical listing, which is what most of them are, is not a change:
	// bumping the revision anyway would push the whole log list to every open
	// stream once per index interval for nothing.
	if (!result.inserted.empty() || present_before != present_after) {
		_revision++;
	}

	return result;
}

void LogDatabase::ensure_upload_rows(int64_t id)
{
	for (const auto& target : _targets) {
		sqlite::Statement stmt(_db, "INSERT OR IGNORE INTO uploads (log_id, target) VALUES (?, ?)");
		stmt.bind(1, id).bind(2, target);
		stmt.execute();
	}
}

// -------------------------------------------------------------------------
// Intent
// -------------------------------------------------------------------------

void LogDatabase::request(const std::vector<int64_t>& ids, const std::vector<std::string>& targets)
{
	std::lock_guard<std::mutex> lock(_mutex);
	sqlite::Transaction transaction(_db);

	for (int64_t id : ids) {
		// An upload needs the file, so asking for either implies fetching it.
		// Clearing the failure count gives an explicitly requested log a fresh
		// place in the queue instead of the back of it.
		sqlite::Statement stmt(_db,
				       "UPDATE logs SET download_requested = 1, download_failures = 0, last_error = '' "
				       "WHERE id = ? AND (downloaded = 0 OR local_path = '')");
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

	if (commit_or_warn(transaction, "request")) {
		_revision++;
	}
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

	if (commit_or_warn(transaction, "cancel")) {
		_revision++;
	}
}

// -------------------------------------------------------------------------
// Work queues
// -------------------------------------------------------------------------

std::vector<LogDatabase::Entry> LogDatabase::logs_to_download() const
{
	std::lock_guard<std::mutex> lock(_mutex);

	sqlite::Statement stmt(_db,
			       "SELECT " + kLogColumns + " FROM logs l "
			       "WHERE l.download_requested = 1 AND (l.downloaded = 0 OR l.local_path = '') AND l.present = 1 "
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
	_revision++;
}

void LogDatabase::record_download_failure(int64_t id, const std::string& error)
{
	std::lock_guard<std::mutex> lock(_mutex);
	sqlite::Statement stmt(_db,
			       "UPDATE logs SET download_failures = download_failures + 1, last_error = ? WHERE id = ?");
	stmt.bind(1, error).bind(2, id);
	stmt.execute();
	_revision++;
}

void LogDatabase::mark_uploaded(int64_t id, const std::string& target, const std::string& location)
{
	std::lock_guard<std::mutex> lock(_mutex);
	sqlite::Statement stmt(_db,
			       "UPDATE uploads SET uploaded = 1, requested = 0, rejected = 0, location = ?, message = '', "
			       "updated_at = ? WHERE log_id = ? AND target = ?");
	stmt.bind(1, location).bind(2, now_epoch()).bind(3, id).bind(4, target);
	stmt.execute();
	_revision++;
}

void LogDatabase::mark_upload_rejected(int64_t id, const std::string& target, const std::string& message)
{
	std::lock_guard<std::mutex> lock(_mutex);
	sqlite::Statement stmt(_db,
			       "UPDATE uploads SET rejected = 1, requested = 0, message = ?, updated_at = ? "
			       "WHERE log_id = ? AND target = ?");
	stmt.bind(1, message).bind(2, now_epoch()).bind(3, id).bind(4, target);
	stmt.execute();
	_revision++;
}

void LogDatabase::record_upload_failure(int64_t id, const std::string& target, const std::string& message)
{
	std::lock_guard<std::mutex> lock(_mutex);
	sqlite::Statement stmt(_db,
			       "UPDATE uploads SET message = ?, updated_at = ? WHERE log_id = ? AND target = ?");
	stmt.bind(1, message).bind(2, now_epoch()).bind(3, id).bind(4, target);
	stmt.execute();
	_revision++;
}

bool LogDatabase::clear_local_file(int64_t id, const std::string& expected_local_path)
{
	std::lock_guard<std::mutex> lock(_mutex);

	// Bound to the path the caller saw. The upload thread works from a snapshot
	// that can be minutes old, and clearing unconditionally would wipe the
	// bookkeeping of a download the index thread finished in the meantime.
	std::string sql = "UPDATE logs SET downloaded = 0, local_path = '', download_requested = 0 WHERE id = ?";

	if (!expected_local_path.empty()) {
		sql += " AND local_path = ?";
	}

	sqlite::Statement stmt(_db, sql);
	stmt.bind(1, id);

	if (!expected_local_path.empty()) {
		stmt.bind(2, expected_local_path);
	}

	stmt.execute();
	const bool changed = sqlite3_changes(_db) > 0;

	if (changed) {
		_revision++;
	}

	return changed;
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

std::vector<int64_t> LogDatabase::ids_not_downloaded() const
{
	std::lock_guard<std::mutex> lock(_mutex);

	sqlite::Statement stmt(_db,
			       "SELECT l.id FROM logs l WHERE l.present = 1 AND (l.downloaded = 0 OR l.local_path = '') ORDER BY " + kNewestFirst);

	std::vector<int64_t> ids;

	while (stmt.step()) {
		ids.push_back(stmt.column_int(0));
	}

	return ids;
}

std::vector<int64_t> LogDatabase::ids_not_uploaded(const std::vector<std::string>& targets) const
{
	std::lock_guard<std::mutex> lock(_mutex);
	std::vector<int64_t> ids;

	if (targets.empty()) {
		return ids;
	}

	std::string placeholders = "?";

	for (size_t i = 1; i < targets.size(); i++) {
		placeholders += ",?";
	}

	// Still on the vehicle or already fetched -- either way the file can be had.
	sqlite::Statement stmt(_db,
			       "SELECT DISTINCT l.id FROM logs l "
			       "JOIN uploads u ON u.log_id = l.id "
			       "WHERE u.target IN (" + placeholders + ") AND u.uploaded = 0 "
			       "AND (l.downloaded = 1 OR l.present = 1) "
			       "ORDER BY " + kNewestFirst);

	for (size_t i = 0; i < targets.size(); i++) {
		stmt.bind(static_cast<int>(i) + 1, targets[i]);
	}

	while (stmt.step()) {
		ids.push_back(stmt.column_int(0));
	}

	return ids;
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

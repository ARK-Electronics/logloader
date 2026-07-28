#include "LegacyImport.hpp"
#include "Log.hpp"
#include "Sqlite.hpp"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <sstream>

namespace fs = std::filesystem;

namespace legacy
{

namespace
{

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

// A second database stays attached on any path that forgets to detach, and the
// next ATTACH then fails with "already in use".
class Attached
{
public:
	Attached(sqlite3* db, const std::string& file)
		: _db(db)
	{
		sqlite::Statement attach(_db, "ATTACH DATABASE ? AS legacy");
		attach.bind(1, file);
		_attached = attach.execute();
	}

	~Attached()
	{
		if (_attached) {
			sqlite::execute(_db, "DETACH DATABASE legacy");
		}
	}

	Attached(const Attached&) = delete;
	Attached& operator=(const Attached&) = delete;

	explicit operator bool() const { return _attached; }

private:
	sqlite3* _db {nullptr};
	bool _attached {false};
};

struct Candidate {
	std::string uuid;
	int64_t legacy_id {0};
	std::string date;
	bool downloaded {false};
	bool uploaded {false};
	bool rejected {false};
};

} // namespace

Importer::Importer(sqlite3* db, const std::string& db_path, const std::string& logs_directory,
		   const std::vector<std::string>& targets)
	: _db(db)
	, _logs_directory(logs_directory)
	, _targets(targets)
{
	const fs::path directory = fs::path(db_path).parent_path();

	// The old database file names, and which target each one's upload state
	// belongs to.
	const std::pair<const char*, const char*> sources[] = {
		{"local_server.db", "local"},
		{"remote_server.db", "remote"},
	};

	for (const auto& [file, target] : sources) {
		const fs::path source = directory / file;

		if (fs::exists(source)) {
			import_one(source.string(), target);
		}
	}

	_has_rows = sqlite::table_exists(_db, "legacy_logs");
}

void Importer::import_one(const std::string& file, const std::string& target)
{
	// Importing twice would resurrect rows the operator has since dealt with.
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

	const Attached legacy_db(_db, file);

	if (!legacy_db) {
		LOG_WARN("Could not read the previous database " << file);
		return;
	}

	// Only the pre-FTP schema is ever imported: it is the one whose state this
	// class cannot otherwise reconstruct.
	sqlite::Statement shape(_db, "SELECT COUNT(*) FROM pragma_table_info('logs', 'legacy') WHERE name = 'id'");

	if (!shape.step() || shape.column_int(0) == 0) {
		LOG_DEBUG("Previous database " << file << " has no pre-FTP log table, nothing to import");
		return;
	}

	int imported = 0;
	sqlite::Transaction transaction(_db);

	{
		sqlite::Statement read(_db, "SELECT uuid, id, date, size_bytes, downloaded, uploaded FROM legacy.logs");

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
	}

	sqlite::Statement blacklist(_db,
				    "UPDATE legacy_logs SET rejected = 1 "
				    "WHERE target = ? AND uuid IN (SELECT uuid FROM legacy.blacklist)");
	blacklist.bind(1, target);
	blacklist.execute();

	sqlite::Statement mark(_db, "INSERT OR REPLACE INTO meta (key, value) VALUES (?, '1')");
	mark.bind(1, "imported_" + target);
	mark.execute();

	if (!transaction.commit()) {
		LOG_WARN("Could not import " << file << "; it will be retried on the next start");
		return;
	}

	if (imported > 0) {
		LOG("Carried over " << imported << " log records from the previous " << target << " database");
	}
}

void Importer::adopt(int64_t log_id, uint32_t size_bytes, std::optional<int64_t> time_utc, const std::string& path)
{
	for (const auto& target : _targets) {
		std::vector<Candidate> candidates;

		{
			sqlite::Statement stmt(_db,
					       "SELECT uuid, legacy_id, date, downloaded, uploaded, rejected FROM legacy_logs "
					       "WHERE target = ? AND matched = 0 AND size_bytes = ?");
			stmt.bind(1, target).bind(2, static_cast<int64_t>(size_bytes));

			while (stmt.step()) {
				Candidate candidate;
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

		if (time_utc.has_value()) {
			constexpr int64_t kMaxDelta = 48 * 3600;
			int64_t best = kMaxDelta;

			for (const auto& candidate : candidates) {
				const auto legacy_time = parse_iso8601_utc(candidate.date);

				if (!legacy_time.has_value()) {
					continue;
				}

				const int64_t delta = std::abs(legacy_time.value() - time_utc.value());

				// Strictly better, so several equidistant candidates leave
				// the first one holding the claim rather than the last.
				if (delta < best) {
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

		// The name the old download used, so a log already on disk is not
		// fetched a second time.
		std::ostringstream legacy_name;
		legacy_name << _logs_directory << "LOG" << std::setfill('0') << std::setw(4)
			    << match->legacy_id << "_" << match->date << ".ulg";

		const std::string legacy_path = legacy_name.str();
		const bool file_exists = fs::exists(legacy_path);

		if (match->downloaded && file_exists) {
			sqlite::Statement update(_db,
						 "UPDATE logs SET downloaded = 1, local_path = ? WHERE id = ? AND downloaded = 0");
			update.bind(1, legacy_path).bind(2, log_id);
			update.execute();
		}

		sqlite::Statement upload(_db,
					 "UPDATE uploads SET uploaded = ?, rejected = ? WHERE log_id = ? AND target = ?");
		upload.bind(1, match->uploaded).bind(2, match->rejected).bind(3, log_id).bind(4, target);
		upload.execute();

		sqlite::Statement mark(_db, "UPDATE legacy_logs SET matched = 1 WHERE target = ? AND uuid = ?");
		mark.bind(1, target).bind(2, match->uuid);
		mark.execute();

		LOG_DEBUG("Carried over " << target << " state for " << path << " from the previous database");
	}
}

} // namespace legacy

// LogDatabase holds every decision logloader makes about a log, and the legacy
// import runs exactly once against real user data and cannot be un-run. Both
// are worth testing without a vehicle attached.
//
//   cmake -B build -DBUILD_TESTING=ON && cmake --build build && ctest --test-dir build

#include "LogDatabase.hpp"
#include "Sqlite.hpp"

#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{

int g_checks = 0;

#define CHECK(condition)                                                       \
	do {                                                                   \
		g_checks++;                                                    \
		if (!(condition)) {                                            \
			std::cerr << "FAILED " << __FILE__ << ":" << __LINE__   \
				  << ": " << #condition << "\n";               \
			std::abort();                                          \
		}                                                              \
	} while (0)

const std::vector<std::string> kTargets {"local", "remote"};

// A directory that cleans up after itself, so a failed run leaves nothing behind.
class Workspace
{
public:
	Workspace()
	{
		_path = fs::temp_directory_path() / ("logloader-test-" + std::to_string(::getpid()) + "-"
						     + std::to_string(_counter++));
		fs::create_directories(_path / "logs");
	}

	~Workspace() { fs::remove_all(_path); }

	std::string db() const { return (_path / "logloader.db").string(); }
	std::string logs() const { return (_path / "logs").string() + "/"; }
	fs::path path() const { return _path; }

private:
	fs::path _path;
	static inline int _counter = 0;
};

LogDatabase::Discovered px4(const std::string& path, uint32_t size, int64_t time)
{
	return {path, size, time};
}

LogDatabase::Discovered ardupilot(const std::string& path, uint32_t size)
{
	return {path, size, std::nullopt};
}

const LogDatabase::Entry* find(const std::vector<LogDatabase::Entry>& entries, const std::string& path)
{
	for (const auto& entry : entries) {
		if (entry.path == path) {
			return &entry;
		}
	}

	return nullptr;
}

// -------------------------------------------------------------------------

void first_index_needs_a_non_empty_listing()
{
	Workspace workspace;
	LogDatabase database(workspace.db(), workspace.logs(), kTargets);
	CHECK(database.ok());

	// The very first listing after startup reports nothing stable, because
	// stability takes two listings to establish. That must not count as the
	// first index, or the next listing looks like a flood of new logs.
	const auto empty = database.sync_index({});
	CHECK(!empty.first_ever);
	CHECK(empty.inserted.empty());

	const auto real = database.sync_index({px4("2026-07-28/10_00_00.ulg", 100, 1000),
					       px4("2026-07-28/11_00_00.ulg", 200, 2000)});
	CHECK(real.first_ever);
	CHECK(real.inserted.size() == 2);
	CHECK(real.present_count == 2);

	// Only once.
	const auto later = database.sync_index({px4("2026-07-28/12_00_00.ulg", 300, 3000)});
	CHECK(!later.first_ever);
	CHECK(later.inserted.size() == 1);
}

void newest_is_newest_for_both_stacks()
{
	{
		Workspace workspace;
		LogDatabase database(workspace.db(), workspace.logs(), kTargets);
		database.sync_index({px4("2026-07-28/10_00_00.ulg", 100, 1000),
				     px4("2026-07-28/12_00_00.ulg", 100, 3000),
				     px4("2026-07-28/11_00_00.ulg", 100, 2000)});

		const auto newest = database.newest_log_id();
		CHECK(newest.has_value());
		CHECK(database.log_by_id(newest.value())->path == "2026-07-28/12_00_00.ulg");
	}

	{
		// ArduPilot reports no time at all, so the number in the name is all
		// there is: 100.BIN is later than 99.BIN despite sorting before it.
		Workspace workspace;
		LogDatabase database(workspace.db(), workspace.logs(), kTargets);
		database.sync_index({ardupilot("98.BIN", 100), ardupilot("99.BIN", 100),
				     ardupilot("100.BIN", 100)});

		const auto newest = database.newest_log_id();
		CHECK(newest.has_value());
		CHECK(database.log_by_id(newest.value())->path == "100.BIN");
	}

	{
		// ...until the counter wraps at LOG_MAX_FILES, where the only usable
		// signal is which log we watched appear.
		Workspace workspace;
		LogDatabase database(workspace.db(), workspace.logs(), kTargets);
		database.sync_index({ardupilot("499.BIN", 100), ardupilot("500.BIN", 100)});
		const auto wrapped = database.sync_index({ardupilot("499.BIN", 100), ardupilot("500.BIN", 100),
				     ardupilot("1.BIN", 100)});
		CHECK(wrapped.inserted.size() == 1);

		const auto newest = database.newest_log_id();
		CHECK(newest.has_value());
		CHECK(database.log_by_id(newest.value())->path == "1.BIN");
	}
}

void a_log_that_leaves_the_vehicle_keeps_its_history()
{
	Workspace workspace;
	LogDatabase database(workspace.db(), workspace.logs(), kTargets);

	const auto sync = database.sync_index({px4("a.ulg", 100, 1000)});
	const int64_t id = sync.inserted.front();

	database.mark_downloaded(id, workspace.logs() + "a.ulg");
	database.mark_uploaded(id, "local", "/plot_app?log=abc");

	// Gone from the card.
	database.sync_index({});
	const auto gone = database.log_by_id(id);
	CHECK(gone.has_value());
	CHECK(!gone->present);
	CHECK(gone->downloaded);
	CHECK(gone->uploads.at("local").uploaded);
	CHECK(gone->uploads.at("local").location == "/plot_app?log=abc");

	// And back again, without losing anything.
	database.sync_index({px4("a.ulg", 100, 1000)});
	const auto back = database.log_by_id(id);
	CHECK(back->present);
	CHECK(back->uploads.at("local").uploaded);
}

// The first listing after a restart has nothing stable in it yet, because
// stability takes two listings. Every log is still on the vehicle, and saying
// otherwise both lies to the UI and empties the download queue for an interval.
void a_growing_log_is_still_on_the_vehicle()
{
	Workspace workspace;
	LogDatabase database(workspace.db(), workspace.logs(), kTargets);

	LogDatabase::Discovered settled = px4("a.ulg", 100, 1000);
	database.sync_index({settled});
	CHECK(database.log_by_id(database.all_logs().front().id)->present);

	// Same listing, but nothing has been confirmed stable this time round.
	LogDatabase::Discovered growing = settled;
	growing.stable = false;
	const auto sync = database.sync_index({growing});

	CHECK(sync.present_count == 0);
	CHECK(sync.inserted.empty());
	CHECK(database.all_logs().front().present);
}

void a_growing_log_is_a_different_log()
{
	Workspace workspace;
	LogDatabase database(workspace.db(), workspace.logs(), kTargets);

	const auto first = database.sync_index({px4("a.ulg", 100, 1000)});
	const auto grown = database.sync_index({px4("a.ulg", 200, 1000)});

	CHECK(grown.inserted.size() == 1);
	CHECK(grown.inserted.front() != first.inserted.front());
	CHECK(!database.log_by_id(first.inserted.front())->present);
	CHECK(database.log_by_id(grown.inserted.front())->present);
}

void requests_drive_the_queues()
{
	Workspace workspace;
	LogDatabase database(workspace.db(), workspace.logs(), kTargets);

	const auto sync = database.sync_index({px4("a.ulg", 100, 1000), px4("b.ulg", 100, 2000)});
	CHECK(database.logs_to_download().empty());

	const int64_t id = sync.inserted.front();
	database.request({id}, {"local"});

	auto pending = database.logs_to_download();
	CHECK(pending.size() == 1);
	CHECK(pending.front().id == id);

	// Nothing to upload until the file is actually here.
	CHECK(database.logs_to_upload("local").empty());

	database.mark_downloaded(id, workspace.logs() + "a.ulg");
	CHECK(database.logs_to_download().empty());

	auto uploadable = database.logs_to_upload("local");
	CHECK(uploadable.size() == 1);
	// The request named only one target.
	CHECK(database.logs_to_upload("remote").empty());

	database.mark_uploaded(id, "local", "/plot_app?log=x");
	CHECK(database.logs_to_upload("local").empty());

	// Cancelling clears a pending request but not work already done.
	const int64_t other = sync.inserted.back();
	database.request({other}, {"local"});
	CHECK(database.logs_to_download().size() == 1);
	database.cancel_requests({other});
	CHECK(database.logs_to_download().empty());
	CHECK(database.log_by_id(id)->uploads.at("local").uploaded);
}

void a_downloaded_log_with_no_file_is_not_downloaded()
{
	Workspace workspace;
	LogDatabase database(workspace.db(), workspace.logs(), kTargets);

	const auto sync = database.sync_index({px4("a.ulg", 100, 1000)});
	const int64_t id = sync.inserted.front();

	database.request({id}, {"local"});
	database.mark_downloaded(id, workspace.logs() + "a.ulg");
	CHECK(database.logs_to_upload("local").size() == 1);

	// The file went away -- manual cleanup, a full disk, a failed upload.
	// The log has to become fetchable again, or it is stranded: reachable from
	// neither queue and invisible to "download everything".
	database.clear_local_file(id);
	CHECK(!database.log_by_id(id)->downloaded);
	CHECK(database.logs_to_upload("local").empty());

	const auto not_downloaded = database.ids_not_downloaded();
	CHECK(not_downloaded.size() == 1);
	CHECK(not_downloaded.front() == id);

	database.request({id}, {"local"});
	CHECK(database.logs_to_download().size() == 1);
}

void rejected_uploads_do_not_block_the_queue()
{
	Workspace workspace;
	LogDatabase database(workspace.db(), workspace.logs(), kTargets);

	const auto sync = database.sync_index({px4("a.ulg", 100, 1000), px4("b.ulg", 100, 2000)});

	for (int64_t id : sync.inserted) {
		database.request({id}, {"local"});
		database.mark_downloaded(id, workspace.logs() + std::to_string(id) + ".ulg");
	}

	CHECK(database.logs_to_upload("local").size() == 2);

	database.mark_upload_rejected(sync.inserted.front(), "local", "400: not a ulog");
	auto remaining = database.logs_to_upload("local");
	CHECK(remaining.size() == 1);
	CHECK(remaining.front().id == sync.inserted.back());

	// A transient failure leaves it queued.
	database.record_upload_failure(sync.inserted.back(), "local", "500");
	CHECK(database.logs_to_upload("local").size() == 1);
}

void failure_counts_order_the_download_queue()
{
	Workspace workspace;
	LogDatabase database(workspace.db(), workspace.logs(), kTargets);

	const auto sync = database.sync_index({px4("a.ulg", 100, 3000), px4("b.ulg", 100, 2000)});
	database.request(sync.inserted, {});

	// Newest first to begin with.
	CHECK(database.logs_to_download().front().path == "a.ulg");

	// A log the vehicle will not part with sorts to the back rather than
	// blocking everything behind it.
	database.record_download_failure(sync.inserted.front(), "timeout");
	CHECK(database.logs_to_download().front().path == "b.ulg");
}

void ardupilot_name_reuse_does_not_overwrite()
{
	Workspace workspace;
	LogDatabase database(workspace.db(), workspace.logs(), kTargets);

	const auto sync = database.sync_index({ardupilot("1.BIN", 100), ardupilot("1.BIN", 200)});
	CHECK(sync.inserted.size() == 2);

	const std::string path = workspace.logs() + "1.BIN";
	database.mark_downloaded(sync.inserted.front(), path);

	CHECK(database.local_path_in_use(path, sync.inserted.back()));
	CHECK(!database.local_path_in_use(path, sync.inserted.front()));
}

void ids_for_everything_respect_target_state()
{
	Workspace workspace;
	LogDatabase database(workspace.db(), workspace.logs(), kTargets);

	const auto sync = database.sync_index({px4("a.ulg", 100, 1000), px4("b.ulg", 100, 2000)});
	CHECK(database.ids_not_downloaded().size() == 2);

	for (int64_t id : sync.inserted) {
		database.mark_downloaded(id, workspace.logs() + std::to_string(id) + ".ulg");
	}

	CHECK(database.ids_not_downloaded().empty());
	CHECK(database.ids_not_uploaded({"local"}).size() == 2);

	database.mark_uploaded(sync.inserted.front(), "local", "/x");
	CHECK(database.ids_not_uploaded({"local"}).size() == 1);
	CHECK(database.ids_not_uploaded({"remote"}).size() == 2);
	CHECK(database.ids_not_uploaded({}).empty());
}

// -------------------------------------------------------------------------

// Builds a database in the shape the pre-FTP logloader left behind.
void write_legacy_database(const fs::path& file, const std::vector<std::tuple<std::string, int, std::string,
			   int64_t, bool, bool>>& rows)
{
	sqlite3* db = nullptr;
	CHECK(sqlite3_open(file.c_str(), &db) == SQLITE_OK);
	sqlite::execute(db, "CREATE TABLE logs (uuid TEXT PRIMARY KEY, id INTEGER, date TEXT, size_bytes INTEGER,"
			" downloaded INTEGER, uploaded INTEGER)");
	sqlite::execute(db, "CREATE TABLE blacklist (uuid TEXT PRIMARY KEY, reason TEXT, timestamp TEXT)");

	for (const auto& [uuid, legacy_id, date, size, downloaded, uploaded] : rows) {
		sqlite::Statement stmt(db, "INSERT INTO logs VALUES (?, ?, ?, ?, ?, ?)");
		stmt.bind(1, uuid).bind(2, static_cast<int64_t>(legacy_id)).bind(3, date)
		.bind(4, size).bind(5, downloaded).bind(6, uploaded);
		stmt.execute();
	}

	sqlite3_close(db);
}

void the_revision_moves_only_when_the_index_changes()
{
	Workspace workspace;
	LogDatabase database(workspace.db(), workspace.logs(), kTargets);

	database.sync_index({px4("2026-07-28/10_00_00.ulg", 100, 1785276000)});
	const uint64_t after_insert = database.revision();

	// An identical listing, which is what most of them are, is not a change.
	database.sync_index({px4("2026-07-28/10_00_00.ulg", 100, 1785276000)});
	CHECK(database.revision() == after_insert);

	// A card wiped clean is very much a change: the streams must hear that
	// every log is gone.
	database.sync_index({});
	CHECK(database.revision() > after_insert);

	// And so is the card coming back, even though no row is inserted.
	const uint64_t after_wipe = database.revision();
	database.sync_index({px4("2026-07-28/10_00_00.ulg", 100, 1785276000)});
	CHECK(database.revision() > after_wipe);
}

void the_upgrade_does_not_refetch_or_reupload()
{
	Workspace workspace;

	// Three logs: one fetched and uploaded, one fetched but not uploaded, one
	// neither. Their files are named the way the old version named them.
	write_legacy_database(workspace.path() / "local_server.db", {
		{"aaa", 1, "2026-07-28T10:00:00Z", 100, true, true},
		{"bbb", 2, "2026-07-28T11:00:00Z", 200, true, false},
		{"ccc", 3, "2026-07-28T12:00:00Z", 300, false, false},
	});

	const fs::path uploaded_file = fs::path(workspace.logs()) / "LOG0001_2026-07-28T10:00:00Z.ulg";
	const fs::path fetched_file = fs::path(workspace.logs()) / "LOG0002_2026-07-28T11:00:00Z.ulg";
	std::fclose(std::fopen(uploaded_file.c_str(), "w"));
	std::fclose(std::fopen(fetched_file.c_str(), "w"));

	LogDatabase database(workspace.db(), workspace.logs(), kTargets);
	CHECK(database.ok());

	// The FTP listing reports the same logs by size, with times close to what
	// the old database recorded.
	const auto sync = database.sync_index({px4("2026-07-28/10_00_00.ulg", 100, 1785276000),
					       px4("2026-07-28/11_00_00.ulg", 200, 1785279600),
					       px4("2026-07-28/12_00_00.ulg", 300, 1785283200)});
	CHECK(sync.inserted.size() == 3);

	const auto logs = database.all_logs();

	const auto* already_done = find(logs, "2026-07-28/10_00_00.ulg");
	CHECK(already_done != nullptr);
	CHECK(already_done->downloaded);
	CHECK(already_done->uploads.at("local").uploaded);

	const auto* fetched = find(logs, "2026-07-28/11_00_00.ulg");
	CHECK(fetched->downloaded);
	CHECK(!fetched->uploads.at("local").uploaded);

	const auto* untouched = find(logs, "2026-07-28/12_00_00.ulg");
	CHECK(!untouched->downloaded);
	CHECK(!untouched->uploads.at("local").uploaded);

	// The other target never had these logs.
	CHECK(!already_done->uploads.at("remote").uploaded);

	// Nothing the import adopted may carry intent: a fleet's worth of merely
	// downloaded logs queuing for upload on upgrade is exactly the flood the
	// import exists to avoid.
	CHECK(database.logs_to_download().empty());
	CHECK(database.logs_to_upload("local").empty());
	CHECK(database.logs_to_upload("remote").empty());
}

void the_upgrade_runs_once()
{
	Workspace workspace;
	write_legacy_database(workspace.path() / "local_server.db", {
		{"aaa", 1, "2026-07-28T10:00:00Z", 100, true, true},
	});

	{
		LogDatabase database(workspace.db(), workspace.logs(), kTargets);
		const auto sync = database.sync_index({px4("2026-07-28/10_00_00.ulg", 100, 1785276000)});
		database.mark_upload_rejected(sync.inserted.front(), "local", "operator dealt with it");
	}

	// Re-opening must not resurrect what the first run already applied.
	LogDatabase database(workspace.db(), workspace.logs(), kTargets);
	const auto logs = database.all_logs();
	CHECK(logs.size() == 1);
	CHECK(logs.front().uploads.at("local").rejected);
}

void a_missing_legacy_file_is_fetched_again()
{
	Workspace workspace;
	// Recorded as downloaded, but the file is not there any more.
	write_legacy_database(workspace.path() / "local_server.db", {
		{"aaa", 1, "2026-07-28T10:00:00Z", 100, true, false},
	});

	LogDatabase database(workspace.db(), workspace.logs(), kTargets);
	database.sync_index({px4("2026-07-28/10_00_00.ulg", 100, 1785276000)});

	const auto logs = database.all_logs();
	CHECK(!logs.front().downloaded);
	CHECK(logs.front().local_path.empty());
	CHECK(database.ids_not_downloaded().size() == 1);
}

// CREATE TABLE IF NOT EXISTS does nothing to a table that is already there, so a
// column added later is invisible on an existing database and every query naming
// it silently returns nothing.
void an_older_schema_is_migrated()
{
	Workspace workspace;

	{
		sqlite3* db = nullptr;
		CHECK(sqlite3_open(workspace.db().c_str(), &db) == SQLITE_OK);
		sqlite::execute(db,
				"CREATE TABLE logs ("
				"  id INTEGER PRIMARY KEY, path TEXT NOT NULL, size_bytes INTEGER NOT NULL,"
				"  time_utc INTEGER, local_path TEXT NOT NULL DEFAULT '',"
				"  downloaded INTEGER NOT NULL DEFAULT 0, download_requested INTEGER NOT NULL DEFAULT 0,"
				"  present INTEGER NOT NULL DEFAULT 1, download_failures INTEGER NOT NULL DEFAULT 0,"
				"  last_error TEXT NOT NULL DEFAULT '', discovered_at INTEGER NOT NULL DEFAULT 0,"
				"  UNIQUE (path, size_bytes))");
		sqlite::execute(db,
				"INSERT INTO logs (path, size_bytes, time_utc, discovered_at) "
				"VALUES ('old.ulg', 100, 1000, 7)");
		sqlite3_close(db);
	}

	LogDatabase database(workspace.db(), workspace.logs(), kTargets);
	CHECK(database.ok());

	const auto logs = database.all_logs();
	CHECK(logs.size() == 1);
	CHECK(logs.front().path == "old.ulg");

	// And the new column is usable, not just present.
	const auto sync = database.sync_index({px4("old.ulg", 100, 1000), px4("new.ulg", 200, 2000)});
	CHECK(sync.inserted.size() == 1);
	CHECK(database.newest_log_id().has_value());
}

void a_database_with_no_predecessor_is_untouched()
{
	Workspace workspace;
	LogDatabase database(workspace.db(), workspace.logs(), kTargets);
	const auto sync = database.sync_index({px4("a.ulg", 100, 1000)});
	CHECK(sync.inserted.size() == 1);
	CHECK(!database.log_by_id(sync.inserted.front())->downloaded);
}

} // namespace

int main()
{
	first_index_needs_a_non_empty_listing();
	newest_is_newest_for_both_stacks();
	a_log_that_leaves_the_vehicle_keeps_its_history();
	a_growing_log_is_still_on_the_vehicle();
	a_growing_log_is_a_different_log();
	requests_drive_the_queues();
	a_downloaded_log_with_no_file_is_not_downloaded();
	rejected_uploads_do_not_block_the_queue();
	failure_counts_order_the_download_queue();
	ardupilot_name_reuse_does_not_overwrite();
	ids_for_everything_respect_target_state();
	the_revision_moves_only_when_the_index_changes();

	an_older_schema_is_migrated();
	the_upgrade_does_not_refetch_or_reupload();
	the_upgrade_runs_once();
	a_missing_legacy_file_is_fetched_again();
	a_database_with_no_predecessor_is_untouched();

	std::cout << "all checks passed (" << g_checks << ")\n";
	return 0;
}

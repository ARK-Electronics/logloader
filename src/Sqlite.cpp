#include "Sqlite.hpp"
#include "Log.hpp"

namespace sqlite
{

Statement::Statement(sqlite3* db, const std::string& sql)
	: _db(db)
	, _sql(sql)
{
	if (_db == nullptr) {
		return;
	}

	if (sqlite3_prepare_v2(_db, sql.c_str(), -1, &_stmt, nullptr) != SQLITE_OK) {
		LOG_ERROR("SQL prepare failed: " << sqlite3_errmsg(_db) << " -- " << sql);
		_stmt = nullptr;
	}
}

Statement::~Statement()
{
	if (_stmt != nullptr) {
		sqlite3_finalize(_stmt);
	}
}

Statement& Statement::bind(int index, int64_t value)
{
	if (_stmt != nullptr) {
		sqlite3_bind_int64(_stmt, index, value);
	}

	return *this;
}

Statement& Statement::bind(int index, const std::string& value)
{
	if (_stmt != nullptr) {
		sqlite3_bind_text(_stmt, index, value.c_str(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
	}

	return *this;
}

Statement& Statement::bind(int index, bool value)
{
	return bind(index, static_cast<int64_t>(value ? 1 : 0));
}

Statement& Statement::bind(int index, std::optional<int64_t> value)
{
	if (_stmt == nullptr) {
		return *this;
	}

	if (value.has_value()) {
		sqlite3_bind_int64(_stmt, index, value.value());

	} else {
		sqlite3_bind_null(_stmt, index);
	}

	return *this;
}

bool Statement::step()
{
	if (_stmt == nullptr) {
		return false;
	}

	const int rc = sqlite3_step(_stmt);

	if (rc == SQLITE_ROW) {
		return true;
	}

	if (rc != SQLITE_DONE) {
		LOG_ERROR("SQL step failed: " << sqlite3_errmsg(_db) << " -- " << _sql);
	}

	return false;
}

bool Statement::execute()
{
	if (_stmt == nullptr) {
		return false;
	}

	const int rc = sqlite3_step(_stmt);

	if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
		LOG_ERROR("SQL step failed: " << sqlite3_errmsg(_db) << " -- " << _sql);
		return false;
	}

	return true;
}

int64_t Statement::column_int(int index) const
{
	return _stmt != nullptr ? sqlite3_column_int64(_stmt, index) : 0;
}

std::string Statement::column_text(int index) const
{
	if (_stmt == nullptr) {
		return {};
	}

	const unsigned char* text = sqlite3_column_text(_stmt, index);
	return text != nullptr ? reinterpret_cast<const char*>(text) : std::string {};
}

std::optional<int64_t> Statement::column_optional_int(int index) const
{
	if (_stmt == nullptr || sqlite3_column_type(_stmt, index) == SQLITE_NULL) {
		return std::nullopt;
	}

	return sqlite3_column_int64(_stmt, index);
}

bool execute(sqlite3* db, const std::string& sql)
{
	char* error = nullptr;

	if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
		LOG_ERROR("SQL failed: " << (error != nullptr ? error : "unknown") << " -- " << sql);
		sqlite3_free(error);
		return false;
	}

	return true;
}

bool table_exists(sqlite3* db, const std::string& table)
{
	Statement stmt(db, "SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' AND name = ?");
	stmt.bind(1, table);
	return stmt.step() && stmt.column_int(0) > 0;
}

bool column_exists(sqlite3* db, const std::string& table, const std::string& column)
{
	Statement stmt(db, "SELECT COUNT(*) FROM pragma_table_info(?) WHERE name = ?");
	stmt.bind(1, table).bind(2, column);
	return stmt.step() && stmt.column_int(0) > 0;
}

Transaction::Transaction(sqlite3* db)
	: _db(db)
{
	_open = execute(_db, "BEGIN");
}

Transaction::~Transaction()
{
	if (_open) {
		execute(_db, "ROLLBACK");
	}
}

bool Transaction::commit()
{
	if (!_open) {
		return false;
	}

	_open = false;
	return execute(_db, "COMMIT");
}

} // namespace sqlite

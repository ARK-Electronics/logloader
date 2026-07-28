#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <sqlite3.h>

// Thin RAII layer over the sqlite3 C API. The point is that a prepared
// statement finalizes itself, so a query cannot leak one by returning early.
namespace sqlite
{

// Bind indices are 1-based and column indices 0-based, matching sqlite3.
// Text and blob binds copy (SQLITE_TRANSIENT), so binding a temporary is safe.
class Statement
{
public:
	Statement(sqlite3* db, const std::string& sql);
	~Statement();

	Statement(const Statement&) = delete;
	Statement& operator=(const Statement&) = delete;

	// False when the statement failed to prepare; the error is already logged.
	explicit operator bool() const { return _stmt != nullptr; }

	Statement& bind(int index, int64_t value);
	Statement& bind(int index, const std::string& value);
	Statement& bind(int index, bool value);
	Statement& bind(int index, std::optional<int64_t> value);

	// True while rows remain.
	bool step();

	// Runs a statement that returns no rows. True when it completed.
	bool execute();

	int64_t column_int(int index) const;
	std::string column_text(int index) const;
	std::optional<int64_t> column_optional_int(int index) const;

private:
	sqlite3* _db {nullptr};
	sqlite3_stmt* _stmt {nullptr};
	std::string _sql;
};

// Runs statements that take no parameters (schema DDL, PRAGMA, transactions).
bool execute(sqlite3* db, const std::string& sql);

bool table_exists(sqlite3* db, const std::string& table);
bool column_exists(sqlite3* db, const std::string& table, const std::string& column);

// Rolls back unless commit() is called, so an early return cannot leave a
// half-applied multi-statement change behind.
class Transaction
{
public:
	explicit Transaction(sqlite3* db);
	~Transaction();

	Transaction(const Transaction&) = delete;
	Transaction& operator=(const Transaction&) = delete;

	bool commit();

private:
	sqlite3* _db {nullptr};
	bool _open {false};
};

} // namespace sqlite

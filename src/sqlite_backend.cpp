#include "sqlite_backend.h"

int SqliteBackend::init_database() {
    return db_.init_database();
}

int SqliteBackend::addLog(const std::string& uuid,
                          const std::string& id,
                          const std::string& name,
                          const double pos_x, const double pos_y, const double pos_z,
                          const std::string& world,
                          const std::string& obj_id,
                          const std::string& obj_name,
                          const long long time,
                          const std::string& type,
                          const std::string& data,
                          const std::string& status) {
    return db_.addLog(uuid, id, name, pos_x, pos_y, pos_z, world,
                      obj_id, obj_name, time, type, data, status);
}

int SqliteBackend::addLogs(const std::vector<DatabaseLogEntry>& entries) {
    // Convert DatabaseLogEntry vector to the tuple format expected by Database::addLogs
    std::vector<std::tuple<std::string, std::string, std::string, double, double, double,
                           std::string, std::string, std::string, long long,
                           std::string, std::string, std::string>> dbLogs;
    dbLogs.reserve(entries.size());
    for (const auto& e : entries) {
        dbLogs.emplace_back(e.uuid, e.id, e.name,
                            e.pos_x, e.pos_y, e.pos_z,
                            e.world, e.obj_id, e.obj_name,
                            e.time, e.type, e.data, e.status);
    }
    return db_.addLogs(dbLogs);
}

int SqliteBackend::searchLog(std::vector<std::map<std::string, std::string>>& result,
                             const std::pair<std::string, double>& key,
                             std::atomic<bool>* cancel) {
    return db_.searchLog(result, key, cancel);
}

int SqliteBackend::searchLog(std::vector<std::map<std::string, std::string>>& result,
                             const std::pair<std::string, double>& key,
                             const double x, const double y, const double z, const double r,
                             const std::string& world,
                             std::atomic<bool>* cancel) {
    return db_.searchLog(result, key, x, y, z, r, world, cancel);
}

bool SqliteBackend::updateStatusesByUUIDs(
    const std::vector<std::pair<std::string, std::string>>& pairs) {
    return db_.updateStatusesByUUIDs(pairs);
}

bool SqliteBackend::beginCleanup() {
    if (cleanup_db_) {
        sqlite3_close(cleanup_db_);
        cleanup_db_ = nullptr;
    }

    const std::string& filename = db_.getDbFilename();
    if (sqlite3_open(filename.c_str(), &cleanup_db_) != SQLITE_OK) {
        std::cerr << "[Tianyan] beginCleanup: open failed: "
                  << (cleanup_db_ ? sqlite3_errmsg(cleanup_db_) : "unknown") << std::endl;
        if (cleanup_db_) { sqlite3_close(cleanup_db_); cleanup_db_ = nullptr; }
        return false;
    }

    // 清理连接配置：不争内存，不缓存在 pool
    sqlite3_exec(cleanup_db_, "PRAGMA busy_timeout=30000;", nullptr, nullptr, nullptr);
    sqlite3_exec(cleanup_db_, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(cleanup_db_, "PRAGMA cache_size=-65536;",  nullptr, nullptr, nullptr);
    sqlite3_exec(cleanup_db_, "PRAGMA temp_store=FILE;",    nullptr, nullptr, nullptr);
    return true;
}

int SqliteBackend::cleanupDeleteBatch(const long long timestamp, const int limit) {
    if (!cleanup_db_) return -1;
    if (limit <= 0) return 0;

    const std::string sql =
        "DELETE FROM LOGDATA WHERE rowid IN ("
        "SELECT rowid FROM LOGDATA WHERE time < ? "
        "ORDER BY time, rowid LIMIT ?"
        ");";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(cleanup_db_, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        std::cerr << "[Tianyan] cleanupDeleteBatch prepare: " << sqlite3_errmsg(cleanup_db_)
                  << std::endl;
        sqlite3_finalize(stmt);
        return -1;
    }

    sqlite3_bind_int64(stmt, 1, timestamp);
    sqlite3_bind_int(stmt, 2, limit);

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        std::cerr << "[Tianyan] cleanupDeleteBatch step: " << sqlite3_errmsg(cleanup_db_)
                  << std::endl;
        sqlite3_finalize(stmt);
        return -1;
    }

    const int deleted = sqlite3_changes(cleanup_db_);
    sqlite3_finalize(stmt);
    return deleted;
}

bool SqliteBackend::cleanupCheckpoint() {
    if (!cleanup_db_) return false;
    char* err = nullptr;
    if (const int rc = sqlite3_exec(cleanup_db_, "PRAGMA wal_checkpoint(TRUNCATE);", nullptr, nullptr, &err); rc != SQLITE_OK) {
        std::cerr << "[Tianyan] cleanupCheckpoint: " << (err ? err : "") << std::endl;
        sqlite3_free(err);
        return false;
    }
    return true;
}

bool SqliteBackend::abortCleanup() {
    if (!cleanup_db_) return true;

    sqlite3_exec(cleanup_db_, "PRAGMA wal_checkpoint(TRUNCATE);", nullptr, nullptr, nullptr);
    sqlite3_close(cleanup_db_);
    cleanup_db_ = nullptr;
    return true;
}

bool SqliteBackend::endCleanup() {
    if (!cleanup_db_) return false;

    // 截断 WAL
    char* err = nullptr;
    sqlite3_exec(cleanup_db_, "PRAGMA wal_checkpoint(TRUNCATE);", nullptr, nullptr, &err);
    sqlite3_free(err);

    // 检查碎片率，只有超过 20% 才值得重建全库
    int64_t free_pages = 0, total_pages = 1;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(cleanup_db_, "PRAGMA freelist_count;", -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) free_pages = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
    }
    if (sqlite3_prepare_v2(cleanup_db_, "PRAGMA page_count;", -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) total_pages = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
    }

    if (const double frag_ratio = total_pages > 0 ? static_cast<double>(free_pages) / static_cast<double>(total_pages) : 0.0; frag_ratio < 0.20) {
        sqlite3_close(cleanup_db_);
        cleanup_db_ = nullptr;
        return true;
    }

    // 关闭清理连接，释放所有缓存的 dirty pages
    sqlite3_close(cleanup_db_);
    cleanup_db_ = nullptr;

    // 用全新最小连接跑 VACUUM，避免携带 DELETE 阶段的缓存
    const std::string& filename = db_.getDbFilename();
    sqlite3* vacuum_db = nullptr;
    if (sqlite3_open(filename.c_str(), &vacuum_db) != SQLITE_OK) {
        std::cerr << "[Tianyan] endCleanup: open vacuum db failed" << std::endl;
        if (vacuum_db) sqlite3_close(vacuum_db);
        return false;
    }

    sqlite3_exec(vacuum_db, "PRAGMA busy_timeout=30000;", nullptr, nullptr, nullptr);
    sqlite3_exec(vacuum_db, "PRAGMA cache_size=-8000;",   nullptr, nullptr, nullptr);
    sqlite3_exec(vacuum_db, "PRAGMA temp_store=FILE;",    nullptr, nullptr, nullptr);
    sqlite3_exec(vacuum_db, "PRAGMA synchronous=OFF;",    nullptr, nullptr, nullptr);

    int rc = sqlite3_exec(vacuum_db, "VACUUM;", nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::cerr << "[Tianyan] endCleanup VACUUM: " << (err ? err : "") << std::endl;
        sqlite3_free(err);
        sqlite3_close(vacuum_db);
        return false;
    }

    sqlite3_close(vacuum_db);
    return true;
}

int64_t SqliteBackend::getCleanCount(const long long timestamp) {
    return db_.getCleanCount(timestamp);
}

int SqliteBackend::deleteBatch(const long long timestamp, const int limit) {
    return db_.deleteBatch(timestamp, limit);
}

std::string SqliteBackend::generateUuid() {
    return db_util::generate_uuid_v4();
}

int SqliteBackend::executeSQL(const std::string& sql) {
    return db_.executeSQL(sql);
}

int SqliteBackend::querySQL(const std::string& sql,
                            std::vector<std::map<std::string, std::string>>& result) {
    return db_.querySQL(sql, result);
}

int SqliteBackend::updateSQL(const std::string& table,
                             const std::string& set_clause,
                             const std::string& where_clause) {
    return db_.updateSQL(table, set_clause, where_clause);
}

bool SqliteBackend::isValueExists(const std::string& tableName,
                                  const std::string& columnName,
                                  const std::string& value) {
    return db_.isValueExists(tableName, columnName, value);
}

bool SqliteBackend::updateValue(const std::string& tableName,
                                const std::string& targetColumn,
                                const std::string& newValue,
                                const std::string& conditionColumn,
                                const std::string& conditionValue) {
    return db_.updateValue(tableName, targetColumn, newValue, conditionColumn, conditionValue);
}

bool SqliteBackend::updateStatusByUUID(const std::string& uuid,
                                       const std::string& newStatus) {
    return db_.updateStatusByUUID(uuid, newStatus);
}

int SqliteBackend::getAllLog(std::vector<std::map<std::string, std::string>>& result) {
    return db_.getAllLog(result);
}

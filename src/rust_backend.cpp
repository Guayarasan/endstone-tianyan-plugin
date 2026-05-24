#include "rust_backend.h"

#include "database.hpp"

#include <chrono>
#include <iostream>
#include <vector>

// ============================================================
// C FFI declarations (from librust_mysql.a)
// ============================================================

extern "C" {
    // Lifecycle
    void* rust_mysql_new(const char* host, uint16_t port,
                         const char* user, const char* pass,
                         const char* db);
    void rust_mysql_free(void* handle);

    // Connection
    bool rust_mysql_connect(void* handle);
    bool rust_mysql_disconnect(void* handle);
    bool rust_mysql_is_connected(const void* handle);

    // Query (returns opaque result ptr, or nullptr on error)
    void* rust_mysql_query(void* handle, const char* sql);
    void* rust_mysql_query_params(void* handle, const char* sql,
                                   const char* const* params, int32_t param_count);
    void* rust_mysql_query_params_with_cancel(void* handle, const char* sql,
                                               const char* const* params,
                                               int32_t param_count,
                                               bool (*cancel_fn)(void*),
                                               void* cancel_ctx);
    int64_t rust_mysql_execute(void* handle, const char* sql);
    int64_t rust_mysql_execute_params(void* handle, const char* sql,
                                       const char* const* params,
                                       int32_t param_count);

    // Result access
    int32_t rust_mysql_result_rows(const void* result);
    int32_t rust_mysql_result_cols(const void* result);
    const char* rust_mysql_result_col_name(const void* result, int32_t col);
    const char* rust_mysql_result_value(const void* result,
                                         int32_t row, int32_t col);
    void rust_mysql_free_result(void* result);

    // Error & utility
    const char* rust_mysql_last_error(const void* handle);
    char* rust_mysql_generate_uuid();
    void rust_mysql_free_string(char* s);
}

// ============================================================
// Cancel callback trampoline
// ============================================================

static bool cancel_callback(void* ctx) {
    auto* flag = static_cast<std::atomic<bool>*>(ctx);
    return flag && flag->load();
}

// ============================================================
// Construction / destruction
// ============================================================

RustBackend::RustBackend(const RustMySQLConfig& config) {
    handle_ = rust_mysql_new(
        config.host.c_str(),
        config.port,
        config.user.c_str(),
        config.password.c_str(),
        config.database.c_str()
    );
    if (handle_) {
        connected_ = rust_mysql_connect(handle_);
        if (!connected_) {
            std::cerr << "[Tianyan][RustMySQL] Connect failed: "
                      << lastError() << std::endl;
        }
    } else {
        std::cerr << "[Tianyan][RustMySQL] Failed to create handle" << std::endl;
    }
}

RustBackend::~RustBackend() {
    if (handle_) {
        rust_mysql_disconnect(handle_);
        rust_mysql_free(handle_);
    }
}

// ============================================================
// Error helper
// ============================================================

std::string RustBackend::lastError() const {
    if (!handle_) return "null handle";
    const char* err = rust_mysql_last_error(handle_);
    return err ? std::string(err) : "";
}

// ============================================================
// Result conversion helpers
// ============================================================

int RustBackend::resultToMaps(
    void* result,
    std::vector<std::map<std::string, std::string>>& out)
{
    if (!result) return -1;

    int rows = rust_mysql_result_rows(result);
    int cols = rust_mysql_result_cols(result);

    // Collect column names
    std::vector<std::string> col_names;
    col_names.reserve(cols);
    for (int c = 0; c < cols; ++c) {
        const char* name = rust_mysql_result_col_name(result, c);
        col_names.emplace_back(name ? name : "");
    }

    out.clear();
    out.reserve(rows);
    for (int r = 0; r < rows; ++r) {
        std::map<std::string, std::string> row;
        for (int c = 0; c < cols; ++c) {
            const char* val = rust_mysql_result_value(result, r, c);
            row[col_names[c]] = val ? val : "";
        }
        out.push_back(std::move(row));
    }

    rust_mysql_free_result(result);
    return 0;
}

int RustBackend::queryToMaps(
    const std::string& sql,
    std::vector<std::map<std::string, std::string>>& result,
    const std::vector<std::string>& params) const
{
    if (!handle_) return -1;

    void* qr;
    if (params.empty()) {
        qr = rust_mysql_query(handle_, sql.c_str());
    } else {
        // Build C string array
        std::vector<const char*> c_params;
        c_params.reserve(params.size());
        for (const auto& p : params) {
            c_params.push_back(p.c_str());
        }
        qr = rust_mysql_query_params(handle_, sql.c_str(),
                                      c_params.data(),
                                      static_cast<int32_t>(c_params.size()));
    }
    if (!qr) {
        std::cerr << "[Tianyan][RustMySQL] query failed: " << lastError()
                  << "\n  SQL: " << sql.substr(0, 200) << std::endl;
        return -1;
    }
    return resultToMaps(qr, result);
}

int RustBackend::queryToMapsWithCancel(
    const std::string& sql,
    std::vector<std::map<std::string, std::string>>& result,
    const std::vector<std::string>& params,
    std::atomic<bool>* cancel) const
{
    if (!handle_ || !cancel) return queryToMaps(sql, result, params);
    if (cancel->load()) return -1;

    std::vector<const char*> c_params;
    c_params.reserve(params.size());
    for (const auto& p : params) {
        c_params.push_back(p.c_str());
    }

    void* qr = rust_mysql_query_params_with_cancel(
        handle_, sql.c_str(),
        c_params.data(), static_cast<int32_t>(c_params.size()),
        &cancel_callback, cancel);

    if (!qr) {
        if (cancel->load()) return -1; // cancelled
        std::cerr << "[Tianyan][RustMySQL] cancel query failed: "
                  << lastError() << std::endl;
        return -1;
    }
    return resultToMaps(qr, result);
}

// ============================================================
// IDatabaseBackend implementation
// ============================================================

int RustBackend::init_database() {
    if (!handle_) return -1;

    // Create LOGDATA table
    const char* create_sql =
        "CREATE TABLE IF NOT EXISTS LOGDATA ("
        "uuid VARCHAR(36) PRIMARY KEY, "
        "id VARCHAR(255), "
        "name VARCHAR(255), "
        "pos_x DOUBLE, pos_y DOUBLE, pos_z DOUBLE, "
        "world VARCHAR(255), "
        "obj_id VARCHAR(255), "
        "obj_name VARCHAR(255), "
        "time BIGINT, "
        "type VARCHAR(255), "
        "data TEXT, "
        "status VARCHAR(255)"
        ")";

    if (rust_mysql_execute(handle_, create_sql) < 0) {
        std::cerr << "[Tianyan][RustMySQL] init_database create table failed: "
                  << lastError() << std::endl;
        return -1;
    }

    // Create indexes (ignore errors if they already exist)
    rust_mysql_execute(handle_,
        "CREATE INDEX IF NOT EXISTS idx_logdata_pos "
        "ON LOGDATA(pos_x, pos_y, pos_z)");
    rust_mysql_execute(handle_,
        "CREATE INDEX IF NOT EXISTS idx_logdata_time ON LOGDATA(time)");

    return 0;
}

int RustBackend::addLog(
    const std::string& uuid, const std::string& id, const std::string& name,
    double pos_x, double pos_y, double pos_z,
    const std::string& world, const std::string& obj_id,
    const std::string& obj_name, long long time,
    const std::string& type, const std::string& data,
    const std::string& status)
{
    if (!handle_) return -1;

    const std::string sql =
        "INSERT IGNORE INTO LOGDATA (uuid, id, name, pos_x, pos_y, pos_z, "
        "world, obj_id, obj_name, time, type, data, status) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";

    std::vector<std::string> params = {
        uuid, id, name,
        std::to_string(pos_x), std::to_string(pos_y), std::to_string(pos_z),
        world, obj_id, obj_name,
        std::to_string(time), type, data, status
    };

    std::vector<const char*> c_params;
    c_params.reserve(params.size());
    for (const auto& p : params) {
        c_params.push_back(p.c_str());
    }

    int64_t ret = rust_mysql_execute_params(
        handle_, sql.c_str(),
        c_params.data(), static_cast<int32_t>(c_params.size()));
    return ret >= 0 ? 0 : -1;
}

int RustBackend::addLogs(const std::vector<DatabaseLogEntry>& entries) {
    if (entries.empty()) return 0;
    if (!handle_) return -1;

    // Use a transaction for batch insert
    rust_mysql_execute(handle_, "START TRANSACTION");

    const std::string sql =
        "INSERT IGNORE INTO LOGDATA (uuid, id, name, pos_x, pos_y, pos_z, "
        "world, obj_id, obj_name, time, type, data, status) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";

    bool failed = false;
    for (const auto& e : entries) {
        std::vector<std::string> params = {
            e.uuid, e.id, e.name,
            std::to_string(e.pos_x), std::to_string(e.pos_y), std::to_string(e.pos_z),
            e.world, e.obj_id, e.obj_name,
            std::to_string(e.time), e.type, e.data, e.status
        };
        std::vector<const char*> c_params;
        c_params.reserve(params.size());
        for (const auto& p : params) {
            c_params.push_back(p.c_str());
        }

        if (rust_mysql_execute_params(handle_, sql.c_str(),
                                       c_params.data(),
                                       static_cast<int32_t>(c_params.size())) < 0) {
            failed = true;
            break;
        }
    }

    if (failed) {
        rust_mysql_execute(handle_, "ROLLBACK");
        return -1;
    }

    rust_mysql_execute(handle_, "COMMIT");
    return 0;
}

int RustBackend::searchLog(
    std::vector<std::map<std::string, std::string>>& result,
    const std::pair<std::string, double>& key,
    std::atomic<bool>* cancel)
{
    const std::string sql =
        "SELECT * FROM LOGDATA WHERE "
        "(name LIKE ? OR type LIKE ? OR data LIKE ?) AND "
        "time >= UNIX_TIMESTAMP() - ? "
        "ORDER BY time";

    std::vector<std::string> params = {
        "%" + key.first + "%",
        "%" + key.first + "%",
        "%" + key.first + "%",
        std::to_string(static_cast<long long>(key.second * 3600))
    };

    if (cancel) {
        return queryToMapsWithCancel(sql, result, params, cancel);
    }
    return queryToMaps(sql, result, params);
}

int RustBackend::searchLog(
    std::vector<std::map<std::string, std::string>>& result,
    const std::pair<std::string, double>& key,
    double x, double y, double z, double r,
    const std::string& world,
    std::atomic<bool>* cancel)
{
    const std::string sql =
        "SELECT * FROM LOGDATA WHERE "
        "(name LIKE ? OR type LIKE ? OR data LIKE ?) AND "
        "time >= UNIX_TIMESTAMP() - ? AND "
        "world = ? AND "
        "pos_x >= ? AND pos_x <= ? AND "
        "pos_y >= ? AND pos_y <= ? AND "
        "pos_z >= ? AND pos_z <= ? AND "
        "(POW(pos_x - ?, 2) + POW(pos_y - ?, 2) + POW(pos_z - ?, 2)) <= ? "
        "ORDER BY time";

    std::vector<std::string> params = {
        "%" + key.first + "%",
        "%" + key.first + "%",
        "%" + key.first + "%",
        std::to_string(static_cast<long long>(key.second * 3600)),
        world,
        std::to_string(x - r), std::to_string(x + r),
        std::to_string(y - r), std::to_string(y + r),
        std::to_string(z - r), std::to_string(z + r),
        std::to_string(x), std::to_string(y), std::to_string(z),
        std::to_string(r * r)
    };

    if (cancel) {
        return queryToMapsWithCancel(sql, result, params, cancel);
    }
    return queryToMaps(sql, result, params);
}

bool RustBackend::updateStatusesByUUIDs(
    const std::vector<std::pair<std::string, std::string>>& pairs)
{
    if (pairs.empty()) return true;
    if (!handle_) return false;

    rust_mysql_execute(handle_, "START TRANSACTION");

    for (const auto& [uuid, status] : pairs) {
        std::vector<const char*> c_params = {status.c_str(), uuid.c_str()};
        if (rust_mysql_execute_params(
                handle_,
                "UPDATE LOGDATA SET status = ? WHERE uuid = ?",
                c_params.data(), 2) < 0) {
            rust_mysql_execute(handle_, "ROLLBACK");
            return false;
        }
    }

    rust_mysql_execute(handle_, "COMMIT");
    return true;
}

bool RustBackend::cleanDataBase(double hours) {
    if (!handle_) return false;

    yuhangle::clean_data_status = 2;
    auto start_time = std::chrono::high_resolution_clock::now();

    const long long current_time = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const long long threshold = current_time - static_cast<long long>(hours * 3600);
    const std::string threshold_str = std::to_string(threshold);

    // Count rows to be deleted
    std::vector<std::map<std::string, std::string>> count_result;
    if (queryToMaps(
            "SELECT COUNT(*) AS cnt FROM LOGDATA WHERE time < ?",
            count_result, {threshold_str}) != 0) {
        yuhangle::clean_data_status = -1;
        return false;
    }

    int deleted_count = 0;
    if (!count_result.empty()) {
        auto it = count_result[0].find("cnt");
        if (it != count_result[0].end()) {
            deleted_count = std::stoi(it->second);
        }
    }

    // Delete old rows
    const std::vector<const char*> c_params = {threshold_str.c_str()};
    const int64_t affected = rust_mysql_execute_params(
        handle_, "DELETE FROM LOGDATA WHERE time < ?",
        c_params.data(), 1);

    if (affected < 0) {
        yuhangle::clean_data_message.clear();
        yuhangle::clean_data_message.emplace_back("MySQL delete failed");
        yuhangle::clean_data_status = -1;
        return false;
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    double seconds = std::chrono::duration<double>(duration).count();

    yuhangle::clean_data_message.clear();
    yuhangle::clean_data_message.emplace_back("Time elapsed: ");
    yuhangle::clean_data_message.emplace_back(std::to_string(seconds));
    yuhangle::clean_data_message.emplace_back("Number of cleaned logs: ");
    yuhangle::clean_data_message.emplace_back(std::to_string(deleted_count));

    yuhangle::clean_data_status = 1;
    return true;
}

std::string RustBackend::generateUuid() {
    char* uuid_cstr = rust_mysql_generate_uuid();
    if (!uuid_cstr) return "";
    std::string result(uuid_cstr);
    rust_mysql_free_string(uuid_cstr);
    return result;
}

int RustBackend::executeSQL(const std::string& sql) {
    if (!handle_) return -1;
    int64_t affected = rust_mysql_execute(handle_, sql.c_str());
    return affected >= 0 ? 0 : -1;
}

int RustBackend::querySQL(
    const std::string& sql,
    std::vector<std::map<std::string, std::string>>& result)
{
    return queryToMaps(sql, result);
}

int RustBackend::updateSQL(
    const std::string& table,
    const std::string& set_clause,
    const std::string& where_clause)
{
    if (!handle_) return -1;
    const std::string sql =
        "UPDATE " + table + " SET " + set_clause + " WHERE " + where_clause;
    int64_t affected = rust_mysql_execute(handle_, sql.c_str());
    return affected >= 0 ? 0 : -1;
}

bool RustBackend::isValueExists(
    const std::string& tableName,
    const std::string& columnName,
    const std::string& value)
{
    std::vector<std::map<std::string, std::string>> result;
    int rc = queryToMaps(
        "SELECT COUNT(*) AS cnt FROM " + tableName +
        " WHERE " + columnName + " = ?",
        result, {value});

    if (rc != 0 || result.empty()) return false;
    auto it = result[0].find("cnt");
    if (it == result[0].end()) return false;
    return std::stoi(it->second) > 0;
}

bool RustBackend::updateValue(
    const std::string& tableName,
    const std::string& targetColumn,
    const std::string& newValue,
    const std::string& conditionColumn,
    const std::string& conditionValue)
{
    if (!handle_) return false;

    const std::string sql = "UPDATE " + tableName +
                            " SET " + targetColumn + " = ?" +
                            " WHERE " + conditionColumn + " = ?";

    std::vector<const char*> c_params = {newValue.c_str(), conditionValue.c_str()};
    int64_t affected = rust_mysql_execute_params(
        handle_, sql.c_str(), c_params.data(), 2);
    return affected > 0;
}

bool RustBackend::updateStatusByUUID(
    const std::string& uuid,
    const std::string& newStatus)
{
    if (!handle_) return false;

    std::vector<const char*> c_params = {newStatus.c_str(), uuid.c_str()};
    int64_t affected = rust_mysql_execute_params(
        handle_,
        "UPDATE LOGDATA SET status = ? WHERE uuid = ?",
        c_params.data(), 2);
    return affected >= 0;
}

int RustBackend::getAllLog(
    std::vector<std::map<std::string, std::string>>& result)
{
    return queryToMaps("SELECT * FROM LOGDATA", result);
}

//
// Created by yuhang on 2026/3/24.
//

#pragma once
#include <string>
#include <vector>
#include <future>

namespace tianyan {

    //Tianyan api Version
    inline constexpr int TIANYAN_API_VERSION = 1;

    // LogData struct
    struct LogData {
        std::string uuid;
        std::string id;
        std::string name;
        double pos_x;
        double pos_y;
        double pos_z;
        std::string world;
        std::string obj_id;
        std::string obj_name;
        long long time;
        std::string type;
        std::string data;
        std::string status;
    };

    class ITianyanAPI : public endstone::Service {
    public:

        // --- 版本控制接口 ---

        /**
         * @brief Get API Version
         * @return INT Version
         */
        virtual int getApiVersion() const = 0;

        /**
         * @brief Check if the current header file version is compatible with the plugin version
         * @return Returns true if plugin version >= current header version, indicating compatibility
         */
        bool isCompatible() const {
            return getApiVersion() >= TIANYAN_API_VERSION;
        }

        // --- 业务接口 ---

        /**
         * @brief [Synchronous] Retrieve log data within a specified time period (subject to data limit)
         * @note When calling from the main thread, note that there may still be minor fluctuations
         *       if the data volume is large.
         * @param hours Number of seconds to query records from the past
         * @param limit Maximum number of records to return, defaults to 10000 (consistent with the tys command)
         */
        std::vector<LogData> getLogDataSync(const double hours, const int limit = 10000) {
            if (!isCompatible()) return {};
            return getLogDataSyncImpl(hours, limit);
        }

        /**
         * @brief [Asynchronous] Retrieve log data within a specified time period (no size limit)
         * @return std::future handle, returns the complete result when .get() is called
         */
        virtual std::future<std::vector<LogData>> getLogDataAsync(double hours) = 0;
private:
        virtual std::vector<LogData> getLogDataSyncImpl(double hours, int limit) = 0;
    };

} // namespace tianyan
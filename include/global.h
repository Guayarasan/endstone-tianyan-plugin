//
// Created by yuhang on 2025/11/6.
//

#ifndef TIANYAN_GLOBAL_H
#define TIANYAN_GLOBAL_H
#include "tianyan_core.h"
#include <nlohmann/json.hpp>
#include <condition_variable>
using namespace nlohmann;
//日志缓存
inline vector<TianyanCore::LogData> logDataCache;
//封禁设备ID玩家缓存
inline vector<TianyanCore::BanIDPlayer> BanIDPlayers;
//缓存锁
inline std::mutex cacheMutex;
//数据库状态
inline std::atomic is_db_over = false;

namespace yuhangle {
//数据库迁移状态 0=空闲 1=进行中 2=成功 -1=失败
inline std::atomic migrate_status = 0;
inline std::vector<std::string> migrate_message;
inline int migrate_total = 0;
inline int migrate_progress = 0;
inline std::string migrate_source_type;
inline std::string migrate_target_type;
inline std::string migrate_sender_name;

//数据库清理状态 0=空闲 1=成功 -1=失败 2=进行中
inline std::atomic<int> clean_data_status = 0;
inline std::vector<std::string> clean_data_message;
inline std::string clean_data_sender_name;
inline int64_t clean_data_total = 0;
inline int64_t clean_data_progress = 0;
} // namespace yuhangle

#endif //TIANYAN_GLOBAL_H
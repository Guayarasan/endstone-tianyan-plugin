#ifndef TIANYAN_TRANSLATE_H
#define TIANYAN_TRANSLATE_H

#include <fstream>
#include <nlohmann/json.hpp>
#include <utility>
#include <fmt/format.h>
#include <string>

class translate {
public:
    translate() = default;

    explicit translate(std::string lang_file) : lang_file_(std::move(lang_file)) {
        loadLanguage();
    };

    std::pair<bool, std::string> loadLanguage(std::string new_file = "") {
        if (!new_file.empty()) {
            lang_file_ = std::move(new_file);
        }

        if (lang_file_.empty()) {
            return {false, "No language file path provided."};
        }

        try {
            if (std::ifstream f(lang_file_); f.is_open()) {
                languageResource = json::parse(f);
                return {true, "Language file loaded successfully."};
            }
        } catch (const std::exception& e) {
            languageResource.clear();
            return {false, std::string("JSON Error: ") + e.what()};
        }

        return {false, "File not found or cannot be opened."};
    }

    // 获取本地化字符串
    [[nodiscard]] std::string getLocal(const std::string &key) const {
        // 如果资源库为空，或者找不到 key，直接返回原始 key
        if (!languageResource.is_null() && languageResource.contains(key)) {
            return languageResource[key].get<std::string>();
        }
        return key;
    }

    template<typename... Args>
    std::string tr(const std::string& key, Args&&... args) const {
        std::string pattern = getLocal(key);
        try {
            return fmt::vformat(pattern, fmt::make_format_args(args...));
        } catch (...) {
            return std::move(pattern);
        }
    }

private:
    std::string lang_file_;
    json languageResource;
};
#endif //TIANYAN_TRANSLATE_H
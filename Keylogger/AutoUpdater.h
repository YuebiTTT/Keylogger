#ifndef AUTO_UPDATER_H
#define AUTO_UPDATER_H

#include <string>

class AutoUpdater {
private:
    std::string current_version_;  // 当前版本

public:
    // 构造函数，指定当前版本（服务器主动推送更新，无需API地址）
    AutoUpdater(const std::string& current_version = "1.0.0");

    // 析构函数
    ~AutoUpdater();

    // 下载更新文件
    bool downloadUpdate(const std::string& download_url, const std::string& save_path);

    // 安装更新
    bool installUpdate(const std::string& update_file_path);

    // 设置当前版本
    void setCurrentVersion(const std::string& version) { current_version_ = version; }

    // 获取当前版本
    std::string getCurrentVersion() const { return current_version_; }
};

#endif // AUTO_UPDATER_H
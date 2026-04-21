#ifndef AUTO_UPDATER_H
#define AUTO_UPDATER_H

#include <string>

class AutoUpdater {
private:
    std::string api_base_url_;  // API基础地址
    std::string current_version_;  // 当前版本

public:
    // 构造函数，指定API基础地址和当前版本
    AutoUpdater(const std::string& api_base = "http://10.88.202.73:5244", 
                const std::string& current_version = "1.0.0");

    // 析构函数
    ~AutoUpdater();

    // 检查是否有更新
    bool checkForUpdates(std::string& latest_version, std::string& download_url);

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
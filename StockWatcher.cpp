
#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <process.h>

#include <curl/curl.h>
#include <vector>
#include <regex>
#include <fstream>
#include <codecvt>
#include <locale>

#include "Resource.h"
#include "Network.h"

#pragma comment (lib, "dwmapi.lib")
#pragma comment (lib, "libcurl.lib")
#pragma comment (lib, "libcrypto.lib")
#pragma comment (lib, "libssl.lib")


LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
void PositionWindowAboveTaskbar(HWND hwnd);
void CreateContextMenu(HWND hwnd, POINT pt);
void CreateTrayContextMenu(HWND hwnd, POINT pt);
void DisplayStock(HDC hdc, HWND hwnd, HDC g_hdcMem);

// 메뉴 항목 ID
#define IDM_CLOSE    1001
#define IDM_COPY     1002
#define IDM_OPTION1  1003
#define IDM_OPTION2  1004
//#define IDM_EXIT     1005
#define IDM_SHOW     1006
#define WM_TRAYICON  (WM_USER + 1)

const wchar_t* g_appname = L"StockWatcher";
const int WINDOW_WIDTH = 200;
const int WINDOW_HEIGHT = 30;
const int SCROLL_TIMER_ID = 3;
const int SCROLL_SPEED = 50;    // 스크롤 타이머 간격 (ms)


enum SCROLLTYPE
{
    LR_SIDE,
    UPDOWN
};

SCROLLTYPE g_scrollType = SCROLLTYPE::UPDOWN;

int g_waitcount = 0;
int g_scrollX = 0;
int g_scrollY = 0;
int g_currentStock = 0;

HFONT hFont;
HDC g_hdcMem = NULL;           // 더블 버퍼링용
HBITMAP g_hbmMem = NULL;
HGDIOBJ g_hbmOld = NULL;
NOTIFYICONDATA g_nid = { 0 };

CRITICAL_SECTION cs;
std::wstring stockvalue = L"";

CurlSession curl;
std::string readBuffer;

enum RISE
{
    RISE_NONE,
    RISE_UP,
    RISE_DOWN,
};

struct STOCKINFO
{
    std::string name;
    std::string code;
    std::string value;
    RISE rise;

    STOCKINFO()
    {
        value = "?????";
        rise = RISE::RISE_NONE;
    }
};

std::vector<STOCKINFO> stocklist;
int stockreadTime = 1000 * 60;  // 1min
bool forceTrack = false;

int readcount = 0;

std::wstring StringToWString(const std::string& str) 
{
    if (str.empty()) {
        return std::wstring();
    }

    int size = MultiByteToWideChar(CP_ACP, 0, str.c_str(),
        static_cast<int>(str.size()), nullptr, 0);

    std::wstring wstr(size, 0);
    MultiByteToWideChar(CP_ACP, 0, str.c_str(),
        static_cast<int>(str.size()), &wstr[0], size);

    return wstr;
}


std::vector<std::string> SplitLines(const std::string& text)
{
    std::vector<std::string> lines;
    size_t start = 0;
    size_t end = 0;

    while ((end = text.find('\n', start)) != std::string::npos) 
    {
        std::string line = text.substr(start, end - start);

        // 앞뒤 공백 제거
        line.erase(0, line.find_first_not_of(" \t\r\n"));
        line.erase(line.find_last_not_of(" \t\r\n") + 1);

        if (!line.empty()) 
        {
            lines.push_back(line);
        }
        start = end + 1;
    }

    // 마지막 라인 처리
    std::string lastLine = text.substr(start);
    lastLine.erase(0, lastLine.find_first_not_of(" \t\r\n"));
    lastLine.erase(lastLine.find_last_not_of(" \t\r\n") + 1);
    if (!lastLine.empty()) {
        lines.push_back(lastLine);
    }

    return lines;
}

bool FindSequentialStrings(const std::vector<std::string>& lines, int& startIndex, bool& up)
{
    const std::vector<std::string> targetStrings =
    {
        "<div class=\"today\">",
        "<p class=\"no_today\">",
    };


    const std::vector<std::string> updown =
    {
        "<em class=\"no_up\">",
        "<em class=\"no_down\">"
    };

    for (size_t i = 0; i < lines.size(); ++i)
    {
        bool found = true;

        if (i + targetStrings.size() > lines.size())
            return false;

        for (size_t j = 0; j < targetStrings.size(); ++j)
        {
            if (lines[i + j].find(targetStrings[j]) == std::string::npos)
            {
                found = false;
                break;
            }
        }

        if (found) 
        {
            if (lines[i + 2].find(updown[0]) == std::string::npos && lines[i + 2].find(updown[1]) == std::string::npos)
                return false;

            if (lines[i + 2].find(updown[0]) == std::string::npos)
                up = false;
            else
                up = true;

            startIndex = i;
            return true;
        }
    }

    return false;
}

std::string ExtractNumber(const std::string& html)
{
    std::regex pattern("<span class=\"blind\">(.*?)</span>");
    std::smatch matches;

    if (std::regex_search(html, matches, pattern)) {
        return matches[1].str(); // 첫 번째 캡처 그룹 반환
    }

    return "";
}

std::vector<std::string> SplitString(const std::string& str, char delimiter) 
{
    std::vector<std::string> tokens;
    size_t start = 0;
    size_t end = str.find(delimiter);

    while (end != std::string::npos) 
    {
        tokens.push_back(str.substr(start, end - start));
        start = end + 1;
        end = str.find(delimiter, start);
    }

    tokens.push_back(str.substr(start));
    return tokens;
}

std::vector<std::string> ReadLines(const std::string& filename)
{
    std::vector<std::string> lines;
    std::ifstream file(filename);
    std::string line;

    while (std::getline(file, line)) 
    {
        lines.push_back(line);
    }

    return lines;
}

bool ValidCheckTime()
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    // st.wHour   : 시간 (24시간제)
    // st.wMinute : 분
    // st.wSecond : 초

    // 형식 맞추기 (03:33:11 형태로)
    //wchar_t timeStr[9];
    //swprintf(timeStr, 9, L"%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);

    if (st.wHour >= 9 && st.wDayOfWeek != 0 && st.wDayOfWeek != 6)
    {
        if(st.wHour >= 15 && st.wMinute < 30)
            return false;
        else
            return true;
    }


    return false;
}


void CheckStock(void* ignored)
{
    while (1)
    {
        if (ValidCheckTime())
        {
            readcount++;
            for (STOCKINFO& s : stocklist)
            {
                //std::string url = "https://finance.naver.com/item/main.naver?code=000100";
                std::string url = "https://finance.naver.com/item/main.naver?code=" + s.code;
                std::string response;
                bool ret = curl.PerformGet(url, response);
                if (ret)
                {
                    std::vector<std::string> sss = SplitLines(response);

                    int find = 0;
                    bool updown = false;
                    bool ret = FindSequentialStrings(sss, find, updown);

                    //if( ret == false)
                    //    ret = FindSequentialStrings(sss, find, false);

                    if (ret == true)
                    {
                        std::string msg = ExtractNumber(sss[find + 3]);
                        EnterCriticalSection(&cs);
                        //stockvalue.assign(msg.begin(), msg.end());
                        s.value = msg;
                        s.rise = updown ? RISE::RISE_UP : RISE::RISE_DOWN;
                        LeaveCriticalSection(&cs);
                    }
                }
            }
        }

        for (int i = 0; i < 60 * stockreadTime; i++)
        {
            EnterCriticalSection(&cs);
            if (forceTrack)
            {
                forceTrack = false;
                LeaveCriticalSection(&cs);
                break;
            }
            LeaveCriticalSection(&cs);
            Sleep(1000);
        }
        //Sleep(stockreadTime);
    }
}


void LoadConfig()
{
    std::vector<std::string> str = ReadLines("config.txt");

    for (std::string s : str)
    {
        std::vector<std::string> p = SplitString(s, ',');

        if (p[0] == "SYSTEM" && p[1] == "TIME")
        {
            std::string rtime = p[2];
            stockreadTime = stoi(rtime);// *1000 * 60;
        }
        else if (p[0] == "STOCK")
        {
            STOCKINFO info;
            info.name = p[1];
            info.code = p[2];
            info.value = "";
            stocklist.push_back(info);
        }
    }
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
    PSTR szCmdLine, int iCmdShow)
{
    HWND        hwnd;
    MSG         msg;
    WNDCLASS    wndclass;

    wndclass.style = CS_HREDRAW | CS_VREDRAW;
    wndclass.lpfnWndProc = WndProc;
    wndclass.cbClsExtra = 0;
    wndclass.cbWndExtra = 0;
    wndclass.hInstance = hInstance;
    wndclass.hIcon = LoadIcon(NULL, L"StockWatcher.ico");
    wndclass.hCursor = LoadCursor(NULL, IDC_ARROW);
    wndclass.hbrBackground = (HBRUSH)(CreateSolidBrush(RGB(64, 64, 64)));
    wndclass.lpszMenuName = NULL;
    wndclass.lpszClassName = g_appname;

    InitializeCriticalSection(&cs);

    if (!RegisterClass(&wndclass))
    {
        MessageBox(NULL, TEXT("프로그램 초기화 실패!"), g_appname, MB_ICONERROR);
        return 0;
    }

    hwnd = CreateWindowEx(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED,
        g_appname,
        NULL,
        WS_POPUP,
        0, 0,
        WINDOW_WIDTH, WINDOW_HEIGHT,
        NULL,
        NULL,
        hInstance,
        NULL);

    if (!hwnd)
    {
        MessageBox(NULL, TEXT("윈도우 생성 실패!"), g_appname, MB_ICONERROR);
        return 0;
    }

    SetLayeredWindowAttributes(hwnd, 0, 180, LWA_ALPHA);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd);
    PositionWindowAboveTaskbar(hwnd);

    LoadConfig();
    curl.Initialize();
    _beginthread(CheckStock, 0, NULL);


    while (GetMessage(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return msg.wParam;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    HDC hdc;
    PAINTSTRUCT ps;
    RECT rect;
    POINT pt;

    switch (message)
    {
    case WM_CREATE:
    {
        // 폰트 생성
        hFont = CreateFont(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"맑은 고딕");

        // 둥근 모서리 설정
        HRGN hRgn = CreateRoundRectRgn(0, 0, WINDOW_WIDTH, WINDOW_HEIGHT, 15, 15);
        SetWindowRgn(hwnd, hRgn, TRUE);

        // 스크롤 타이머 시작
        SetTimer(hwnd, SCROLL_TIMER_ID, SCROLL_SPEED, NULL);

        // 트레이 아이콘 설정
        g_nid.cbSize = sizeof(NOTIFYICONDATA);
        g_nid.hWnd = hwnd;
        g_nid.uID = 1;
        g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        g_nid.uCallbackMessage = WM_TRAYICON;
        g_nid.hIcon = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_STOCKWATCHER));
        //g_nid.hIcon = LoadIcon(NULL, MAKEINTRESOURCE(IDI_STOCKWATCHER));
        //g_nid.hIcon = LoadIcon(NULL, L"StockWatcher.ico");
        wcscpy_s(g_nid.szTip, g_appname);
        Shell_NotifyIcon(NIM_ADD, &g_nid);

        // 더블 버퍼링 초기화
        hdc = GetDC(hwnd);
        g_hdcMem = CreateCompatibleDC(hdc);
        g_hbmMem = CreateCompatibleBitmap(hdc, WINDOW_WIDTH, WINDOW_HEIGHT);
        g_hbmOld = SelectObject(g_hdcMem, g_hbmMem);
        ReleaseDC(hwnd, hdc);
    }
    return 0;

    case WM_TRAYICON:
        if (lParam == WM_RBUTTONUP)
        {
            GetCursorPos(&pt);
            CreateTrayContextMenu(hwnd, pt);
        }
        return 0;

    case WM_PAINT:
    {
        SIZE textSize;
        hdc = BeginPaint(hwnd, &ps);
        GetClientRect(hwnd, &rect);

        // 배경 그리기
        HBRUSH hBrush = CreateSolidBrush(RGB(64, 64, 64));
        FillRect(g_hdcMem, &rect, hBrush);
        DeleteObject(hBrush);

        // 텍스트 설정
        SetBkMode(g_hdcMem, TRANSPARENT);
        SetTextColor(g_hdcMem, RGB(255, 255, 255));
        SelectObject(g_hdcMem, hFont);

        if (g_scrollType == SCROLLTYPE::LR_SIDE)
        {
            EnterCriticalSection(&cs);
            std::wstring lastv = L"";// = std::wstring(g_windowText) + stockvalue;
            for (STOCKINFO s : stocklist)
                lastv += StringToWString(s.name) + std::wstring(L" : ") + StringToWString(s.value) + std::wstring(L"  ");

            lastv += std::wstring(L"    (") + std::to_wstring(readcount) + std::wstring(L")");
            LeaveCriticalSection(&cs);

            // 텍스트 크기 계산
            GetTextExtentPoint32(g_hdcMem, lastv.c_str(), wcslen(lastv.c_str()), &textSize);
            // 스크롤
            TextOut(g_hdcMem, WINDOW_WIDTH - g_scrollX, (WINDOW_HEIGHT - textSize.cy) / 2, lastv.c_str(), wcslen(lastv.c_str()));
        }
        else
        {
            if(stocklist.size() > 0)
                DisplayStock(hdc, hwnd, g_hdcMem);
        }

        // 더블 버퍼 복사
        BitBlt(hdc, 0, 0, WINDOW_WIDTH, WINDOW_HEIGHT, g_hdcMem, 0, 0, SRCCOPY);
        EndPaint(hwnd, &ps);
    }
    return 0;

    case WM_RBUTTONUP:
        pt.x = LOWORD(lParam);
        pt.y = HIWORD(lParam);
        ClientToScreen(hwnd, &pt);
        CreateContextMenu(hwnd, pt);
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDM_CLOSE:
        case IDM_EXIT:
            DestroyWindow(hwnd);
            break;
        case IDM_SHOW:
            ShowWindow(hwnd, SW_SHOW);
            break;
        case IDM_COPY:
            forceTrack = true;

            if (OpenClipboard(hwnd))
            {
                //EmptyClipboard();
                //size_t len = (wcslen(g_windowText) + 1) * sizeof(wchar_t);
                //HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, len);
                //if (hMem)
                //{
                //    wchar_t* pMem = (wchar_t*)GlobalLock(hMem);
                //    wcscpy_s(pMem, len / sizeof(wchar_t), g_windowText);
                //    GlobalUnlock(hMem);
                //    SetClipboardData(CF_UNICODETEXT, hMem);
                //}
                //CloseClipboard();
            }
            break;
        case IDM_OPTION1:
            MessageBox(hwnd, L"Option 1 selected", L"Menu", MB_OK);
            break;
        case IDM_OPTION2:
            MessageBox(hwnd, L"Option 2 selected", L"Menu", MB_OK);
            break;
        }
        return 0;

    case WM_TIMER:
        if (wParam == SCROLL_TIMER_ID && stocklist.empty() == false)
        {
            HDC hdc = GetDC(hwnd);
            SIZE textSize;
            SelectObject(hdc, hFont);
            
            STOCKINFO s = stocklist[g_currentStock];
            std::wstring lastv = StringToWString(s.name) + std::wstring(L" : ") + StringToWString(s.value);


            //std::wstring lastv = L"";
            //for (STOCKINFO s : stocklist)
            //    lastv += StringToWString(s.name) + L" : " + StringToWString(s.value) + L"  ";
            GetTextExtentPoint32(hdc, lastv.c_str(), wcslen(lastv.c_str()), &textSize);
            ReleaseDC(hwnd, hdc);

            if (g_scrollType == SCROLLTYPE::LR_SIDE)
            {
                g_scrollX++;  // 스크롤 속도 조절
                if (g_scrollX > WINDOW_WIDTH + textSize.cx)
                    g_scrollX = 0;
            }
            else
            {
                if (g_scrollY == (WINDOW_HEIGHT - textSize.cy) / 2 + textSize.cy)
                {
                    g_waitcount++;
                    if (g_waitcount > 50)
                    {
                        g_waitcount = 0;
                        g_scrollY++;
                    }
                }
                else
                {
                    g_scrollY++;
                    if (g_scrollY > WINDOW_HEIGHT + textSize.cy)
                    {
                        g_scrollY = 0;
                        if (g_currentStock >= (int)stocklist.size() - 1)
                            g_currentStock = 0;
                        else
                            g_currentStock++;
                    }
                }
            }

            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;

    case WM_DESTROY:
        SelectObject(g_hdcMem, g_hbmOld);
        DeleteObject(g_hbmMem);
        DeleteDC(g_hdcMem);
        Shell_NotifyIcon(NIM_DELETE, &g_nid);
        DeleteObject(hFont);
        PostQuitMessage(0);
        return 0;

    case WM_NCHITTEST:
        return HTCLIENT;
    }
    return DefWindowProc(hwnd, message, wParam, lParam);
}

void PositionWindowAboveTaskbar(HWND hwnd)
{
    RECT workArea;
    SystemParametersInfo(SPI_GETWORKAREA, 0, &workArea, 0);
    SetWindowPos(hwnd,
        HWND_TOPMOST,
        workArea.right - WINDOW_WIDTH - 10,
        workArea.bottom - WINDOW_HEIGHT - 10,
        WINDOW_WIDTH,
        WINDOW_HEIGHT,
        SWP_NOACTIVATE);
}

void CreateContextMenu(HWND hwnd, POINT pt)
{
    HMENU hMenu = CreatePopupMenu();

    AppendMenu(hMenu, MF_STRING, IDM_COPY, L"업데이트");
    AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenu(hMenu, MF_STRING, IDM_OPTION1, L"옵션 1");
    AppendMenu(hMenu, MF_STRING, IDM_OPTION2, L"옵션 2");
    AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenu(hMenu, MF_STRING, IDM_CLOSE, L"종료");

    SetMenuDefaultItem(hMenu, IDM_COPY, FALSE);

    TrackPopupMenu(hMenu,
        TPM_RIGHTBUTTON | TPM_LEFTALIGN,
        pt.x, pt.y, 0, hwnd, NULL);

    DestroyMenu(hMenu);
}

void CreateTrayContextMenu(HWND hwnd, POINT pt)
{
    HMENU hMenu = CreatePopupMenu();

    AppendMenu(hMenu, MF_STRING, IDM_COPY, L"업데이트");
    AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenu(hMenu, MF_STRING, IDM_OPTION1, L"옵션 1");
    AppendMenu(hMenu, MF_STRING, IDM_OPTION2, L"옵션 2");
    AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenu(hMenu, MF_STRING, IDM_EXIT, L"종료");

    SetForegroundWindow(hwnd);
    TrackPopupMenu(hMenu,
        TPM_RIGHTBUTTON | TPM_BOTTOMALIGN,
        pt.x, pt.y, 0, hwnd, NULL);

    DestroyMenu(hMenu);
}


void DisplayStock(HDC hdc, HWND hwnd, HDC g_hdcMem)
{
    // △ ▲ ▽ ▼ → ← ↑ ↓
    const std::wstring u = L"▲";
    const std::wstring d = L"▼";
    const std::wstring n = L"△▽";

    SIZE textSize;

    // 텍스트 설정
    SetBkMode(g_hdcMem, TRANSPARENT);
    SetTextColor(g_hdcMem, RGB(255, 255, 255));
    SelectObject(g_hdcMem, hFont);

    STOCKINFO s = stocklist[g_currentStock];
    std::wstring lastv = StringToWString(s.name) + std::wstring(L" : ") + StringToWString(s.value);
    GetTextExtentPoint32(g_hdcMem, lastv.c_str(), wcslen(lastv.c_str()), &textSize);

    // 스크롤
    TextOut(g_hdcMem, (WINDOW_WIDTH - textSize.cx) / 2, WINDOW_HEIGHT - g_scrollY, lastv.c_str(), wcslen(lastv.c_str()));

    if (s.rise == RISE::RISE_UP)
    {
        SetTextColor(g_hdcMem, RGB(255, 0, 0));
        TextOut(g_hdcMem, (WINDOW_WIDTH - textSize.cx) / 2 - 15, WINDOW_HEIGHT - g_scrollY, u.c_str(), 1);
    }
    else if (s.rise == RISE::RISE_DOWN)
    {
        SetTextColor(g_hdcMem, RGB(0, 0, 255));
        TextOut(g_hdcMem, (WINDOW_WIDTH - textSize.cx) / 2 - 15, WINDOW_HEIGHT - g_scrollY, d.c_str(), 1);
    }
    else
    {
        SetTextColor(g_hdcMem, RGB(0, 255, 0));
        TextOut(g_hdcMem, (WINDOW_WIDTH - textSize.cx) / 2 - 30, WINDOW_HEIGHT - g_scrollY, n.c_str(), 2);
    }
}
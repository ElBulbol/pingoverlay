#include <windows.h>
#include <gdiplus.h>
#include <string>
#include <sstream>
#include <memory>
#include <thread>
#include <chrono>
#include <mutex>
#include <shellapi.h>
#include <commctrl.h>
#include <vector>
#include <algorithm> // For std::clamp

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")

// Global variables
static const wchar_t* CLASS_NAME = L"PingOverlayClass";
static const wchar_t* WINDOW_TITLE = L"Ping Overlay";
static HINSTANCE hInst;
static HWND hwnd;
static std::wstring pingTime = L"0 ms";
static std::mutex pingMutex;
static bool running = true;
static POINT dragOffset;
static bool isDragging = false;
static HWND hwndClose, hwndMinimize;

// Forward declarations
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
std::wstring ExecutePing();
void UpdatePing();
void CreateButtons(HWND);
Gdiplus::Color GetPingColor(int pingMs);

// Initialize GDI+
class GdiPlusInitializer {
public:
    GdiPlusInitializer() {
        Gdiplus::GdiplusStartupInput gdiplusStartupInput;
        Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, nullptr);
    }
    ~GdiPlusInitializer() {
        Gdiplus::GdiplusShutdown(gdiplusToken);
    }
private:
    ULONG_PTR gdiplusToken;
};

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    hInst = hInstance;

    // Initialize GDI+
    GdiPlusInitializer gdiplusInit;

    // Initialize common controls
    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_WIN95_CLASSES;
    InitCommonControlsEx(&icc);

    // Register window class
    WNDCLASSEX wc = { sizeof(WNDCLASSEX) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hbrBackground = nullptr;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassEx(&wc);

    // Create window
    hwnd = CreateWindowEx(
        WS_EX_TOPMOST, // Always on top
        CLASS_NAME,
        WINDOW_TITLE,
        WS_POPUP,
        CW_USEDEFAULT, CW_USEDEFAULT,
        120, 70, // Increased window size
        nullptr, nullptr, hInstance, nullptr
    );

    if (!hwnd) {
        MessageBox(nullptr, L"Window creation failed!", L"Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    // Create buttons
    CreateButtons(hwnd);

    // Show window
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    // Start ping thread
    std::thread pingThread(UpdatePing);

    // Message loop
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // Cleanup
    {
        std::lock_guard<std::mutex> lock(pingMutex);
        running = false;
    }
    pingThread.join();

    return 0;
}

void CreateButtons(HWND hwndParent) {
    hwndClose = CreateWindow(
        L"BUTTON", L"X",
        WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
        80, 5, 15, 15, // Small button at top-right
        hwndParent, (HMENU)100,
        hInst, nullptr
    );

    hwndMinimize = CreateWindow(
        L"BUTTON", L"-",
        WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
        60, 5, 15, 15, // Small button next to close
        hwndParent, (HMENU)101,
        hInst, nullptr
    );

    // Make both buttons owner-drawn to match the style
    SendMessage(hwndClose, BM_SETSTYLE, BS_OWNERDRAW, 0);
    SendMessage(hwndMinimize, BM_SETSTYLE, BS_OWNERDRAW, 0);
}

std::wstring ExecutePing() {
    std::wstring result = L"Error";

    // Create pipes
    HANDLE hRead, hWrite;
    SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) {
        return result;
    }

    // Set up startup info
    STARTUPINFOW si = { sizeof(STARTUPINFOW) };
    PROCESS_INFORMATION pi;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hWrite;
    si.hStdError = hWrite;

    // Execute ping command
    wchar_t cmd[] = L"ping 1.1.1.1 -n 1";
    if (CreateProcessW(nullptr, cmd, nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(hWrite);

        // Read output
        std::vector<char> buffer(1024);
        DWORD bytesRead;
        std::string output;
        while (ReadFile(hRead, buffer.data(), buffer.size() - 1, &bytesRead, nullptr) && bytesRead > 0) {
            buffer[bytesRead] = '\0';
            output += buffer.data();
        }

        // Parse ping time
        size_t pos = output.find("time=");
        if (pos != std::string::npos) {
            pos += 5;
            size_t end = output.find("ms", pos);
            if (end != std::string::npos) {
                std::string timeStr = output.substr(pos, end - pos);
                result = std::wstring(timeStr.begin(), timeStr.end()) + L" ms";
            }
        }

        WaitForSingleObject(pi.hProcess, INFINITE);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }

    CloseHandle(hRead);
    return result;
}

void UpdatePing() {
    while (true) {
        {
            std::lock_guard<std::mutex> lock(pingMutex);
            if (!running) break;
        }

        auto result = ExecutePing();
        {
            std::lock_guard<std::mutex> lock(pingMutex);
            pingTime = result;
        }

        InvalidateRect(hwnd, nullptr, TRUE);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

// Helper function to interpolate between two colors
Gdiplus::Color InterpolateColor(float t, Gdiplus::Color c1, Gdiplus::Color c2) {
    int r = static_cast<int>(c1.GetR() + t * (c2.GetR() - c1.GetR()));
    int g = static_cast<int>(c1.GetG() + t * (c2.GetG() - c1.GetG()));
    int b = static_cast<int>(c1.GetB() + t * (c2.GetB() - c1.GetB()));
    return Gdiplus::Color(255, r, g, b);
}

// Compute ping text color based on ping value
Gdiplus::Color GetPingColor(int pingMs) {
    // Define color stops
    const int thresholds[] = { 0, 50, 70, 100, 200 }; // More sensitive thresholds
    const Gdiplus::Color colors[] = {
        Gdiplus::Color(255, 0, 255, 0),   // Green at 0 ms
        Gdiplus::Color(255, 0, 255, 0),   // Green at 50 ms
        Gdiplus::Color(255, 255, 255, 0), // Yellow at 70 ms
        Gdiplus::Color(255, 255, 128, 128), // Light red at 100 ms
        Gdiplus::Color(255, 255, 0, 0)    // Bold red at 200 ms
    };

    // Clamp ping value to avoid extrapolation
    pingMs = std::max(0, pingMs);

    // Find the appropriate range
    for (int i = 0; i < 4; ++i) {
        if (pingMs >= thresholds[i] && pingMs <= thresholds[i + 1]) {
            float t = static_cast<float>(pingMs - thresholds[i]) / (thresholds[i + 1] - thresholds[i]);
            return InterpolateColor(t, colors[i], colors[i + 1]);
        }
    }

    // If pingMs > 200, return bold red
    return colors[4];
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static Gdiplus::Graphics* graphics = nullptr;
    static Gdiplus::SolidBrush* blackBrush = nullptr;
    static Gdiplus::SolidBrush* textBrush = nullptr;
    static Gdiplus::Font* font = nullptr;

    switch (msg) {
    case WM_CREATE: {
        // Initialize GDI+ objects
        graphics = new Gdiplus::Graphics(GetDC(hwnd));
        blackBrush = new Gdiplus::SolidBrush(Gdiplus::Color(255, 0, 0, 0));
        textBrush = new Gdiplus::SolidBrush(Gdiplus::Color(255, 255, 255, 255));
        Gdiplus::FontFamily fontFamily(L"Arial");
        font = new Gdiplus::Font(&fontFamily, 12, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);

        // Set timer for ping updates
        SetTimer(hwnd, 1, 1000, nullptr);
        break;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        // Create double buffer
        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP memBitmap = CreateCompatibleBitmap(hdc, 100, 60);
        SelectObject(memDC, memBitmap);

        Gdiplus::Graphics memGraphics(memDC);
        memGraphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

        // Draw solid black rounded rectangle
        Gdiplus::GraphicsPath path;
        path.AddArc(0, 0, 10, 10, 180, 90);
        path.AddArc(90, 0, 10, 10, 270, 90);
        path.AddArc(90, 50, 10, 10, 0, 90);
        path.AddArc(0, 50, 10, 10, 90, 90);
        path.CloseFigure();
        memGraphics.FillPath(blackBrush, &path);

        // Draw ping time with dynamic color
        std::lock_guard<std::mutex> lock(pingMutex);
        int pingMs = 0;
        if (pingTime != L"Error") {
            try {
                // Extract numeric part (e.g., "45" from "45 ms")
                std::wstring numStr = pingTime.substr(0, pingTime.find(L" "));
                pingMs = std::stoi(numStr);
            }
            catch (...) {
                pingMs = 0; // Fallback for parsing errors
            }
        }
        textBrush->SetColor(GetPingColor(pingMs));
        memGraphics.DrawString(pingTime.c_str(), -1, font, Gdiplus::PointF(10, 25), textBrush);

        // Copy to screen
        BitBlt(hdc, 0, 0, 100, 60, memDC, 0, 0, SRCCOPY);

        // Cleanup
        DeleteDC(memDC);
        DeleteObject(memBitmap);
        EndPaint(hwnd, &ps);
        break;
    }

    case WM_DRAWITEM: {
        LPDRAWITEMSTRUCT pDIS = (LPDRAWITEMSTRUCT)lParam;
        if (pDIS->hwndItem == hwndClose || pDIS->hwndItem == hwndMinimize) {
            HDC hdc = pDIS->hDC;
            RECT rc = pDIS->rcItem;

            // Draw button background
            FillRect(hdc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
            SetTextColor(hdc, RGB(255, 255, 255)); // White text for buttons

            // Draw text
            SetBkMode(hdc, TRANSPARENT);
            wchar_t text[2];
            GetWindowTextW(pDIS->hwndItem, text, 2);
            DrawTextW(hdc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            return TRUE;
        }
        break;
    }

    case WM_LBUTTONDOWN: {
        POINT pt = { LOWORD(lParam), HIWORD(lParam) };
        dragOffset.x = pt.x;
        dragOffset.y = pt.y;
        isDragging = true;
        SetCapture(hwnd);
        break;
    }

    case WM_MOUSEMOVE: {
        if (isDragging) {
            POINT pt;
            GetCursorPos(&pt);
            SetWindowPos(hwnd, nullptr, pt.x - dragOffset.x, pt.y - dragOffset.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
        }
        break;
    }

    case WM_LBUTTONUP: {
        isDragging = false;
        ReleaseCapture();
        break;
    }

    case WM_COMMAND: {
        if (LOWORD(wParam) == 100) {
            DestroyWindow(hwnd);
        }
        else if (LOWORD(wParam) == 101) {
            ShowWindow(hwnd, SW_MINIMIZE);
        }
        break;
    }

    case WM_TIMER: {
        InvalidateRect(hwnd, nullptr, TRUE);
        break;
    }

    case WM_DESTROY: {
        KillTimer(hwnd, 1);
        delete graphics;
        delete blackBrush;
        delete textBrush;
        delete font;
        {
            std::lock_guard<std::mutex> lock(pingMutex);
            running = false;
        }
        PostQuitMessage(0);
        break;
    }

    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}
// g++ -o overlayindecator overlaypingindicatour.cpp -mwindows -lgdiplus -lcomctl32 -lshell32 -municode; Start-Process .\overlayindecator.exe
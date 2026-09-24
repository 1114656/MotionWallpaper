#pragma once
#include "../MotionWallpaper.Common/DisplayTopology.h"
#include <windows.h>
#include <string>
#pragma comment(lib, "gdi32.lib")

namespace motion::app
{
    inline LRESULT CALLBACK display_identification_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
    {
        if (message == WM_PAINT) {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(window, &paint);
            RECT rect{}; GetClientRect(window, &rect);
            auto background = CreateSolidBrush(RGB(18, 38, 66));
            FillRect(dc, &rect, background); DeleteObject(background);
            auto font = CreateFontW(-MulDiv(60, GetDpiForWindow(window), 96), 0, 0, 0,
                FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            auto old = SelectObject(dc, font);
            SetTextColor(dc, RGB(255, 255, 255)); SetBkMode(dc, TRANSPARENT);
            wchar_t text[32]{}; GetWindowTextW(window, text, 32);
            DrawTextW(dc, text, -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(dc, old); DeleteObject(font); EndPaint(window, &paint); return 0;
        }
        if (message == WM_TIMER || message == WM_LBUTTONDOWN) { DestroyWindow(window); return 0; }
        if (message == WM_NCHITTEST) return HTTRANSPARENT;
        return DefWindowProcW(window, message, wparam, lparam);
    }

    inline void identify_displays(std::vector<DisplayTarget> const& displays)
    {
        constexpr auto className = L"MotionWallpaper.DisplayIdentification";
        WNDCLASSW type{};
        type.lpfnWndProc = display_identification_proc;
        type.hInstance = GetModuleHandleW(nullptr); type.lpszClassName = className;
        RegisterClassW(&type);
        // Repeat clicks replace the previous overlays rather than accumulating.
        while (auto previous = FindWindowW(className, nullptr)) DestroyWindow(previous);
        for (size_t i = 0; i < displays.size(); ++i) {
            auto const& display = displays[i];
            auto label = std::to_wstring(i + 1);
            HWND overlay = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                className, label.c_str(), WS_POPUP, display.bounds.left + 36,
                display.bounds.top + 36, 170, 120, nullptr, nullptr, type.hInstance, nullptr);
            if (overlay) { ShowWindow(overlay, SW_SHOWNOACTIVATE); SetTimer(overlay, 1, 3000, nullptr); }
        }
    }
}

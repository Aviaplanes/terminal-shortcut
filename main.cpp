#define WIN32_LEAN_AND_MEAN
#define _WIN32_DCOM
#include <windows.h>
#include <shlobj.h>
#include <exdisp.h>
#include <shobjidl.h>
#include <string>
#include <vector>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")

#define WM_TRIGGER_SHORTCUT (WM_USER + 1)
#define WM_TRAY_ICON        (WM_USER + 2)
#define ID_TRAY_EXIT        1001

HHOOK g_hHook = nullptr;
HWND  g_hMainWnd = nullptr;
NOTIFYICONDATAW g_nid = {};
bool  g_shortcutActive = false; // флаг: сработал ли Win+Enter в текущем нажатии

// ---------- Детект директории ----------

bool IsDesktopWindow(HWND hwnd) {
    wchar_t className[256];
    GetClassNameW(hwnd, className, 256);
    if (wcscmp(className, L"Progman") == 0) return true;
    if (wcscmp(className, L"WorkerW") == 0) {
        if (FindWindowExW(hwnd, nullptr, L"SHELLDLL_DefView", nullptr))
            return true;
    }
    return false;
}

bool IsExplorerWindow(HWND hwnd) {
    wchar_t className[256];
    GetClassNameW(hwnd, className, 256);
    return wcscmp(className, L"CabinetWClass") == 0;
}

std::wstring GetShellViewPath(HWND hwnd, bool isDesktop) {
    std::wstring result;

    IShellWindows* pShellWindows = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellWindows, NULL, CLSCTX_ALL,
        IID_IShellWindows, (void**)&pShellWindows);
    if (FAILED(hr) || !pShellWindows) return result;

    IShellBrowser* pShellBrowser = nullptr;

    if (isDesktop) {
        VARIANT vEmpty; VariantInit(&vEmpty);
        long hwndLong = 0;
        IDispatch* pDisp = nullptr;
        hr = pShellWindows->FindWindowSW(&vEmpty, &vEmpty, SWC_DESKTOP,
            &hwndLong, SWFO_NEEDDISPATCH, &pDisp);
        if (SUCCEEDED(hr) && pDisp) {
            IServiceProvider* pSP = nullptr;
            if (SUCCEEDED(pDisp->QueryInterface(IID_IServiceProvider, (void**)&pSP))) {
                pSP->QueryService(SID_STopLevelBrowser, IID_IShellBrowser, (void**)&pShellBrowser);
                pSP->Release();
            }
            pDisp->Release();
        }
    } else {
        long count = 0;
        pShellWindows->get_Count(&count);
        for (long i = 0; i < count; i++) {
            VARIANT vi; VariantInit(&vi);
            vi.vt = VT_I4; vi.lVal = i;

            IDispatch* pDisp = nullptr;
            if (FAILED(pShellWindows->Item(vi, &pDisp)) || !pDisp) continue;

            IWebBrowserApp* pWebBrowser = nullptr;
            if (SUCCEEDED(pDisp->QueryInterface(IID_IWebBrowserApp, (void**)&pWebBrowser))) {
                HWND hwndWB = nullptr;
                pWebBrowser->get_HWND((LONG_PTR*)&hwndWB);
                if (hwndWB == hwnd) {
                    IServiceProvider* pSP = nullptr;
                    if (SUCCEEDED(pWebBrowser->QueryInterface(IID_IServiceProvider, (void**)&pSP))) {
                        pSP->QueryService(SID_STopLevelBrowser, IID_IShellBrowser, (void**)&pShellBrowser);
                        pSP->Release();
                    }
                    pWebBrowser->Release();
                    pDisp->Release();
                    break;
                }
                pWebBrowser->Release();
            }
            pDisp->Release();
        }
    }

    pShellWindows->Release();
    if (!pShellBrowser) return result;

    IShellView* pShellView = nullptr;
    hr = pShellBrowser->QueryActiveShellView(&pShellView);
    pShellBrowser->Release();
    if (FAILED(hr) || !pShellView) return result;

    IDataObject* pDataObject = nullptr;
    if (SUCCEEDED(pShellView->GetItemObject(SVGIO_SELECTION, IID_IDataObject, (void**)&pDataObject)) && pDataObject) {
        FORMATETC fmte = { CF_HDROP, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        STGMEDIUM stgm;
        if (SUCCEEDED(pDataObject->GetData(&fmte, &stgm))) {
            HDROP hDrop = (HDROP)stgm.hGlobal;
            UINT fileCount = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
            if (fileCount > 0) {
                wchar_t itemPath[MAX_PATH];
                if (DragQueryFileW(hDrop, 0, itemPath, MAX_PATH)) {
                    DWORD attrs = GetFileAttributesW(itemPath);
                    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
                        result = itemPath;
                    } else {
                        std::wstring p(itemPath);
                        size_t pos = p.find_last_of(L"\\/");
                        if (pos != std::wstring::npos) result = p.substr(0, pos);
                    }
                }
            }
            ReleaseStgMedium(&stgm);
        }
        pDataObject->Release();
    }

    if (result.empty()) {
        IFolderView* pFolderView = nullptr;
        if (SUCCEEDED(pShellView->QueryInterface(IID_IFolderView, (void**)&pFolderView))) {
            IPersistFolder2* pPersistFolder = nullptr;
            if (SUCCEEDED(pFolderView->GetFolder(IID_IPersistFolder2, (void**)&pPersistFolder))) {
                LPITEMIDLIST pidl = nullptr;
                if (SUCCEEDED(pPersistFolder->GetCurFolder(&pidl))) {
                    wchar_t buffer[MAX_PATH];
                    if (SHGetPathFromIDListW(pidl, buffer)) result = buffer;
                    CoTaskMemFree(pidl);
                }
                pPersistFolder->Release();
            }
            pFolderView->Release();
        }
    }

    pShellView->Release();
    return result;
}

std::wstring GetTargetDirectory() {
    HWND hwnd = GetForegroundWindow();
    std::wstring path;

    if (IsExplorerWindow(hwnd)) {
        path = GetShellViewPath(hwnd, false);
    } else if (IsDesktopWindow(hwnd)) {
        path = GetShellViewPath(hwnd, true);
    }

    if (path.empty()) {
        wchar_t homeDir[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_PROFILE, NULL, 0, homeDir)))
            path = homeDir;
    }

    return path;
}

// ---------- Запуск терминала ----------

void LaunchTerminal(const std::wstring& path) {
    std::wstring cmdLine = L"wt.exe -d \"" + path + L"\"";

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};

    std::vector<wchar_t> buf(cmdLine.begin(), cmdLine.end());
    buf.push_back(0);

    BOOL ok = CreateProcessW(NULL, buf.data(), NULL, NULL, FALSE,
        0, NULL, NULL, &si, &pi);

    if (!ok) {
        std::wstring fallback = L"cmd.exe /K cd /d \"" + path + L"\"";
        std::vector<wchar_t> buf2(fallback.begin(), fallback.end());
        buf2.push_back(0);
        if (CreateProcessW(NULL, buf2.data(), NULL, NULL, FALSE,
            0, NULL, NULL, &si, &pi)) {
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }
        return;
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
}

// ---------- Хук клавиатуры: Win + Enter ----------

LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        KBDLLHOOKSTRUCT* pKey = (KBDLLHOOKSTRUCT*)lParam;

        if (pKey->vkCode == VK_RETURN) {
            bool winDown = (GetAsyncKeyState(VK_LWIN) & 0x8000) ||
                           (GetAsyncKeyState(VK_RWIN) & 0x8000);

            if (winDown) {
                if (wParam == WM_KEYDOWN) {
                    PostMessage(g_hMainWnd, WM_TRIGGER_SHORTCUT, 0, 0);
                }
                // подавляем и keydown, и keyup Enter,
                // чтобы событие не улетело в активное окно
                return 1;
            }
        }
    }
    return CallNextHookEx(g_hHook, nCode, wParam, lParam);
}

// ---------- Окно / трей ----------

void AddTrayIcon(HWND hwnd) {
    g_nid.cbSize = sizeof(NOTIFYICONDATAW);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY_ICON;
    g_nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wcscpy_s(g_nid.szTip, L"Terminal Shortcut");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_TRIGGER_SHORTCUT: {
        std::wstring dir = GetTargetDirectory();
        LaunchTerminal(dir);
        return 0;
    }
    case WM_TRAY_ICON:
        if (lParam == WM_RBUTTONUP) {
            POINT pt;
            GetCursorPos(&pt);
            HMENU hMenu = CreatePopupMenu();
            AppendMenuW(hMenu, MF_STRING, ID_TRAY_EXIT, L"Выход");
            SetForegroundWindow(hwnd);
            TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
            DestroyMenu(hMenu);
        }
        return 0;
    case WM_COMMAND:
        if (LOWORD(wParam) == ID_TRAY_EXIT) {
            DestroyWindow(hwnd);
        }
        return 0;
    case WM_DESTROY:
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
    HANDLE hMutex = CreateMutexW(NULL, TRUE, L"TerminalShortcutMutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    const wchar_t CLASS_NAME[] = L"TerminalShortcutWindowClass";
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    RegisterClassW(&wc);

    g_hMainWnd = CreateWindowExW(0, CLASS_NAME, L"Terminal Shortcut", 0,
        0, 0, 0, 0, HWND_MESSAGE, nullptr, hInstance, nullptr);

    AddTrayIcon(g_hMainWnd);

    g_hHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc,
        GetModuleHandleW(NULL), 0);

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (g_hHook) UnhookWindowsHookEx(g_hHook);
    CoUninitialize();
    CloseHandle(hMutex);
    return 0;
}
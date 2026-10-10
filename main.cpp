#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlwapi.h>
#include <winhttp.h>

#include <WebView2.h>
#include <wrl.h>

#include <filesystem>
#include <iterator>
#include <fstream>
#include <system_error>
#include <string>
#include <vector>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {
    constexpr wchar_t WindowClassName[] = L"TACT_AIM_Desktop_Window";
    constexpr wchar_t WindowTitle[] = L"TACT-AIM Desktop";

    ComPtr<ICoreWebView2Controller> g_controller;
    ComPtr<ICoreWebView2> g_webview;
    EventRegistrationToken g_messageToken{};
    std::wstring g_gameUrl;

    constexpr wchar_t UpdateHost[] = L"raw.githubusercontent.com";
    constexpr wchar_t UpdatePath[] = L"/Subterra178/tact-aimfeed/main/mirror/play/index.html";
    constexpr wchar_t UserAgent[] = L"TACT-AIM-Desktop/0.1";

    std::wstring ReadTextFile(const std::filesystem::path& path) {
        std::wifstream file(path, std::ios::binary);
        if (!file) return L"";
        return std::wstring(std::istreambuf_iterator<wchar_t>(file), std::istreambuf_iterator<wchar_t>());
    }

    std::wstring GetHeaderValue(HINTERNET request, DWORD header) {
        DWORD bytes = 0;
        WinHttpQueryHeaders(request, header, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &bytes, WINHTTP_NO_HEADER_INDEX);
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes == 0) return L"";
        std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1);
        if (!WinHttpQueryHeaders(request, header, WINHTTP_HEADER_NAME_BY_INDEX, buffer.data(), &bytes, WINHTTP_NO_HEADER_INDEX)) return L"";
        return buffer.data();
    }

    bool DownloadLatestGame(const std::filesystem::path& gamePath) {
        HINTERNET session = WinHttpOpen(UserAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session) return false;
        WinHttpSetTimeouts(session, 3000, 3000, 5000, 15000);
        HINTERNET connection = WinHttpConnect(session, UpdateHost, INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!connection) { WinHttpCloseHandle(session); return false; }

        const auto etagPath = gamePath.parent_path() / L".game-etag";
        std::wstring oldEtag;
        {
            std::wifstream etagFile(etagPath);
            if (etagFile) std::getline(etagFile, oldEtag);
        }

        HINTERNET request = WinHttpOpenRequest(connection, L"HEAD", UpdatePath, nullptr,
                                                WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                WINHTTP_FLAG_SECURE);
        bool updated = false;
        if (request && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                          WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(request, nullptr)) {
            DWORD status = 0, statusSize = sizeof(status);
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
            const std::wstring remoteEtag = GetHeaderValue(request, WINHTTP_QUERY_ETAG);
            WinHttpCloseHandle(request);
            request = nullptr;

            if (status == 200 && (!std::filesystem::exists(gamePath) || remoteEtag.empty() || remoteEtag != oldEtag)) {
                request = WinHttpOpenRequest(connection, L"GET", UpdatePath, nullptr,
                                             WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                             WINHTTP_FLAG_SECURE);
                if (request && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                                  WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                    WinHttpReceiveResponse(request, nullptr)) {
                    DWORD getStatus = 0, getStatusSize = sizeof(getStatus);
                    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                        WINHTTP_HEADER_NAME_BY_INDEX, &getStatus, &getStatusSize, WINHTTP_NO_HEADER_INDEX);
                    const auto tempPath = gamePath.parent_path() / L"index.html.download";
                    std::ofstream output(tempPath, std::ios::binary | std::ios::trunc);
                    bool ok = getStatus == 200 && static_cast<bool>(output);
                    while (ok) {
                        DWORD available = 0;
                        if (!WinHttpQueryDataAvailable(request, &available)) { ok = false; break; }
                        if (available == 0) break;
                        std::vector<char> chunk(available);
                        DWORD bytesRead = 0;
                        if (!WinHttpReadData(request, chunk.data(), available, &bytesRead)) { ok = false; break; }
                        output.write(chunk.data(), bytesRead);
                        if (!output) { ok = false; break; }
                    }
                    output.close();
                    if (ok && std::filesystem::file_size(tempPath) > 0 &&
                        MoveFileExW(tempPath.c_str(), gamePath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                        std::wofstream etagFile(etagPath, std::ios::trunc);
                        if (etagFile) etagFile << remoteEtag;
                        updated = true;
                    } else {
                        std::error_code ignored;
                        std::filesystem::remove(tempPath, ignored);
                    }
                }
            }
            if (request) WinHttpCloseHandle(request);
        } else if (request) {
            WinHttpCloseHandle(request);
        }
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return updated;
    }

    std::filesystem::path GetBasePath() {
        wchar_t exePath[32768]{};
        const DWORD length = GetModuleFileNameW(nullptr, exePath, static_cast<DWORD>(std::size(exePath)));
        if (length == 0 || length >= std::size(exePath)) {
            return std::filesystem::current_path();
        }
        return std::filesystem::path(exePath).parent_path();
    }

    std::filesystem::path FindContentFile(const wchar_t* relativePath) {
        const auto besideExe = GetBasePath() / relativePath;
        if (std::filesystem::exists(besideExe)) {
            return besideExe;
        }
        return std::filesystem::current_path() / relativePath;
    }

    std::wstring PathToFileUrl(const std::filesystem::path& path) {
        std::vector<wchar_t> buffer(32768);
        DWORD length = static_cast<DWORD>(buffer.size());
        const HRESULT hr = UrlCreateFromPathW(path.c_str(), buffer.data(), &length, 0);
        if (FAILED(hr)) {
            return L"";
        }
        return std::wstring(buffer.data());
    }

    void ShowError(HWND hwnd, const wchar_t* message) {
        MessageBoxW(hwnd, message, WindowTitle, MB_OK | MB_ICONERROR);
    }

    void NavigateToMenu() {
        if (!g_webview) return;
        const auto menuPath = FindContentFile(L"launcher\\index.html");
        if (!std::filesystem::exists(menuPath)) {
            ShowError(nullptr, L"あらぁ?めにゅぅ画面が見つからないようです。");
            return;
        }
        const auto menuUrl = PathToFileUrl(menuPath);
        if (menuUrl.empty()) {
            ShowError(nullptr, L"メニュー画面のファイルURLを作成できませんでした。");
            return;
        }
        g_webview->Navigate(menuUrl.c_str());
    }

    void InitializeWebView(HWND hwnd) {
        const auto gamePath = FindContentFile(L"game\\index.html");
        std::error_code directoryError;
        std::filesystem::create_directories(gamePath.parent_path(), directoryError);

        DownloadLatestGame(gamePath);
        if (!std::filesystem::exists(gamePath)) {
            ShowError(hwnd, L"TACT-AIM本体を取得できませんでした。インターネット接続を確認してくださいな。\n\n取得先: GitHub mirror/play/index.html");
            return;
        }

        g_gameUrl = PathToFileUrl(gamePath);
        if (g_gameUrl.empty()) {
            ShowError(hwnd, L"ファイルURLを作成できませんでした。");
            return;
        }

        CreateCoreWebView2EnvironmentWithOptions(
            nullptr, nullptr, nullptr,
            Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
                [hwnd](HRESULT result, ICoreWebView2Environment* environment) -> HRESULT {
                    if (FAILED(result) || !environment) {
                        ShowError(hwnd, L"あらぁ?WebView2 Runtime が見つからないようです。インストールしてくださいな。");
                        return S_OK;
                    }

                    environment->CreateCoreWebView2Controller(
                        hwnd,
                        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                            [hwnd](HRESULT controllerResult, ICoreWebView2Controller* controller) -> HRESULT {
                                if (FAILED(controllerResult) || !controller) {
                                    ShowError(hwnd, L"WebView2コントローラーを作成できませんでした。");
                                    return S_OK;
                                }

                                g_controller = controller;
                                g_controller->get_CoreWebView2(&g_webview);

                                RECT bounds{};
                                GetClientRect(hwnd, &bounds);
                                g_controller->put_Bounds(bounds);

                                g_webview->add_WebMessageReceived(
                                    Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                        [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                            LPWSTR rawMessage = nullptr;
                                            if (FAILED(args->TryGetWebMessageAsString(&rawMessage)) || !rawMessage) {
                                                return S_OK;
                                            }
                                            const std::wstring message(rawMessage);
                                            CoTaskMemFree(rawMessage);

                                            constexpr wchar_t prefix[] = L"start|";
                                            if (message.rfind(prefix, 0) == 0 && g_webview) {
                                                const std::wstring encodedName = message.substr(std::size(prefix) - 1);
                                                const std::wstring target = g_gameUrl + L"?name=" + encodedName;
                                                g_webview->Navigate(target.c_str());
                                            } else if (message == L"menu") {
                                                NavigateToMenu();
                                            }
                                            return S_OK;
                                        }).Get(),
                                    &g_messageToken);

                                NavigateToMenu();
                                return S_OK;
                            }).Get());
                    return S_OK;
                }).Get());
    }

    LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
            case WM_CREATE:
                InitializeWebView(hwnd);
                return 0;
            case WM_SIZE:
                if (g_controller) {
                    RECT bounds{};
                    GetClientRect(hwnd, &bounds);
                    g_controller->put_Bounds(bounds);
                }
                return 0;
            case WM_DESTROY:
                if (g_webview && g_messageToken.value != 0) {
                    g_webview->remove_WebMessageReceived(g_messageToken);
                }
                g_webview.Reset();
                g_controller.Reset();
                PostQuitMessage(0);
                return 0;
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(comResult)) return 1;

    WNDCLASSW wc{};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.lpszClassName = WindowClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);

    if (!RegisterClassW(&wc)) {
        CoUninitialize();
        return 1;
    }

    HWND hwnd = CreateWindowExW(
        0, WindowClassName, WindowTitle, WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1280, 800,
        nullptr, nullptr, instance, nullptr);

    if (!hwnd) {
        CoUninitialize();
        return 1;
    }

    ShowWindow(hwnd, showCommand);
    UpdateWindow(hwnd);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    CoUninitialize();
    return static_cast<int>(message.wParam);
}

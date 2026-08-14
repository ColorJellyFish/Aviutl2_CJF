// CJFPreviewPenTool.cpp
// AviUtl2 (AviUtl ExEdit2) generic plugin (.aux2)
// ペンツール: フリーハンドで線を描き、座標+タイミングを記録して
// ペンツール.obj2 の「線の座標」項目へ書き込み、「線が引かれていく」アニメーションオブジェクトにする。
//
// 設計 (Phase 2 の範囲指定プラグインを流用):
//   1. ペンツール.obj2 の「ペンモード起動」チェックをポーリングで検知
//   2. 現在フレームを rendering_scene_video で取得し、専用ウィンドウに表示
//   3. 左ドラッグで線を記録（シーン座標 + モード開始からのミリ秒）
//   4. 右クリックで確定 → 座標列と中心位置をペンツール.obj2 へ書き込む
//   5. Esc でキャンセル
//
// 座標データ形式（--value@pts:線の座標 に書く文字列）:
//   "x,y,t;x,y,t;..."   x,y: シーンpx（左上原点） t: ペンモード開始からのミリ秒

#define NOMINMAX // windows.h の min/max マクロで std::min/std::max が壊れるのを防ぐ
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <new>
#include <string>
#include <vector>

#include "plugin2.h"
#include "logger2.h"

COMMON_PLUGIN_TABLE common_plugin_table = {
    L"CJF Pen Tool",
    L"AviUtl2 freehand pen tool (drawing animation object)",
};

static LOG_HANDLE* logger = nullptr;
static HWND host_window = nullptr;
static HINSTANCE module_instance = nullptr;
static EDIT_HANDLE* edit_handle = nullptr; // RegisterPlugin で取得、以後も有効 (SDK サンプル準拠)

static HWND frame_window = nullptr;
static const wchar_t frame_class_name[] = L"CJFPreviewPenFrame";
static constexpr wchar_t panel_message_name[] = L"CJF.PreviewRangeSelector.Panel.Pen";
static const char pen_object_alias[] = u8R"([Object]
[Object.0]
effect.name=ペンツール
進捗スライダーで制御=0
進捗(%)=0.00
ペンモード起動=0
線をクリア=0
ペンの太さ(px)=8
入り長さ=0
入り太さ=0
抜き長さ=0
抜き太さ=0
柔らかさ=2
手ブレ補正=16
形状優先=3
色=000000
線の座標(シーンpx・ミリ秒)=""
[Object.1]
effect.name=標準描画
X=0.00
Y=0.00
)";

// チェックONで検知した対象オブジェクト（適用時に選択状態へ依存しない）
static OBJECT_HANDLE pen_trigger_object = nullptr;

// 「線をクリア」の対象オブジェクト（チェック/右クリックメニューから。遅延実行用）
static OBJECT_HANDLE clear_request_object = nullptr;

static void request_pen_mode();  // 前方宣言 (RegisterPlugin のメニュー登録から呼ぶ)
static void reset_pen_checkbox(); // 前方宣言 (begin_pen_mode の失敗パスから呼ぶ)

// チェックボタン式起動: 設定ウィンドウの「ペンモード起動」チェックをポーリングで検知する。
#define PICK_POLL_TIMER_ID 3
#define PICK_POLL_INTERVAL_MS 200
static bool pick_trigger_detected = false; // 検知→適用/キャンセルまで再検知を防ぐ
static bool checkbox_polling_enabled = true;
static bool polling_in_progress = false;
static DWORD polling_backoff_until = 0;
#define CONFIG_CHECKBOX_ID 3001
static HWND config_window = nullptr;
static constexpr COLORREF CJF_UI_BG = RGB(0x20, 0x20, 0x20);
static constexpr COLORREF CJF_UI_BUTTON = RGB(0x2a, 0x2a, 0x2a);
static constexpr COLORREF CJF_UI_BORDER = RGB(0x60, 0x60, 0x60);
static constexpr COLORREF CJF_UI_TEXT = RGB(0xff, 0xff, 0xff);
#define CONFIRM_RETRY_TIMER_ID 4
#define CONFIRM_RETRY_INTERVAL_MS 50
#define CONFIRM_RETRY_MAX 20
static int confirm_retry_count = 0;

// レンダリングで取得した現在フレーム (シーン解像度, RGBA, r が先頭)
static std::vector<unsigned char> frame_rgba;
static int frame_w = 0;
static int frame_h = 0;

// ウィンドウサイズにスケーリングした表示バッファ (BGRA 不透明, alpha=255)
static std::vector<unsigned char> display_bg;
static int client_w = 0;
static int client_h = 0;
// ウィンドウ 1px あたりのシーン px 数 (シーン→ウィンドウの縮尺の逆数)
static float scene_per_window = 1.0f;

enum class Mode { Idle, PenDraw };
static Mode mode = Mode::Idle;

// レンダリング中の再入を防ぐ。
static bool render_in_progress = false;

// L-SMASH WorksのIndex作成中にlwinputの進捗ダイアログが表示される。
// L-SMASH側のメッセージポンプからこのタイマーが実行されるため、その間に
// call_read_section_param()へ再入すると入力処理中のロックと競合し得る。
static bool is_lwinput_busy() {
    return FindWindowW(nullptr, L"lwinput") != nullptr;
}

static std::wstring get_config_path() {
    wchar_t path[MAX_PATH] = {};
    if (!GetModuleFileNameW(module_instance, path, MAX_PATH)) return {};
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return {};
    *(slash + 1) = L'\0';
    return std::wstring(path) + L"CJFPreviewPenTool.ini";
}

static void load_settings() {
    const std::wstring path = get_config_path();
    if (!path.empty())
        checkbox_polling_enabled = GetPrivateProfileIntW(
            L"General", L"EnableCheckboxPolling", 1, path.c_str()) != 0;
}

static void save_settings() {
    const std::wstring path = get_config_path();
    if (!path.empty())
        WritePrivateProfileStringW(
            L"General", L"EnableCheckboxPolling",
            checkbox_polling_enabled ? L"1" : L"0", path.c_str());
}

static void update_poll_timer() {
    if (!frame_window) return;
    if (checkbox_polling_enabled)
        SetTimer(frame_window, PICK_POLL_TIMER_ID, PICK_POLL_INTERVAL_MS, nullptr);
    else
        KillTimer(frame_window, PICK_POLL_TIMER_ID);
}

static LRESULT CALLBACK config_wnd_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CREATE: {
        HWND checkbox = CreateWindowExW(
            0, L"BUTTON", L"チェックボックス式起動を有効にする",
            WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX | BS_OWNERDRAW,
            16, 16, 300, 24, hwnd, reinterpret_cast<HMENU>(CONFIG_CHECKBOX_ID),
            module_instance, nullptr);
        SendMessageW(checkbox, BM_SETCHECK,
            checkbox_polling_enabled ? BST_CHECKED : BST_UNCHECKED, 0);
        CreateWindowExW(
            0, L"BUTTON", L"閉じる", WS_VISIBLE | WS_CHILD | BS_OWNERDRAW,
            236, 52, 80, 26, hwnd, reinterpret_cast<HMENU>(IDCANCEL),
            module_instance, nullptr);
        return 0;
    }
    case WM_ERASEBKGND: {
        RECT rc = {};
        GetClientRect(hwnd, &rc);
        HBRUSH brush = CreateSolidBrush(CJF_UI_BG);
        FillRect(reinterpret_cast<HDC>(wparam), &rc, brush);
        DeleteObject(brush);
        return 1;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        HDC dc = reinterpret_cast<HDC>(wparam);
        SetTextColor(dc, CJF_UI_TEXT);
        SetBkColor(dc, CJF_UI_BG);
        static HBRUSH brush = CreateSolidBrush(CJF_UI_BG);
        return reinterpret_cast<LRESULT>(brush);
    }
    case WM_DRAWITEM: {
        LPDRAWITEMSTRUCT dis = reinterpret_cast<LPDRAWITEMSTRUCT>(lparam);
        if (dis->CtlID == CONFIG_CHECKBOX_ID) {
            SetBkMode(dis->hDC, TRANSPARENT);
            SetTextColor(dis->hDC, CJF_UI_TEXT);
            RECT box = dis->rcItem;
            box.right = box.left + 16;
            box.bottom = box.top + 16;
            HBRUSH bg = CreateSolidBrush(CJF_UI_BG);
            FillRect(dis->hDC, &box, bg);
            DeleteObject(bg);
            HBRUSH border = CreateSolidBrush(CJF_UI_BORDER);
            FrameRect(dis->hDC, &box, border);
            DeleteObject(border);
            if (IsDlgButtonChecked(hwnd, CONFIG_CHECKBOX_ID) == BST_CHECKED) {
                HPEN pen = CreatePen(PS_SOLID, 2, CJF_UI_TEXT);
                HGDIOBJ old_pen = SelectObject(dis->hDC, pen);
                MoveToEx(dis->hDC, box.left + 3, box.top + 8, nullptr);
                LineTo(dis->hDC, box.left + 7, box.bottom - 3);
                LineTo(dis->hDC, box.right - 3, box.top + 3);
                SelectObject(dis->hDC, old_pen);
                DeleteObject(pen);
            }
            RECT text = dis->rcItem;
            text.left += 24;
            DrawTextW(dis->hDC, L"チェックボックス式起動を有効にする", -1, &text,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            return TRUE;
        }
        if (dis->CtlID != IDCANCEL) break;
        HBRUSH brush = CreateSolidBrush(
            (dis->itemState & ODS_SELECTED) ? CJF_UI_BG : CJF_UI_BUTTON);
        FillRect(dis->hDC, &dis->rcItem, brush);
        DeleteObject(brush);
        SetBkMode(dis->hDC, TRANSPARENT);
        SetTextColor(dis->hDC, CJF_UI_TEXT);
        DrawTextW(dis->hDC, L"閉じる", -1, &dis->rcItem,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        HBRUSH border = CreateSolidBrush(CJF_UI_BORDER);
        FrameRect(dis->hDC, &dis->rcItem, border);
        DeleteObject(border);
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wparam) == CONFIG_CHECKBOX_ID && HIWORD(wparam) == BN_CLICKED) {
            checkbox_polling_enabled = (IsDlgButtonChecked(hwnd, CONFIG_CHECKBOX_ID) == BST_CHECKED);
            save_settings();
            update_poll_timer();
            InvalidateRect(GetDlgItem(hwnd, CONFIG_CHECKBOX_ID), nullptr, TRUE);
            return 0;
        }
        if (LOWORD(wparam) == IDCANCEL) {
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        config_window = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

static void open_config_window(HWND owner, HINSTANCE dll_hinst) {
    if (config_window && IsWindow(config_window)) {
        ShowWindow(config_window, SW_SHOWNORMAL);
        SetForegroundWindow(config_window);
        return;
    }
    static bool class_registered = false;
    if (!class_registered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = config_wnd_proc;
        wc.hInstance = dll_hinst ? dll_hinst : module_instance;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpszClassName = L"CJFPreviewPenConfig";
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return;
        class_registered = true;
    }
    config_window = CreateWindowExW(
        WS_EX_DLGMODALFRAME, L"CJFPreviewPenConfig", L"CJF ペンツールの設定",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, 340, 125,
        owner, nullptr, dll_hinst ? dll_hinst : module_instance, nullptr);
    if (config_window) {
        ShowWindow(config_window, SW_SHOWNORMAL);
        UpdateWindow(config_window);
    }
}

// 記録中のストローク（シーン座標 + モード開始からの ms）
// 左ドラッグ 1 回 = 1 ストローク。strokes に書き順で積み、ペンアップ（マウス解放）で区切る
struct PenPoint {
    double sx, sy;
    DWORD t;
    double acc; // ストローク先頭からの累積長（シーン px）。プレビュー描画の二分探索用に保持
};
static std::vector<std::vector<PenPoint>> strokes;
static DWORD pen_mode_start = 0;
static bool pen_down = false;

// ペンモード中の Undo/Redo（ストローク単位）
// undo_stack: 取り消したストロークの履歴 / redo_stack: やり直し用の履歴
// ペンモード開始〜終了（確定/キャンセル）まで有効。新しいストロークを描くと redo はクリアされる。
static std::vector<std::vector<PenPoint>> undo_stack;
static std::vector<std::vector<PenPoint>> redo_stack;

// ペンモード中のホットキー（Ctrl+Z / Ctrl+Y）。フォーカスが裏のウィンドウ
// （オブジェクト設定等）にあっても吸われずに Undo/Redo を実行するために使う。
// ペンモード開始時に登録し、終了（hide_frame）時に解除する。
#define HOTKEY_UNDO_ID 0x5001
#define HOTKEY_REDO_ID 0x5002



// プレビュー用の設定（ペンツール.obj2 から読み取る。未読時は黒・8px）
static COLORREF preview_color = RGB(0, 0, 0);
static int preview_width = 8;
static int preview_taper_in = 0;  // 入り長さ（線幅の倍）
static int preview_taper_out = 0; // 抜き長さ（線幅の倍）
static int preview_taperw_in = 0; // 入り太さ（%）
static int preview_taperw_out = 0;// 抜き太さ（%）

//-----------------------------------------------------------------------------
// フレームレンダリング (EDIT_HANDLE::rendering_scene_video)
//-----------------------------------------------------------------------------

struct RenderCtx {
    unsigned char* dst; // RGBA コピー先 (frame_rgba.data())
    int w, h;
    volatile LONG copied;
    HWND notify_window;
    int frame;
    DWORD started_at;
};

static constexpr UINT WM_CJF_RENDER_COMPLETE = WM_APP + 10;

// レンダリング完了時にイベント通知スレッドから呼ばれる。
// バッファはコールバック中のみ有効なので、即座にコピーして完了を通知する。
static void on_rendered(void* param, int frame, const void* buffer, int width, int height, int pitch) {
    RenderCtx* ctx = static_cast<RenderCtx*>(param);
    if (ctx->dst && buffer && ctx->w == width && ctx->h == height) {
        const unsigned char* src = static_cast<const unsigned char*>(buffer);
        unsigned char* dst = ctx->dst;
        for (int y = 0; y < height; ++y) {
            memcpy(dst + static_cast<size_t>(y) * width * 4,
                   src + static_cast<size_t>(y) * pitch,
                   static_cast<size_t>(width) * 4);
        }
        InterlockedExchange(&ctx->copied, 1);
    }
    if (!PostMessageW(ctx->notify_window, WM_CJF_RENDER_COMPLETE, 0,
                      reinterpret_cast<LPARAM>(ctx)))
        delete ctx;
}

// 指定フレームのシーン出力をレンダリングし、RGBA バッファにコピーする。
// 戻り値: レンダリング受理かつバッファ取得成功なら true。
static bool render_current_frame(int frame, unsigned char* dst, int w, int h) {
    if (!edit_handle) return false;
    RenderCtx* ctx = new (std::nothrow) RenderCtx{ dst, w, h, 0, frame_window, frame, GetTickCount() };
    if (!ctx) return false;
    if (!edit_handle->rendering_scene_video(frame, ctx, on_rendered)) {
        delete ctx;
        return false;
    }
    return true;
}

//-----------------------------------------------------------------------------
// 表示バッファ生成
//-----------------------------------------------------------------------------

static int clamp_int(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// frame_rgba (RGBA, シーン解像度) をウィンドウサイズへバイリニア補間で縮小し、
// display_bg (BGRA, alpha=255) を作る。ウィンドウ開いた時に 1 回だけ実行。
static void build_display_buffer() {
    display_bg.assign(static_cast<size_t>(client_w) * client_h * 4, 0);
    if (frame_w < 1 || frame_h < 1) return;

    for (int y = 0; y < client_h; ++y) {
        float sy = (static_cast<float>(y) + 0.5f) * scene_per_window - 0.5f;
        int y0 = clamp_int(static_cast<int>(std::floorf(sy)), 0, frame_h - 1);
        int y1 = clamp_int(y0 + 1, 0, frame_h - 1);
        float fy = std::max(0.0f, std::min(1.0f, sy - y0));
        for (int x = 0; x < client_w; ++x) {
            float sx = (static_cast<float>(x) + 0.5f) * scene_per_window - 0.5f;
            int x0 = clamp_int(static_cast<int>(std::floorf(sx)), 0, frame_w - 1);
            int x1 = clamp_int(x0 + 1, 0, frame_w - 1);
            float fx = std::max(0.0f, std::min(1.0f, sx - x0));

            const unsigned char* p00 = frame_rgba.data() + (static_cast<size_t>(y0) * frame_w + x0) * 4;
            const unsigned char* p10 = frame_rgba.data() + (static_cast<size_t>(y0) * frame_w + x1) * 4;
            const unsigned char* p01 = frame_rgba.data() + (static_cast<size_t>(y1) * frame_w + x0) * 4;
            const unsigned char* p11 = frame_rgba.data() + (static_cast<size_t>(y1) * frame_w + x1) * 4;

            // RGBA → BGRA 変換も兼ねる
            unsigned char* d = display_bg.data() + (static_cast<size_t>(y) * client_w + x) * 4;
            for (int c = 0; c < 3; ++c) {
                float v = (p00[c] * (1 - fx) + p10[c] * fx) * (1 - fy) +
                          (p01[c] * (1 - fx) + p11[c] * fx) * fy;
                d[2 - c] = static_cast<unsigned char>(v + 0.5f);
            }
            d[3] = 255;
        }
    }
}

//-----------------------------------------------------------------------------
// オーバーレイ描画 (ウィンドウ == 表示バッファ)
//-----------------------------------------------------------------------------

static void draw_hint(HDC dc, int w, int h) {
    // 上部に操作ガイド（黒バー + 白文字）
    RECT bar = { 0, 0, w, 36 };
    HBRUSH br = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(dc, &bar, br);
    DeleteObject(br);
    RECT tr = { 8, 0, w - 8, 36 };
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    HGDIOBJ old_font = SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
    wchar_t hint[128] = {};
    swprintf_s(hint, L"左ドラッグで描画 / 右クリックで確定 / Escでキャンセル");
    DrawTextW(dc, hint, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, old_font);
}

// 記録済みストローク（複数）をウィンドウ座標で描く（プレビュー）
// 入り抜き（preview_taper_in / preview_taper_out / preview_taperw_in / preview_taperw_out）を反映して描く。
// GDI は線ごとに太さ固定なので、入り抜き区間を細かく分割して
// 各線分の太さを変えることで擬似的に可変幅を表現する。
static void draw_stroke_preview(HDC dc, int w, int h) {
    if (strokes.empty()) return;
    int pen_w = std::max(1, static_cast<int>(std::lroundf(preview_width / scene_per_window)));
    HPEN pen = CreatePen(PS_GEOMETRIC | PS_ENDCAP_ROUND | PS_JOIN_ROUND, pen_w, preview_color);
    HGDIOBJ old = SelectObject(dc, pen);
    for (const auto& st : strokes) {
        if (st.empty()) continue;
        int x0 = static_cast<int>(std::lroundf(st[0].sx / scene_per_window));
        int y0 = static_cast<int>(std::lroundf(st[0].sy / scene_per_window));
        if (st.size() == 1) {
            // ドット（丸端ペンで長さ0の線）
            MoveToEx(dc, x0, y0, nullptr);
            LineTo(dc, x0, y0);
            continue;
        }

        // 累積長は PenPoint.acc に保持済み（begin/extend/end で更新）
        const double total = st.back().acc;

        const bool taper_on = total > 0 &&
            ((preview_taper_in > 0 && preview_taperw_in < 100) ||
             (preview_taper_out > 0 && preview_taperw_out < 100));
        if (!taper_on) {
            // 入り抜きなし: 従来どおり 1 本の折れ線
            MoveToEx(dc, x0, y0, nullptr);
            for (size_t i = 1; i < st.size(); ++i) {
                LineTo(dc,
                    static_cast<int>(std::lroundf(st[i].sx / scene_per_window)),
                    static_cast<int>(std::lroundf(st[i].sy / scene_per_window)));
            }
            continue;
        }

        // 入り・抜き区間（シーン px）。ストロークが短い場合はそれぞれ半分ずつに収める
        const double seg_in = std::min(static_cast<double>(preview_taper_in) * preview_width, total * 0.5);
        const double seg_out = std::min(static_cast<double>(preview_taper_out) * preview_width, total * 0.5);
        if (seg_in <= 0 && seg_out <= 0) {
            MoveToEx(dc, x0, y0, nullptr);
            for (size_t i = 1; i < st.size(); ++i) {
                LineTo(dc,
                    static_cast<int>(std::lroundf(st[i].sx / scene_per_window)),
                    static_cast<int>(std::lroundf(st[i].sy / scene_per_window)));
            }
            continue;
        }

        // 累積長上の位置 d の座標を線形補間で求める（acc は単調増加なので二分探索）
        auto point_at = [&](double d, double* px, double* py) {
            if (d <= 0) { *px = st[0].sx; *py = st[0].sy; return; }
            if (d >= total) { *px = st.back().sx; *py = st.back().sy; return; }
            // acc[i] <= d < acc[i+1] となる i を二分探索
            size_t lo = 0, hi = st.size() - 1;
            while (lo + 1 < hi) {
                size_t mid = (lo + hi) / 2;
                if (st[mid].acc <= d) lo = mid; else hi = mid;
            }
            double seglen = st[lo + 1].acc - st[lo].acc;
            double u = seglen > 0 ? (d - st[lo].acc) / seglen : 0;
            *px = st[lo].sx + (st[lo + 1].sx - st[lo].sx) * u;
            *py = st[lo].sy + (st[lo + 1].sy - st[lo].sy) * u;
        };
        // 位置 d での幅倍率（obj2 の taper_ratio と同じ）
        auto ratio_at = [&](double d) -> double {
            const double tw_in = preview_taperw_in / 100.0;
            const double tw_out = preview_taperw_out / 100.0;
            if (seg_in > 0 && d <= seg_in) return tw_in + (1.0 - tw_in) * (d / seg_in);
            if (seg_out > 0 && d >= total - seg_out) return tw_out + (1.0 - tw_out) * ((total - d) / seg_out);
            return 1.0;
        };
        // 線分 [d0, d1] をその中点の太さで描く（太さが変わるときだけペンを作り直す）
        auto draw_line = [&](double d0, double d1) {
            double px0, py0, px1, py1;
            point_at(d0, &px0, &py0);
            point_at(d1, &px1, &py1);
            int wpx = std::max(1, static_cast<int>(std::lroundf(pen_w * ratio_at((d0 + d1) * 0.5))));
            if (wpx != pen_w) {
                HPEN p = CreatePen(PS_GEOMETRIC | PS_ENDCAP_ROUND | PS_JOIN_ROUND, wpx, preview_color);
                SelectObject(dc, p);
                MoveToEx(dc,
                    static_cast<int>(std::lroundf(px0 / scene_per_window)),
                    static_cast<int>(std::lroundf(py0 / scene_per_window)), nullptr);
                LineTo(dc,
                    static_cast<int>(std::lroundf(px1 / scene_per_window)),
                    static_cast<int>(std::lroundf(py1 / scene_per_window)));
                SelectObject(dc, pen);
                DeleteObject(p);
            } else {
                MoveToEx(dc,
                    static_cast<int>(std::lroundf(px0 / scene_per_window)),
                    static_cast<int>(std::lroundf(py0 / scene_per_window)), nullptr);
                LineTo(dc,
                    static_cast<int>(std::lroundf(px1 / scene_per_window)),
                    static_cast<int>(std::lroundf(py1 / scene_per_window)));
            }
        };

        // 分割数（入り抜き区間をウィンドウ 2px 刻みで分割。上限 64 で負荷を抑える）
        int steps_in = std::max(1, std::min(64, static_cast<int>(std::ceil(seg_in / (2.0 * scene_per_window)))));
        int steps_out = std::max(1, std::min(64, static_cast<int>(std::ceil(seg_out / (2.0 * scene_per_window)))));
        // 始端区間（入り）
        if (seg_in > 0) {
            for (int i = 0; i < steps_in; ++i) {
                draw_line(seg_in * i / steps_in, seg_in * (i + 1) / steps_in);
            }
        }
        // 中央区間: 実際の頂点をたどる折れ線（入り区間の終わりから抜き区間の始まりまで）。
        // ここを 1 本の直線で結ぶと、曲線が弦（直線）になってしまう。
        if (seg_in < total - seg_out) {
            double cx0, cy0, cx1, cy1;
            point_at(seg_in, &cx0, &cy0);
            point_at(total - seg_out, &cx1, &cy1);
            MoveToEx(dc,
                static_cast<int>(std::lroundf(cx0 / scene_per_window)),
                static_cast<int>(std::lroundf(cy0 / scene_per_window)), nullptr);
            for (size_t i = 1; i < st.size(); ++i) {
                if (st[i].acc <= seg_in) continue;       // 入り区間内の頂点はスキップ
                if (st[i].acc >= total - seg_out) break; // 抜き区間に入ったら終了
                LineTo(dc,
                    static_cast<int>(std::lroundf(st[i].sx / scene_per_window)),
                    static_cast<int>(std::lroundf(st[i].sy / scene_per_window)));
            }
            LineTo(dc,
                static_cast<int>(std::lroundf(cx1 / scene_per_window)),
                static_cast<int>(std::lroundf(cy1 / scene_per_window)));
        }
        // 終端区間（抜き）
        if (seg_out > 0) {
            for (int i = 0; i < steps_out; ++i) {
                draw_line(total - seg_out * (i + 1) / steps_out, total - seg_out * i / steps_out);
            }
        }
    }
    SelectObject(dc, old);
    DeleteObject(pen);
}

// 再利用する DIB + メモリDC（redraw_frame 毎回の CreateDIBSection/DeleteDC を廃止）
static HDC g_dib_dc = nullptr;
static HBITMAP g_dib = nullptr;
static void* g_dib_bits = nullptr;
static int g_dib_w = 0, g_dib_h = 0;

// ウィンドウサイズの DIB を必要なら確保（サイズが変わったときのみ作り直し）
static bool ensure_dib(int w, int h) {
    if (g_dib_dc && g_dib && g_dib_w == w && g_dib_h == h) return true;
    if (g_dib_dc) { DeleteDC(g_dib_dc); g_dib_dc = nullptr; }
    if (g_dib) { DeleteObject(g_dib); g_dib = nullptr; }
    g_dib_bits = nullptr;
    g_dib_w = w;
    g_dib_h = h;

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    g_dib_dc = CreateCompatibleDC(nullptr);
    if (!g_dib_dc) return false;
    g_dib = CreateDIBSection(g_dib_dc, &bmi, DIB_RGB_COLORS, &g_dib_bits, nullptr, 0);
    return g_dib && g_dib_bits;
}

static void redraw_frame() {
    if (!frame_window || !IsWindowVisible(frame_window)) return;
    if (client_w < 1 || client_h < 1) return;
    if (!ensure_dib(client_w, client_h)) return;

    HGDIOBJ old_bmp = SelectObject(g_dib_dc, g_dib);

    // ベース: 表示バッファ (BGRA 不透明) をコピー
    memcpy(g_dib_bits, display_bg.data(), static_cast<size_t>(client_w) * client_h * 4);

    draw_stroke_preview(g_dib_dc, client_w, client_h);
    draw_hint(g_dib_dc, client_w, client_h);

    RECT win_rc = {};
    GetWindowRect(frame_window, &win_rc);
    POINT pt_src = { 0, 0 };
    POINT pt_dst = { win_rc.left, win_rc.top };
    SIZE size = { client_w, client_h };
    // ピクセルアルファ合成（AC_SRC_ALPHA）は使わない: GDI で 32bit DIB に描画すると
    // 描いたピクセルのアルファが 0 になり、その部分だけクリック判定が透過して
    // 裏のウィンドウ（ホスト）に落ちてしまう（引いた線の上で描けない/確定できない）。
    // AlphaFormat=0 + SourceConstantAlpha=255 ならウィンドウ全体が不透明になり、
    // GDI 描画をそのまま表示できる（アルファ修復ループも不要で、描画負荷は従来と同じ）。
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, 0 };
    static bool ulw_failed = false;
    bool ulw_ok = UpdateLayeredWindow(frame_window, nullptr, &pt_dst, &size, g_dib_dc, &pt_src, 0, &blend, ULW_ALPHA);
    if (!ulw_ok) {
        if (!ulw_failed && logger) {
            ulw_failed = true;
            wchar_t m[256] = {};
            swprintf_s(m, L"[CJF PenTool] UpdateLayeredWindow failed (error=%lu)", GetLastError());
            logger->warn(logger, m);
        }
    } else {
        ulw_failed = false;
    }

    SelectObject(g_dib_dc, old_bmp);
}

//-----------------------------------------------------------------------------
// ペンツール.obj2 の設定読み取り (プレビュー用)
//-----------------------------------------------------------------------------

// 色項目 "ffffff" / "0xffffff" のどちらでも解釈する
static COLORREF parse_color_item(const char* v, COLORREF fallback) {
    if (!v) return fallback;
    const char* p = v;
    while (*p == ' ' || *p == '\t') ++p;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
    unsigned int c = 0;
    int digits = 0;
    while (std::isxdigit(static_cast<unsigned char>(*p)) && digits < 6) {
        int d = 0;
        if (*p >= '0' && *p <= '9') d = *p - '0';
        else if (*p >= 'a' && *p <= 'f') d = *p - 'a' + 10;
        else d = *p - 'A' + 10;
        c = c * 16 + d;
        ++p;
        ++digits;
    }
    if (digits == 0) return fallback;
    return RGB((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
}

struct PenStyleCtx {
    OBJECT_HANDLE obj;
    int frame;
    COLORREF color;
    int width;
    int taper_in;  // 入り長さ（線幅の倍）
    int taper_out; // 抜き長さ（線幅の倍）
    int taperw_in; // 入り太さ（%）
    int taperw_out;// 抜き太さ（%）
    int crashed;
};

// ペンツール.obj2 の 色 / ペンの太さ(px) / 入り抜き を読み取る（プレビュー描画用）
static void read_pen_style_edit(void* param, EDIT_SECTION* edit) {
    PenStyleCtx* ctx = static_cast<PenStyleCtx*>(param);
    __try {
        if (!ctx->obj) return;
        if (edit->count_object_effect(ctx->obj, L"ペンツール") <= 0) return;
        const char* v = edit->get_object_item_value(ctx->obj, L"ペンツール", L"色");
        ctx->color = parse_color_item(v, RGB(0, 0, 0));
        double w = 8.0;
        if (edit->get_object_track_value(ctx->obj, L"ペンツール", L"ペンの太さ(px)", ctx->frame, &w) && w >= 1.0) {
            ctx->width = static_cast<int>(std::lroundf(static_cast<float>(w)));
        }
        double ti = 0.0, to = 0.0, wi = 0.0, wo = 0.0;
        if (edit->get_object_track_value(ctx->obj, L"ペンツール", L"入り長さ", ctx->frame, &ti) && ti >= 0.0) {
            ctx->taper_in = static_cast<int>(std::lroundf(static_cast<float>(ti)));
        }
        if (edit->get_object_track_value(ctx->obj, L"ペンツール", L"抜き長さ", ctx->frame, &to) && to >= 0.0) {
            ctx->taper_out = static_cast<int>(std::lroundf(static_cast<float>(to)));
        }
        if (edit->get_object_track_value(ctx->obj, L"ペンツール", L"入り太さ", ctx->frame, &wi) && wi >= 0.0) {
            ctx->taperw_in = static_cast<int>(std::lroundf(static_cast<float>(wi)));
        }
        if (edit->get_object_track_value(ctx->obj, L"ペンツール", L"抜き太さ", ctx->frame, &wo) && wo >= 0.0) {
            ctx->taperw_out = static_cast<int>(std::lroundf(static_cast<float>(wo)));
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ctx->crashed = 1;
    }
}

//-----------------------------------------------------------------------------
// モード制御
//-----------------------------------------------------------------------------

static void hide_frame() {
    if (frame_window && IsWindow(frame_window)) {
        KillTimer(frame_window, 2);
        KillTimer(frame_window, CONFIRM_RETRY_TIMER_ID);
        ShowWindow(frame_window, SW_HIDE);
    }
    mode = Mode::Idle;
    pen_down = false;
    strokes.clear();
    undo_stack.clear();
    redo_stack.clear();
    // ホットキー解除（ペンモード終了時。登録されていなくても無害）
    UnregisterHotKey(frame_window, HOTKEY_UNDO_ID);
    UnregisterHotKey(frame_window, HOTKEY_REDO_ID);
    pick_trigger_detected = false; // チェックポーリングの再検知を許可
    if (GetCapture() == frame_window) ReleaseCapture();
    if (host_window && IsWindow(host_window)) SetFocus(host_window);
}

static void finish_pen_mode(RenderCtx* ctx) {
    if (!ctx->copied) {
        frame_rgba.clear();
        render_in_progress = false;
        if (pick_trigger_detected) {
            pick_trigger_detected = false;
            reset_pen_checkbox();
        }
        delete ctx;
        return;
    }
    frame_w = ctx->w;
    frame_h = ctx->h;
    const float max_w = 1280.0f, max_h = 800.0f;
    float window_per_scene = std::min(1.0f, max_w / static_cast<float>(frame_w));
    window_per_scene = std::min(window_per_scene, max_h / static_cast<float>(frame_h));
    scene_per_window = 1.0f / window_per_scene;
    client_w = std::max(1, static_cast<int>(std::lroundf(frame_w * window_per_scene)));
    client_h = std::max(1, static_cast<int>(std::lroundf(frame_h * window_per_scene)));
    build_display_buffer();

    preview_color = RGB(0, 0, 0);
    preview_width = 8;
    preview_taper_in = preview_taper_out = 0;
    preview_taperw_in = preview_taperw_out = 0;
    PenStyleCtx style = { pen_trigger_object, ctx->frame, preview_color, preview_width, 0, 0, 0, 0, 0 };
    if (style.obj) {
        edit_handle->call_read_section_param(&style, read_pen_style_edit);
        if (!style.crashed) {
            preview_color = style.color;
            preview_width = std::max(1, style.width);
            preview_taper_in = std::max(0, style.taper_in);
            preview_taper_out = std::max(0, style.taper_out);
            preview_taperw_in = std::max(0, std::min(100, style.taperw_in));
            preview_taperw_out = std::max(0, std::min(100, style.taperw_out));
        }
    }

    RECT host_rc = {};
    GetWindowRect(host_window, &host_rc);
    int x = host_rc.left + (host_rc.right - host_rc.left - client_w) / 2;
    int y = host_rc.top + (host_rc.bottom - host_rc.top - client_h) / 2;
    SetWindowPos(frame_window, HWND_TOPMOST, x, y, client_w, client_h,
                 SWP_SHOWWINDOW | SWP_NOACTIVATE);
    mode = Mode::PenDraw;
    strokes.clear();
    undo_stack.clear();
    redo_stack.clear();
    pen_down = false;
    pen_mode_start = GetTickCount();
    RegisterHotKey(frame_window, HOTKEY_UNDO_ID, MOD_CONTROL | MOD_NOREPEAT, 'Z');
    RegisterHotKey(frame_window, HOTKEY_REDO_ID, MOD_CONTROL | MOD_NOREPEAT, 'Y');
    redraw_frame();
    SetTimer(frame_window, 2, 33, nullptr);
    render_in_progress = false;
    delete ctx;
}

static void begin_pen_mode() {
    if (mode != Mode::Idle || render_in_progress || !frame_window || !edit_handle) return;
    render_in_progress = true;

    EDIT_INFO info = {};
    edit_handle->get_edit_info(&info, sizeof(info));
    if (info.width < 1 || info.height < 1) {
        render_in_progress = false;
        if (pick_trigger_detected) {
            pick_trigger_detected = false;
            reset_pen_checkbox();
        }
        if (logger) logger->warn(logger, L"[CJF PenTool] no scene (invalid size), pen mode aborted");
        return;
    }

    // 現在フレームをレンダリング (wait_rendering_task は参照ロック・更新ロック中だと
    // デッドロックするため、この関数はメニューコールバックではなく
    // PostMessage で遅延されたメッセージループ側から呼ばれる)
    frame_rgba.assign(static_cast<size_t>(info.width) * info.height * 4, 0);
    if (!render_current_frame(info.frame, frame_rgba.data(), info.width, info.height)) {
        frame_rgba.clear();
        render_in_progress = false;
        if (pick_trigger_detected) {
            pick_trigger_detected = false;
            reset_pen_checkbox();
        }
        return;
    }
    // 完了後の表示処理はWM_CJF_RENDER_COMPLETEから続行する。
    return;
#if 0
    DWORD t0 = GetTickCount();
    bool ok = render_current_frame(info.frame, frame_rgba.data(), info.width, info.height);
    DWORD render_ms = GetTickCount() - t0;
    if (!ok) {
        frame_rgba.clear();
        render_in_progress = false;
        if (pick_trigger_detected) {
            pick_trigger_detected = false;
            reset_pen_checkbox();
        }
        if (logger) {
            wchar_t m[256] = {};
            swprintf_s(m, L"[CJF PenTool] rendering_scene_video failed (frame=%d, %lu ms). Output in progress?", info.frame, render_ms);
            logger->warn(logger, m);
        }
        return;
    }
    frame_w = info.width;
    frame_h = info.height;

    // ウィンドウサイズ: シーンを最大 1280x800 に収める (等倍以下は 1:1)
    const float max_w = 1280.0f, max_h = 800.0f;
    float window_per_scene = std::min(1.0f, max_w / static_cast<float>(frame_w));
    window_per_scene = std::min(window_per_scene, max_h / static_cast<float>(frame_h));
    scene_per_window = 1.0f / window_per_scene;
    client_w = std::max(1, static_cast<int>(std::lroundf(frame_w * window_per_scene)));
    client_h = std::max(1, static_cast<int>(std::lroundf(frame_h * window_per_scene)));
    build_display_buffer();

    // プレビュー用の色・太さ・入り抜きを対象オブジェクトから読み取る
    preview_color = RGB(0, 0, 0);
    preview_width = 8;
    preview_taper_in = 0;
    preview_taper_out = 0;
    preview_taperw_in = 0;
    preview_taperw_out = 0;
    PenStyleCtx style = { pen_trigger_object, info.frame, preview_color, preview_width, 0, 0, 0, 0, 0 };
    if (style.obj) {
        edit_handle->call_read_section_param(&style, read_pen_style_edit);
        if (!style.crashed) {
            preview_color = style.color;
            preview_width = std::max(1, style.width);
            preview_taper_in = std::max(0, style.taper_in);
            preview_taper_out = std::max(0, style.taper_out);
            preview_taperw_in = std::max(0, std::min(100, style.taperw_in));
            preview_taperw_out = std::max(0, std::min(100, style.taperw_out));
        }
    }

    // ホストウィンドウ中央に配置
    RECT host_rc = {};
    GetWindowRect(host_window, &host_rc);
    int x = host_rc.left + (host_rc.right - host_rc.left - client_w) / 2;
    int y = host_rc.top + (host_rc.bottom - host_rc.top - client_h) / 2;

    SetWindowPos(frame_window, HWND_TOPMOST, x, y, client_w, client_h,
                 SWP_SHOWWINDOW | SWP_NOACTIVATE);

    mode = Mode::PenDraw;
    strokes.clear();
    undo_stack.clear();
    redo_stack.clear();
    pen_down = false;
    pen_mode_start = GetTickCount();

    // ペンモード中は Ctrl+Z / Ctrl+Y をグローバルに捕まえる（フォーカスが
    // 裏のウィンドウにあっても Undo/Redo が吸われないようにする）
    // MOD_NOREPEAT: Ctrl を押したまま Z/Y を押し続けてもリピートで複数回発火しないようにする
    RegisterHotKey(frame_window, HOTKEY_UNDO_ID, MOD_CONTROL | MOD_NOREPEAT, 'Z');
    RegisterHotKey(frame_window, HOTKEY_REDO_ID, MOD_CONTROL | MOD_NOREPEAT, 'Y');

    redraw_frame();
    SetTimer(frame_window, 2, 33, nullptr); // Esc ポーリング

    render_in_progress = false;

    if (logger) {
        wchar_t m[512] = {};
        swprintf_s(m,
            L"[CJF PenTool] pen window opened: scene=%dx%d frame=%d window=%dx%d scale=%.4f (drag to draw, right-click to confirm, Esc to cancel)",
            frame_w, frame_h, info.frame, client_w, client_h, scene_per_window);
        logger->log(logger, m);
    }
#endif
}

// チェックボタン式起動: 対象オブジェクトの「ペンモード起動」チェックを OFF に戻す。
static void reset_pen_checkbox() {
    if (!edit_handle) return;
    edit_handle->call_edit_section_param(nullptr, [](void*, EDIT_SECTION* edit) {
        // チェック OFF はエイリアスファイルと同じ "0"/"1" 形式で書く
        int n = edit->get_selected_object_num();
        for (int i = 0; i < n; ++i) {
            OBJECT_HANDLE obj = edit->get_selected_object(i);
            if (!obj) continue;
            if (edit->count_object_effect(obj, L"ペンツール") <= 0) continue;
            const char* v = edit->get_object_item_value(obj, L"ペンツール", L"ペンモード起動");
            if (v && v[0] == '1') {
                edit->set_object_item_value(obj, L"ペンツール", L"ペンモード起動", "0");
            }
        }
        OBJECT_HANDLE obj = edit->get_focus_object();
        if (obj && edit->count_object_effect(obj, L"ペンツール") > 0) {
            const char* v = edit->get_object_item_value(obj, L"ペンツール", L"ペンモード起動");
            if (v && v[0] == '1') {
                edit->set_object_item_value(obj, L"ペンツール", L"ペンモード起動", "0");
            }
        }
    });
}

static void cancel_pen_mode() {
    if (logger) logger->log(logger, L"[CJF PenTool] canceled (Esc)");
    if (pick_trigger_detected) reset_pen_checkbox();
    pen_trigger_object = nullptr;
    hide_frame();
}

// 「線をクリア」: 座標列を空にしてチェックを OFF に戻す（1 つの Undo エントリ）。
// 既に空かつチェック OFF の場合は何もしない（無駄な Undo を積まない）。
static void handle_clear_request() {
    if (!edit_handle) return;
    OBJECT_HANDLE obj = clear_request_object;
    clear_request_object = nullptr;
    if (!obj) return;
    edit_handle->call_edit_section_param(&obj, [](void* param, EDIT_SECTION* edit) {
        OBJECT_HANDLE o = *static_cast<OBJECT_HANDLE*>(param);
        __try {
            if (edit->count_object_effect(o, L"ペンツール") <= 0) return;
            const char* v = edit->get_object_item_value(o, L"ペンツール", L"線の座標(シーンpx・ミリ秒)");
            const char* ck = edit->get_object_item_value(o, L"ペンツール", L"線をクリア");
            bool has_data = v && v[0] != '\0';
            bool ck_on = ck && ck[0] == '1';
            if (!has_data && !ck_on) return;
            edit->set_object_item_value(o, L"ペンツール", L"線の座標(シーンpx・ミリ秒)", "");
            if (ck_on) {
                edit->set_object_item_value(o, L"ペンツール", L"線をクリア", "0");
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    });
    if (logger) logger->log(logger, L"[CJF PenTool] cleared stroke data");
}

//-----------------------------------------------------------------------------
// 確定: ストロークをペンツール.obj2 へ反映
//-----------------------------------------------------------------------------

struct PenApplyCtx {
    std::string existing_pts; // 適用前の「線の座標」（スキャンで読む。追記の元）
    std::string pts;          // 書込む最終座標列 "x,y,t;...|x,y,t;..."
    int pos_x, pos_y;         // 全ストローク中心のオブジェクト座標 (シーン中心原点・Y下向き)
    OBJECT_HANDLE candidates[32]; // 適用対象候補
    int candidate_num;
    int pick_was_on;      // チェック「ペンモード起動」が ON の候補があったか
    int applied;          // 更新できたペンツールエフェクト数
    int checked;          // ペンツールとして識別できたエフェクト数
    int selected;         // 選択中オブジェクト数 (診断用)
    int focus;            // フォーカスオブジェクトを使ったか (診断用)
    int effect_total;     // count_object_effect(ペンツール) の合計 (診断用)
    int pos_failed;       // 標準描画 X/Y の書込に失敗した数 (診断用)
    int crashed;          // コールバック内でアクセス違反を捕捉したか (SEH 保護)
    int crash_stage;      // クラッシュ発生ステージ (診断用)
};

// 適用対象の候補オブジェクトを収集する。
// 優先順位: ① チェック/右クリックメニューで保存した対象 (pen_trigger_object)
//          ② 選択中オブジェクト ③ 無ければオブジェクト設定ウィンドウのフォーカスオブジェクト
static void collect_apply_candidates(PenApplyCtx* ctx, EDIT_SECTION* edit) {
    ctx->candidate_num = 0;
    if (pen_trigger_object) {
        ctx->candidates[ctx->candidate_num++] = pen_trigger_object;
        ctx->focus = 2; // 診断: トリガー由来
    }
    int n = edit->get_selected_object_num();
    ctx->selected = n;
    for (int i = 0; i < n && ctx->candidate_num < 32; ++i) {
        OBJECT_HANDLE obj = edit->get_selected_object(i);
        if (obj) ctx->candidates[ctx->candidate_num++] = obj;
    }
    if (ctx->candidate_num == 0) {
        OBJECT_HANDLE focus = edit->get_focus_object();
        if (focus) {
            ctx->candidates[ctx->candidate_num++] = focus;
            ctx->focus = 1;
        }
    }
}

// 適用ステップ0 (読み取り・Undo なし): 候補オブジェクトを収集し、チェック
// 「ペンモード起動」が ON の候補があるかを事前確認する。
static void apply_scan_edit(void* param, EDIT_SECTION* edit) {
    PenApplyCtx* ctx = static_cast<PenApplyCtx*>(param);
    __try {
        ctx->crash_stage = 1;
        collect_apply_candidates(ctx, edit);
        ctx->crash_stage = 4;
        for (int ci = 0; ci < ctx->candidate_num; ++ci) {
            OBJECT_HANDLE obj = ctx->candidates[ci];
            if (edit->count_object_effect(obj, L"ペンツール") <= 0) continue;
            const char* v = edit->get_object_item_value(obj, L"ペンツール", L"ペンモード起動");
            if (v && v[0] == '1') {
                ctx->pick_was_on = 1;
            }
            // 追記元の既存座標列（先頭のペンツール候補のものだけ保持）
            if (ctx->existing_pts.empty()) {
                const char* e = edit->get_object_item_value(obj, L"ペンツール", L"線の座標(シーンpx・ミリ秒)");
                if (e) ctx->existing_pts = e;
            }
        }
        ctx->crash_stage = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ctx->crashed = 1;
    }
}

// 適用ステップ1 (Undo エントリ 1): チェック「ペンモード起動」を OFF に戻す。
// 座標・位置の書込 (ステップ2) とは別の編集セクションで書くことで、Undo 1 回では
// 座標・位置だけが戻り、チェックは OFF のままになる (ON に戻されてペンモードが
// 再起動するのを防ぐ)。
static void apply_pick_reset_edit(void* param, EDIT_SECTION* edit) {
    PenApplyCtx* ctx = static_cast<PenApplyCtx*>(param);
    __try {
        ctx->crash_stage = 4;
        for (int ci = 0; ci < ctx->candidate_num; ++ci) {
            OBJECT_HANDLE obj = ctx->candidates[ci];
            if (edit->count_object_effect(obj, L"ペンツール") <= 0) continue;
            const char* v = edit->get_object_item_value(obj, L"ペンツール", L"ペンモード起動");
            if (v && v[0] == '1') {
                edit->set_object_item_value(obj, L"ペンツール", L"ペンモード起動", "0");
            }
        }
        ctx->crash_stage = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ctx->crashed = 1;
    }
}

// 適用ステップ2 (Undo エントリ 2): 線の座標 (ペンツール) と 標準設定 X/Y (標準描画) を書く。
// 位置の書込先は効果名「標準描画」・キー X/Y (範囲指定プラグインで確立済み)。
static void apply_stroke_edit(void* param, EDIT_SECTION* edit) {
    PenApplyCtx* ctx = static_cast<PenApplyCtx*>(param);
    char xbuf[32] = {}, ybuf[32] = {};
    snprintf(xbuf, sizeof(xbuf), "%.2f", static_cast<double>(ctx->pos_x));
    snprintf(ybuf, sizeof(ybuf), "%.2f", static_cast<double>(ctx->pos_y));

    __try {
        for (int ci = 0; ci < ctx->candidate_num; ++ci) {
            OBJECT_HANDLE obj = ctx->candidates[ci];
            ctx->crash_stage = 3;
            int count = edit->count_object_effect(obj, L"ペンツール");
            if (count > 0) ctx->effect_total += count;
            if (count <= 0) continue;
            ++ctx->checked;

            ctx->crash_stage = 4;
            bool pts_ok = edit->set_object_item_value(obj, L"ペンツール", L"線の座標(シーンpx・ミリ秒)", ctx->pts.c_str());
            bool pos_ok =
                edit->set_object_item_value(obj, L"標準描画", L"X", xbuf) &&
                edit->set_object_item_value(obj, L"標準描画", L"Y", ybuf);
            if (pts_ok) {
                ++ctx->applied;
                if (!pos_ok) ++ctx->pos_failed;
            }
        }
        ctx->crash_stage = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ctx->crashed = 1;
    }
}

static void log_apply_failed(const wchar_t* why) {
    if (!logger) return;
    wchar_t m[256] = {};
    swprintf_s(m, L"[CJF PenTool] %s; stroke not applied", why);
    logger->log(logger, m);
}

// "x,y,t;x,y,t;...|x,y,t;..." を座標の配列に変換（追記・中心計算用）
static void parse_pts_string(const std::string& s, std::vector<std::vector<PenPoint>>& out) {
    out.clear();
    if (s.empty()) return;
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t bar = s.find('|', pos);
        std::string st = (bar == std::string::npos) ? s.substr(pos) : s.substr(pos, bar - pos);
        std::vector<PenPoint> pts;
        size_t q = 0;
        while (q <= st.size()) {
            size_t semi = st.find(';', q);
            std::string seg = (semi == std::string::npos) ? st.substr(q) : st.substr(q, semi - q);
            double x = 0, y = 0;
            unsigned long t = 0;
            if (sscanf(seg.c_str(), "%lf,%lf,%lu", &x, &y, &t) == 3) {
                pts.push_back({ x, y, static_cast<DWORD>(t), 0.0 }); // acc はプレビュー専用（既存データでは未使用）
            }
            if (semi == std::string::npos) break;
            q = semi + 1;
        }
        if (!pts.empty()) out.push_back(std::move(pts));
        if (bar == std::string::npos) break;
        pos = bar + 1;
    }
}

// 既存 + 新規ストロークを合成して座標列を作る。
// タイムスタンプは既存の最終 t にペンアップ間隔(300ms)を足した位置から続ける
// （書き順＝再生順が維持される）。
static std::string build_pts_string(const std::string& existing) {
    std::string pts = existing;
    DWORD offset = 0;
    if (!pts.empty()) {
        size_t comma = pts.rfind(',');
        if (comma != std::string::npos) {
            offset = static_cast<DWORD>(strtoul(pts.c_str() + comma + 1, nullptr, 10));
        }
        offset += 300; // ペンアップ（ストローク間）の間隔
    }
    char buf[64];
    for (const auto& st : strokes) {
        if (st.empty()) continue;
        if (!pts.empty()) pts += "|";
        for (size_t i = 0; i < st.size(); ++i) {
            if (i) pts += ";";
            snprintf(buf, sizeof(buf), "%.1f,%.1f,%lu", st[i].sx, st[i].sy,
                     static_cast<unsigned long>(offset + st[i].t));
            pts += buf;
        }
    }
    return pts;
}

// 既存 + 新規ストローク全体の中心をオブジェクト座標（シーン中心原点・Y下向き）で計算
static void compute_combined_center(const std::string& existing, int* pos_x, int* pos_y) {
    std::vector<std::vector<PenPoint>> all;
    parse_pts_string(existing, all);
    for (const auto& st : strokes) {
        if (!st.empty()) all.push_back(st);
    }
    if (all.empty()) {
        *pos_x = 0;
        *pos_y = 0;
        return;
    }
    double min_x = all[0][0].sx, max_x = all[0][0].sx;
    double min_y = all[0][0].sy, max_y = all[0][0].sy;
    for (const auto& st : all) {
        for (const auto& p : st) {
            min_x = std::min(min_x, p.sx);
            max_x = std::max(max_x, p.sx);
            min_y = std::min(min_y, p.sy);
            max_y = std::max(max_y, p.sy);
        }
    }
    int px = static_cast<int>(std::lroundf(static_cast<float>((min_x + max_x) * 0.5 - frame_w * 0.5)));
    int py = static_cast<int>(std::lroundf(static_cast<float>((min_y + max_y) * 0.5 - frame_h * 0.5)));
    *pos_x = clamp_int(px, -8192, 8192);
    *pos_y = clamp_int(py, -8192, 8192);
}

static bool apply_strokes_to_pen_tool() {
    if (!edit_handle || strokes.empty()) return false;

    PenApplyCtx ctx = {};
    ctx.pts.clear();
    ctx.pos_x = 0;
    ctx.pos_y = 0;

    if (logger) {
        size_t total_pts = 0;
        DWORD t_first = 0, t_last = 0;
        bool first_pt = true;
        for (const auto& st : strokes) {
            for (const auto& p : st) {
                total_pts++;
                if (first_pt) {
                    t_first = p.t;
                    t_last = p.t;
                    first_pt = false;
                } else {
                    t_last = p.t;
                }
            }
        }
        wchar_t m[512] = {};
        swprintf_s(m, L"[CJF PenTool] confirm: %d stroke(s), %zu point(s), %lu ms",
            static_cast<int>(strokes.size()), total_pts,
            static_cast<unsigned long>(t_last - t_first));
        logger->log(logger, m);
    }

    // 読み取りスキャン（候補収集 + 既存座標列の取得 + チェックON確認）
    if (!edit_handle->call_read_section_param(&ctx, apply_scan_edit)) {
        log_apply_failed(L"could not enter read section (playback/output in progress?)");
        return false;
    }
    if (ctx.crashed) {
        log_apply_failed(L"access violation in scan");
        return false;
    }

    // 既存 + 新規の合成と中心計算（編集セクション外の純計算）
    ctx.pts = build_pts_string(ctx.existing_pts);
    compute_combined_center(ctx.existing_pts, &ctx.pos_x, &ctx.pos_y);

    // チェック「ペンモード起動」を OFF に（座標・位置の書込と別の Undo エントリ）
    if (ctx.pick_was_on) {
        if (!edit_handle->call_edit_section_param(&ctx, apply_pick_reset_edit)) {
            log_apply_failed(L"could not enter edit section for checkbox (playback/output in progress?)");
            return false;
        }
        if (ctx.crashed) {
            log_apply_failed(L"access violation in checkbox reset");
            return false;
        }
    }

    // 座標列 + 標準描画 X/Y の書込
    if (!edit_handle->call_edit_section_param(&ctx, apply_stroke_edit)) {
        log_apply_failed(L"could not enter edit section for stroke (playback/output in progress?)");
        return false;
    }

    if (logger) {
        wchar_t m[384] = {};
        if (ctx.crashed) {
            swprintf_s(m, L"[CJF PenTool] access violation caught in edit callback at stage %d (protected); stroke not applied", ctx.crash_stage);
        } else if (ctx.applied > 0) {
            if (ctx.pos_failed > 0) {
                swprintf_s(m, L"[CJF PenTool] applied stroke to %d CJF ペンツール effect(s) (pts len=%zu), but 標準 X/Y write failed",
                    ctx.applied, ctx.pts.size());
            } else {
                swprintf_s(m, L"[CJF PenTool] applied stroke to %d CJF ペンツール effect(s) (pts len=%zu, pos %d,%d)",
                    ctx.applied, ctx.pts.size(), ctx.pos_x, ctx.pos_y);
            }
        } else if (ctx.checked == 0) {
            if (ctx.selected == 0 && ctx.focus == 0) {
                swprintf_s(m, L"[CJF PenTool] no object selected or focused; stroke not applied");
            } else {
                swprintf_s(m, L"[CJF PenTool] %d object(s) found, total ペンツール effect count=%d, but no CJF ペンツール object identified; stroke not applied",
                    ctx.selected + ctx.focus, ctx.effect_total);
            }
        } else {
            swprintf_s(m, L"[CJF PenTool] failed to apply stroke (set_object_item_value returned false)");
        }
        logger->log(logger, m);
    }
    return ctx.applied > 0;
}

static void retry_pen_confirm() {
    if (mode != Mode::PenDraw || strokes.empty()) return;
    if (apply_strokes_to_pen_tool()) {
        KillTimer(frame_window, CONFIRM_RETRY_TIMER_ID);
        confirm_retry_count = 0;
        pen_trigger_object = nullptr;
        hide_frame();
        return;
    }
    if (++confirm_retry_count >= CONFIRM_RETRY_MAX) {
        KillTimer(frame_window, CONFIRM_RETRY_TIMER_ID);
        confirm_retry_count = 0;
        if (logger) logger->warn(logger, L"[CJF PenTool] confirm retry limit reached; strokes kept for manual retry");
    }
}

//-----------------------------------------------------------------------------
// ウィンドウプロシージャ (入力捕捉)
//-----------------------------------------------------------------------------

static void clamp_to_client(POINT* pt) {
    if (pt->x < 0) pt->x = 0;
    if (pt->y < 0) pt->y = 0;
    if (pt->x > client_w) pt->x = client_w;
    if (pt->y > client_h) pt->y = client_h;
}

static void window_to_scene(const POINT& pt, double* sx, double* sy) {
    *sx = pt.x * scene_per_window;
    *sy = pt.y * scene_per_window;
}

// 左ドラッグ開始: 新しいストロークを追加（既存ストロークは保持 = 描き足し）
static void begin_stroke(const POINT& pt) {
    pen_down = true;
    SetCapture(frame_window);
    // 新しいストロークを描くと redo 履歴は無効（標準的な Undo/Redo の挙動）
    redo_stack.clear();
    std::vector<PenPoint> st;
    PenPoint pp;
    window_to_scene(pt, &pp.sx, &pp.sy);
    pp.t = GetTickCount() - pen_mode_start;
    pp.acc = 0.0; // ストローク先頭
    st.push_back(pp);
    strokes.push_back(std::move(st));
    redraw_frame();
}

static void extend_stroke(const POINT& pt) {
    if (!pen_down || strokes.empty()) return;
    double sx, sy;
    window_to_scene(pt, &sx, &sy);
    std::vector<PenPoint>& st = strokes.back();
    const PenPoint& last = st.back();
    double dx = sx - last.sx;
    double dy = sy - last.sy;
    // シーン座標で 1px 以上動いたら記録（補間なし）。
    // イベント間を距離刻みで補間すると、速く描くほど 1 イベントで追加する点が
    // 数十個になり、点の総数が爆発的に増える（→ プレビュー描画・再生メッシュ生成が
    // 二次関数的に重くなり、AviUtl2 ごと固まることがある）。
    // 補間を廃止して従来の 1px 閾値方式に戻す（ワープは別途対策する）。
    if (dx * dx + dy * dy >= 1.0) {
        PenPoint pp = { sx, sy, GetTickCount() - pen_mode_start,
                        last.acc + std::sqrt(dx * dx + dy * dy) };
        st.push_back(pp);
        redraw_frame();
    }
}

static void end_stroke(const POINT& pt) {
    if (!pen_down) return;
    pen_down = false;
    ReleaseCapture();
    if (strokes.empty()) return;
    // マウスアップ位置をストローク終端として追加
    double sx, sy;
    window_to_scene(pt, &sx, &sy);
    std::vector<PenPoint>& st = strokes.back();
    const PenPoint& last = st.back();
    double dx = sx - last.sx;
    double dy = sy - last.sy;
    if (dx * dx + dy * dy >= 0.25) {
        PenPoint pp = { sx, sy, GetTickCount() - pen_mode_start,
                        last.acc + std::sqrt(dx * dx + dy * dy) };
        st.push_back(pp);
    }
    redraw_frame();
}

// Ctrl+Z: 最後のストロークを元に戻す（ストローク単位）
static void undo_stroke() {
    if (mode != Mode::PenDraw || pen_down || strokes.empty()) return;
    redo_stack.push_back(std::move(strokes.back()));
    strokes.pop_back();
    redraw_frame();
}

// Ctrl+Y: 取り消したストロークをやり直す（ストローク単位）
static void redo_stroke() {
    if (mode != Mode::PenDraw || pen_down || redo_stack.empty()) return;
    strokes.push_back(std::move(redo_stack.back()));
    redo_stack.pop_back();
    redraw_frame();
}

static void confirm_stroke() {
    if (mode != Mode::PenDraw) return;
    if (strokes.empty()) {
        // 何も描かずに確定: 変更なしで閉じる。
        // このまま hide_frame だけだと「ペンモード起動」チェックが ON のまま残り、
        // ポーリングが再検知してペンモードが再起動してしまうので、チェックを OFF にする。
        if (logger) logger->log(logger, L"[CJF PenTool] confirmed with no stroke (no change)");
        if (pick_trigger_detected) reset_pen_checkbox();
        pen_trigger_object = nullptr;
        hide_frame();
        return;
    }
    if (!apply_strokes_to_pen_tool()) {
        // 1段目だけ成功する場合があるため、UIイベントを返してから自動再試行する。
        confirm_retry_count = 0;
        SetTimer(frame_window, CONFIRM_RETRY_TIMER_ID, CONFIRM_RETRY_INTERVAL_MS, nullptr);
        return;
    }
    pen_trigger_object = nullptr;
    hide_frame();
}

static void request_pen_mode() {
    if (logger) logger->log(logger, L"[CJF PenTool] pen mode requested (deferred to message loop)");
    if (frame_window) PostMessageW(frame_window, WM_APP + 1, 0, 0);
}

// チェックボタン式起動のポーリング: 選択中/フォーカスオブジェクトの ペンツール 効果に
// 「ペンモード起動」チェックが入っているかを確認する。ON を検知したら対象オブジェクトを
// 保存してペンモードを起動する。
static void poll_pen_trigger() {
    if (!edit_handle || !frame_window || pick_trigger_detected) return;
    if (mode != Mode::Idle || render_in_progress) return;
    if (!checkbox_polling_enabled || polling_in_progress) return;
    if (is_lwinput_busy()) return;
    if (GetCapture() != nullptr) return;
    if (static_cast<LONG>(GetTickCount() - polling_backoff_until) < 0) return;
    if (edit_handle->get_edit_state() != EDIT_HANDLE::EDIT_STATE_EDIT) return;
    polling_in_progress = true;
    struct PollGuard { bool& active; ~PollGuard() { active = false; } } guard{ polling_in_progress };

    EDIT_INFO info = {};
    edit_handle->get_edit_info(&info, sizeof(info));

    struct PickCtx {
        int pen;    // 「ペンモード起動」ON のオブジェクト
        int clear;  // 「線をクリア」ON のオブジェクト
        int frame;
        OBJECT_HANDLE pen_obj;
        OBJECT_HANDLE clear_obj;
    } pick = { 0, 0, info.frame, nullptr, nullptr };

    if (!edit_handle->call_read_section_param(&pick, [](void* param, EDIT_SECTION* edit) {
        PickCtx* p = static_cast<PickCtx*>(param);
        auto check_obj = [p](EDIT_SECTION* ed, OBJECT_HANDLE obj) {
            if (!obj || ed->count_object_effect(obj, L"ペンツール") <= 0) return;
            bool on = false;
            if (ed->get_object_check_value(obj, L"ペンツール", L"ペンモード起動", p->frame, &on) && on && !p->pen) {
                p->pen = 1;
                p->pen_obj = obj;
            }
            on = false;
            if (ed->get_object_check_value(obj, L"ペンツール", L"線をクリア", p->frame, &on) && on && !p->clear) {
                p->clear = 1;
                p->clear_obj = obj;
            }
        };
        int n = edit->get_selected_object_num();
        for (int i = 0; i < n; ++i) {
            check_obj(edit, edit->get_selected_object(i));
        }
        check_obj(edit, edit->get_focus_object());
    })) {
        // 一時的に編集側が別の処理中なら、次の数回を飛ばして競合を拡大しない。
        polling_backoff_until = GetTickCount() + 500;
        return;
    }

    // クリアはペンモードより優先（同じオブジェクトで両方 ON の場合はクリアのみ処理）
    if (pick.clear) {
        clear_request_object = pick.clear_obj;
        if (logger) logger->log(logger, L"[CJF PenTool] 線をクリア ON detected");
        if (frame_window) PostMessageW(frame_window, WM_APP + 2, 0, 0);
        return;
    }
    if (pick.pen) {
        pick_trigger_detected = true; // 適用/キャンセルまで再検知しない
        pen_trigger_object = pick.pen_obj;
        if (logger) logger->log(logger, L"[CJF PenTool] ペンモード起動 ON detected, starting pen mode");
        request_pen_mode();
    }
}

// パネル起動時の対象解決。該当オブジェクトが無ければ、現在フレームから
// 3 秒分のペンツールを空きレイヤーへ作成し、そのハンドルを対象にする。
static bool prepare_panel_pen_target() {
    if (!edit_handle) return false;
    EDIT_INFO info = {};
    edit_handle->get_edit_info(&info, sizeof(info));
    struct PanelCtx {
        EDIT_INFO info;
        OBJECT_HANDLE target;
    } ctx = { info, nullptr };
    if (!edit_handle->call_edit_section_param(&ctx, [](void* param, EDIT_SECTION* edit) {
        PanelCtx* p = static_cast<PanelCtx*>(param);
        auto is_target = [](EDIT_SECTION* ed, OBJECT_HANDLE obj) {
            return obj && ed->count_object_effect(obj, L"ペンツール") > 0;
        };
        int n = edit->get_selected_object_num();
        for (int i = 0; i < n && !p->target; ++i) {
            OBJECT_HANDLE obj = edit->get_selected_object(i);
            if (is_target(edit, obj)) p->target = obj;
        }
        if (!p->target) {
            OBJECT_HANDLE obj = edit->get_focus_object();
            if (is_target(edit, obj)) p->target = obj;
        }
        if (p->target) return;

        int fps = p->info.scale > 0 ? p->info.rate / p->info.scale : p->info.rate;
        int length = std::max(1, static_cast<int>(std::lround(3.0 * std::max(1, fps))));
        int first = std::max(0, p->info.layer);
        for (int pass = 0; pass <= p->info.layer_max && !p->target; ++pass) {
            int layer = (first + pass) % std::max(1, p->info.layer_max + 1);
            if (edit->find_object(layer, p->info.frame)) continue;
            p->target = edit->create_object_from_alias(
                pen_object_alias, layer, p->info.frame, length);
            if (p->target) edit->set_focus_object(p->target);
        }
    })) {
        return false;
    }
    if (!ctx.target) return false;
    pen_trigger_object = ctx.target;
    return true;
}

static LRESULT CALLBACK frame_wnd_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    static UINT panel_message = RegisterWindowMessageW(panel_message_name);
    if (message == panel_message) {
        if (prepare_panel_pen_target()) request_pen_mode();
        return 0;
    }
    switch (message) {
    case WM_TIMER: {
        if (wparam == 2 && (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0) {
            cancel_pen_mode();
        }
        if (wparam == PICK_POLL_TIMER_ID) {
            poll_pen_trigger();
        }
        if (wparam == CONFIRM_RETRY_TIMER_ID) {
            retry_pen_confirm();
        }
        return 0;
    }
    case WM_CJF_RENDER_COMPLETE: {
        RenderCtx* ctx = reinterpret_cast<RenderCtx*>(lparam);
        if (ctx) finish_pen_mode(ctx);
        return 0;
    }
    case WM_APP + 1: {
        begin_pen_mode();
        return 0;
    }
    case WM_APP + 2: {
        // 線をクリア（チェック/右クリックメニューから遅延実行）
        handle_clear_request();
        return 0;
    }
    case WM_LBUTTONDOWN: {
        if (mode == Mode::Idle) break;
        POINT pt = { GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
        clamp_to_client(&pt);
        begin_stroke(pt);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (mode == Mode::Idle) break;
        POINT pt = { GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
        clamp_to_client(&pt);
        extend_stroke(pt);
        return 0;
    }
    case WM_LBUTTONUP: {
        if (mode == Mode::Idle) break;
        POINT pt = { GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
        clamp_to_client(&pt);
        end_stroke(pt);
        return 0;
    }
    case WM_RBUTTONDOWN: {
        if (mode == Mode::Idle) break;
        confirm_stroke();
        return 0;
    }
    case WM_HOTKEY: {
        // Ctrl+Z / Ctrl+Y（RegisterHotKey で登録。フォーカスが裏にあっても届く）
        if (wparam == HOTKEY_UNDO_ID) {
            undo_stroke();
            return 0;
        }
        if (wparam == HOTKEY_REDO_ID) {
            redo_stroke();
            return 0;
        }
        break;
    }
    case WM_KEYDOWN: {
        if (wparam == VK_ESCAPE) {
            cancel_pen_mode();
            return 0;
        }
        // RegisterHotKey 失敗時のフォールバック（フォーカスがフレームウィンドウにある場合）
        // ホットキー登録成功時は WM_HOTKEY が優先され、ここには来ない。
        if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) {
            if (wparam == 'Z') {
                undo_stroke();
                return 0;
            }
            if (wparam == 'Y') {
                redo_stroke();
                return 0;
            }
        }
        break;
    }
    case WM_CAPTURECHANGED: {
        // キャプチャが奪われたら（Alt+Tab 等）現在位置でストロークを閉じる。
        // 閉じないままだと終端未確定のストロークが strokes に残り、確定時に途中で切れた線になる。
        if (pen_down && mode == Mode::PenDraw && !strokes.empty()) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            clamp_to_client(&pt);
            pen_down = false; // end_stroke のキャプチャ解放処理をスキップするため直接終端する
            double sx, sy;
            window_to_scene(pt, &sx, &sy);
            std::vector<PenPoint>& st = strokes.back();
            const PenPoint& last = st.back();
            double dx = sx - last.sx;
            double dy = sy - last.sy;
            if (dx * dx + dy * dy >= 0.25) {
                PenPoint pp = { sx, sy, GetTickCount() - pen_mode_start,
                                last.acc + std::sqrt(dx * dx + dy * dy) };
                st.push_back(pp);
            }
            redraw_frame();
        } else {
            // キャプチャが奪われたら描画を止める（モード自体は維持）
            pen_down = false;
        }
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProc(hwnd, message, wparam, lparam);
}

//-----------------------------------------------------------------------------
// プラグインエントリポイント (aviutl2_sdk/plugin2.h の規約に準拠)
//-----------------------------------------------------------------------------

EXTERN_C __declspec(dllexport) DWORD RequiredVersion() {
    return 2003300;
}

EXTERN_C __declspec(dllexport) void InitializeLogger(LOG_HANDLE* handle) {
    // ログ出力は無効化（AviUtl2 のログウィンドウに表示しない）。
    // デバッグ時に復活させる場合は logger = handle; を有効にする。
    // logger = handle;
}

EXTERN_C __declspec(dllexport) bool InitializePlugin(DWORD version) {
    return version >= RequiredVersion();
}

EXTERN_C __declspec(dllexport) void UninitializePlugin() {
    if (frame_window) {
        KillTimer(frame_window, PICK_POLL_TIMER_ID);
        DestroyWindow(frame_window);
        frame_window = nullptr;
    }
    pen_trigger_object = nullptr;
    clear_request_object = nullptr;
    frame_rgba.clear();
    display_bg.clear();
    strokes.clear();
    undo_stack.clear();
    redo_stack.clear();
    if (g_dib) { DeleteObject(g_dib); g_dib = nullptr; }
    if (g_dib_dc) { DeleteDC(g_dib_dc); g_dib_dc = nullptr; }
    g_dib_bits = nullptr;
    g_dib_w = g_dib_h = 0;
    host_window = nullptr;
    edit_handle = nullptr;
    logger = nullptr;
    mode = Mode::Idle;
    pen_down = false;
}

EXTERN_C __declspec(dllexport) COMMON_PLUGIN_TABLE* GetCommonPluginTable(void) {
    return &common_plugin_table;
}

EXTERN_C __declspec(dllexport) void RegisterPlugin(HOST_APP_TABLE* host) {
    if (!host) return;

    edit_handle = host->create_edit_handle();
    if (!edit_handle) return;

    host_window = edit_handle->get_host_app_window();
    if (!host_window) return;

    if (!module_instance) {
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCWSTR>(&RegisterPlugin), &module_instance);
    }

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpszClassName = frame_class_name;
    wc.lpfnWndProc = frame_wnd_proc;
    wc.hInstance = module_instance;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(IDC_CROSS));
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return;

    // 専用ウィンドウ (レイヤード・オーナー付き・アクティブ化しない)
    frame_window = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        frame_class_name, L"CJF Pen Tool", WS_POPUP,
        0, 0, 1, 1,
        host_window, nullptr, module_instance, nullptr);
    if (!frame_window) return;

    load_settings();
    // チェックボタン式起動のポーリングタイマー (常時動作。検知は Idle 時のみ)
    update_poll_timer();

    // 編集セクションを渡さないメニュー登録 (_param 版) + PostMessage 遅延
    host->register_edit_menu_param(L"CJF\\ペンで描く", nullptr, [](void*) {
        request_pen_mode();
    });
    host->register_config_menu(L"CJF\\ペンツール", open_config_window);

    // オブジェクト設定ウィンドウの右クリックメニューに登録。
    // コールバックに OBJECT_HANDLE が直接渡るため、対象オブジェクトを保存して
    // ペンモードの適用時に選択状態へ依存しない。
    host->register_object_item_menu_param(
        L"ペンで描く", true, nullptr, [](void*, OBJECT_HANDLE object, LPCWSTR effect, LPCWSTR item) {
            pen_trigger_object = object;
            if (logger) {
                wchar_t m[256] = {};
                swprintf_s(m, L"[CJF PenTool] context menu: object=%p effect=%s item=%s",
                    object, effect ? effect : L"(effect)", item ? item : L"(all)");
                logger->log(logger, m);
            }
            request_pen_mode();
        });

    // 「線をクリア」も右クリックメニューに登録（遅延実行で座標列を空にする）
    host->register_object_item_menu_param(
        L"線をクリア", true, nullptr, [](void*, OBJECT_HANDLE object, LPCWSTR effect, LPCWSTR item) {
            clear_request_object = object;
            if (frame_window) PostMessageW(frame_window, WM_APP + 2, 0, 0);
        });

    if (logger) {
        logger->log(logger,
            L"[CJF PenTool] initialized. Use ペンツール.obj2 settings checkbox ペンモード起動 / 編集>CJF>ペンで描く to start.");
    }
}

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <d2d1.h>
#include <dwrite.h>
#include <algorithm>
#include <string>
#include <vector>

struct NppData { HWND npp, mainView, secondView; };
struct ShortcutKey;
struct FuncItem {
    wchar_t name[64];
    void (*function)();
    int commandId;
    bool checked;
    ShortcutKey* shortcut;
};

namespace {
enum Sci : UINT {
    GetLength = 2006, GetCodePage = 2137, GetFirstVisibleLine = 2152,
    GetLineCount = 2154, PositionFromLine = 2167, GetMarginLeft = 2156,
    PointXFromPosition = 2164, PointYFromPosition = 2165, LinesOnScreen = 2370,
    TextHeight = 2279, DocLineFromVisible = 2221, GetLineVisible = 2228,
    GetMargins = 2253, GetMarginWidth = 2243, GetTechnology = 2631,
    GetRangePointer = 2643, GetLineEndPosition = 2136,
    TextWidth = 2276, SetExtraAscent = 2525, GetExtraAscent = 2526,
    SetRepresentation = 2665, GetRepresentation = 2666, ClearRepresentation = 2667,
    SetRepresentationAppearance = 2766
};
constexpr UINT_PTR SubclassId = 1;
constexpr unsigned ColorFont = 4; // D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT
#include "EmojiRanges.h"

constexpr int DefaultStyle = 32;
constexpr int ControlCharacterStyle = 36;
constexpr char ChineseSample[] = "\xE4\xB8\xAD"; // U+4E2D
constexpr char IdeographicSpace[] = "\xE3\x80\x80"; // U+3000
constexpr size_t MaxRepresentationBytes = 200;
NppData windows{};
struct ViewState {
    std::vector<char32_t> owned;
    std::string placeholder;
    LRESULT placeholderWidth = 0;
    LRESULT emojiSize = 0;
};
ViewState mainState;
ViewState secondState;
bool enabled = false;
ID2D1Factory* d2d = nullptr;
IDWriteFactory* dwrite = nullptr;

LRESULT sci(HWND view, Sci message, WPARAM w = 0, LPARAM l = 0) {
    return SendMessageW(view, message, w, l);
}

LRESULT textWidth(HWND view, int style, const char* text) {
    return sci(view, TextWidth, style, reinterpret_cast<LPARAM>(text));
}

char32_t readEmojiCandidate(const unsigned char* bytes, int length) {
    const auto lead = bytes[0];
    if ((length == 3 && lead != 0xE2) || (length == 4 && lead != 0xF0))
        return 0;

    char32_t codePoint = lead & (length == 3 ? 0x0F : 0x07);
    for (int i = 1; i < length; ++i) {
        if ((bytes[i] & 0xC0) != 0x80) return 0;
        codePoint = (codePoint << 6) | (bytes[i] & 0x3F);
    }
    return codePoint;
}

void paintEmoji(HWND view, HDC dc, const RECT& client, const ViewState& state) {
    const auto firstDisplay = sci(view, GetFirstVisibleLine);
    const auto first = sci(view, DocLineFromVisible, firstDisplay);
    const auto lineCount = sci(view, GetLineCount);
    const auto height = static_cast<float>(sci(view, TextHeight, 0));
    const auto size = std::min(height, static_cast<float>(state.emojiSize));
    if (first < 0 || first >= lineCount || size <= 0) return;
    const auto last = std::clamp(
        sci(view, DocLineFromVisible, firstDisplay + sci(view, LinesOnScreen)), first, lineCount - 1);

    auto left = sci(view, GetMarginLeft);
    for (LRESULT i = 0, count = sci(view, GetMargins); i < count; ++i)
        left += sci(view, GetMarginWidth, i);

    ID2D1DCRenderTarget* target = nullptr;
    IDWriteTextFormat* format = nullptr;
    ID2D1SolidColorBrush* brush = nullptr;
    const auto properties = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
        96.0f, 96.0f);

    for (auto line = first; line <= last; ++line) {
        if (!sci(view, GetLineVisible, line)) continue;
        const auto start = sci(view, PositionFromLine, line);
        const auto length = std::min<LRESULT>(sci(view, GetLineEndPosition, line) - start, 256 * 1024);
        if (length < 3) continue;
        const auto* bytes = reinterpret_cast<const unsigned char*>(sci(view, GetRangePointer, start, length));
        if (!bytes) continue;
        const auto* end = bytes + length;
        for (auto* cursor = bytes; end - cursor >= 3;) {
            const int byteCount = *cursor == 0xF0 ? 4 : 3;
            if (end - cursor < byteCount) break;
            const auto codePoint = readEmojiCandidate(cursor, byteCount);
            if (!codePoint || !std::binary_search(state.owned.begin(), state.owned.end(), codePoint)) {
                ++cursor;
                continue;
            }

            const auto position = start + (cursor - bytes);
            wchar_t emoji[2];
            const int utf16Length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                reinterpret_cast<const char*>(cursor), byteCount, emoji, 2);
            cursor += byteCount;
            if (!utf16Length) continue;
            const auto x = sci(view, PointXFromPosition, 0, position);
            const auto y = sci(view, PointYFromPosition, 0, position);
            if (x >= client.right || y >= client.bottom || y + height <= 0)
                continue;

            const auto nextX = sci(view, PointXFromPosition, 0, position + byteCount);
            const auto nextY = sci(view, PointYFromPosition, 0, position + byteCount);
            const auto width = static_cast<float>(nextY == y ? nextX - x : state.placeholderWidth);
            if (width <= 0 || x + width <= left) continue;

            if (!target) {
                if (FAILED(d2d->CreateDCRenderTarget(&properties, &target)) ||
                    FAILED(target->BindDC(dc, &client)) ||
                    FAILED(target->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black), &brush)) ||
                    FAILED(dwrite->CreateTextFormat(L"Segoe UI Emoji", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"", &format))) {
                    if (brush) brush->Release();
                    if (target) target->Release();
                    return;
                }
                format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                target->BeginDraw();
                target->PushAxisAlignedClip(D2D1::RectF(left, 0, client.right, client.bottom),
                    D2D1_ANTIALIAS_MODE_ALIASED);
            }

            IDWriteTextLayout* layout = nullptr;
            if (SUCCEEDED(dwrite->CreateTextLayout(emoji, utf16Length, format, height * 2, height, &layout))) {
                DWRITE_TEXT_METRICS metrics{};
                if (SUCCEEDED(layout->GetMetrics(&metrics)) && metrics.width > 0 && metrics.height > 0) {
                    const auto scale = std::min(std::min(width, size) / metrics.width,
                        size / metrics.height);
                    const auto leftOffset = x + (width - metrics.width * scale) / 2;
                    const auto topOffset = y + (height - metrics.height * scale) / 2;
                    target->SetTransform(D2D1::Matrix3x2F(scale, 0, 0, scale, leftOffset, topOffset));
                    target->DrawTextLayout(D2D1::Point2F(), layout, brush,
                        static_cast<D2D1_DRAW_TEXT_OPTIONS>(ColorFont));
                    target->SetTransform(D2D1::Matrix3x2F::Identity());
                }
                layout->Release();
            }
        }
    }

    if (target) {
        target->PopAxisAlignedClip();
        target->EndDraw();
        format->Release();
        brush->Release();
        target->Release();
    }
}

void encodeEmoji(char32_t codePoint, char (&key)[5]) {
    if (codePoint <= 0xFFFF) {
        key[0] = static_cast<char>(0xE0 | (codePoint >> 12));
        key[1] = static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
        key[2] = static_cast<char>(0x80 | (codePoint & 0x3F));
        key[3] = '\0';
        return;
    }
    key[0] = static_cast<char>(0xF0 | (codePoint >> 18));
    key[1] = static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F));
    key[2] = static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
    key[3] = static_cast<char>(0x80 | (codePoint & 0x3F));
    key[4] = '\0';
}

void syncRepresentations(HWND view, bool active, ViewState& state) {
    std::string placeholder;
    LRESULT width = 0;
    LRESULT size = 0;
    if (active) {
        const auto height = sci(view, TextHeight, 0);
        const auto chineseWidth = textWidth(view, DefaultStyle, ChineseSample);
        size = std::min(height, chineseWidth);
        const auto spaceWidth = textWidth(view, ControlCharacterStyle, " ");
        if (size > 0 && spaceWidth > 0) {
            placeholder.assign(std::min<size_t>(MaxRepresentationBytes,
                1 + (size - 1) / spaceWidth), ' ');
            width = textWidth(view, ControlCharacterStyle, placeholder.c_str());

            const auto wideSpaceWidth = textWidth(view, ControlCharacterStyle, IdeographicSpace);
            if (wideSpaceWidth >= size && (width < size || wideSpaceWidth < width)) {
                placeholder = IdeographicSpace;
                width = wideSpaceWidth;
            }
        }
    }
    state.placeholderWidth = width;
    state.emojiSize = size;
    if (placeholder == state.placeholder) return;

    if (state.placeholder.empty()) {
        for (const auto& range : EmojiRanges) {
            for (auto codePoint = range.first; codePoint <= range.last; ++codePoint) {
                char encoded[5];
                encodeEmoji(codePoint, encoded);
                const auto key = reinterpret_cast<WPARAM>(encoded);
                if (sci(view, GetRepresentation, key) != 0) continue;
                sci(view, SetRepresentation, key, reinterpret_cast<LPARAM>(placeholder.c_str()));
                sci(view, SetRepresentationAppearance, key, 0);
                state.owned.push_back(codePoint);
            }
        }
    } else {
        const auto remove = [&](char32_t codePoint) {
            char encoded[5];
            encodeEmoji(codePoint, encoded);
            const auto key = reinterpret_cast<WPARAM>(encoded);
            char current[MaxRepresentationBytes + 1]{};
            const auto size = sci(view, GetRepresentation, key);
            if (size != static_cast<LRESULT>(state.placeholder.size()) ||
                sci(view, GetRepresentation, key, reinterpret_cast<LPARAM>(current)) != size ||
                state.placeholder != current)
                return true;
            if (placeholder.empty()) {
                sci(view, ClearRepresentation, key);
                return true;
            }
            sci(view, SetRepresentation, key, reinterpret_cast<LPARAM>(placeholder.c_str()));
            sci(view, SetRepresentationAppearance, key, 0);
            return false;
        };
        state.owned.erase(std::remove_if(state.owned.begin(), state.owned.end(), remove), state.owned.end());
    }
    state.placeholder = std::move(placeholder);

    // Representation changes alone do not invalidate Scintilla's cached line layout.
    sci(view, SetExtraAscent, sci(view, GetExtraAscent));
}

LRESULT CALLBACK editorProc(HWND view, UINT message, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR) {
    if (message == WM_NCDESTROY)
        RemoveWindowSubclass(view, editorProc, id);
    if (message != WM_PAINT)
        return DefSubclassProc(view, message, w, l);

    auto& state = view == windows.mainView ? mainState : secondState;
    const bool active = sci(view, GetTechnology) == 0 && sci(view, GetCodePage) == CP_UTF8;
    syncRepresentations(view, active, state);
    if (!active || state.owned.empty())
        return DefSubclassProc(view, message, w, l);

    if (!GetUpdateRect(view, nullptr, FALSE))
        return DefSubclassProc(view, message, w, l);

    RECT client{};
    if (!GetClientRect(view, &client) || client.right <= 0 || client.bottom <= 0)
        return DefSubclassProc(view, message, w, l);

    HDC windowDC = GetDC(view);
    if (!windowDC)
        return DefSubclassProc(view, message, w, l);
    HDC buffer = CreateCompatibleDC(windowDC);
    HBITMAP bitmap = buffer ? CreateCompatibleBitmap(windowDC, client.right, client.bottom) : nullptr;
    ReleaseDC(view, windowDC);
    HGDIOBJ previousBitmap = bitmap ? SelectObject(buffer, bitmap) : nullptr;
    if (!previousBitmap) {
        if (bitmap) DeleteObject(bitmap);
        if (buffer) DeleteDC(buffer);
        return DefSubclassProc(view, message, w, l);
    }

    // Rebuild the whole frame because Scintilla can scroll existing screen pixels.
    // Compose the text and overlay before presenting either of them.
    RedrawWindow(view, nullptr, nullptr, RDW_INVALIDATE | RDW_NOERASE | RDW_NOCHILDREN);
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(view, &paint);
    DefSubclassProc(view, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(buffer), PRF_CLIENT);
    paintEmoji(view, buffer, client, state);
    if (dc) BitBlt(dc, 0, 0, client.right, client.bottom, buffer, 0, 0, SRCCOPY);
    EndPaint(view, &paint);

    SelectObject(buffer, previousBitmap);
    DeleteObject(bitmap);
    DeleteDC(buffer);
    return 0;
}

void disable() {
    enabled = false;
    for (HWND view : {windows.mainView, windows.secondView}) {
        if (view && RemoveWindowSubclass(view, editorProc, SubclassId)) {
            auto& state = view == windows.mainView ? mainState : secondState;
            syncRepresentations(view, false, state);
            InvalidateRect(view, nullptr, FALSE);
        }
    }
    if (d2d) d2d->Release();
    if (dwrite) dwrite->Release();
    d2d = nullptr;
    dwrite = nullptr;
}

void enable() {
    if (enabled) return;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2d)) ||
        FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(&dwrite))) ||
        !SetWindowSubclass(windows.mainView, editorProc, SubclassId, 0) ||
        !SetWindowSubclass(windows.secondView, editorProc, SubclassId, 0)) {
        disable();
        MessageBoxW(windows.npp, L"Could not start the GDI emoji overlay.", L"GDI Emoji", MB_ICONERROR);
        return;
    }
    enabled = true;
    InvalidateRect(windows.mainView, nullptr, FALSE);
    InvalidateRect(windows.secondView, nullptr, FALSE);
}

void about() {
    const auto result = ShellExecuteW(windows.npp, L"open",
        L"https://github.com/seantw/NppGdiEmoji", nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        MessageBoxW(windows.npp, L"Could not open https://github.com/seantw/NppGdiEmoji",
            L"About NppGdiEmoji", MB_ICONERROR);
    }
}

FuncItem menu[] = {{L"About NppGdiEmoji", about, 0, false, nullptr}};
}

extern "C" {
__declspec(dllexport) void setInfo(NppData data) { windows = data; }
__declspec(dllexport) const wchar_t* getName() { return L"GDI Emoji"; }
__declspec(dllexport) FuncItem* getFuncsArray(int* count) { *count = 1; return menu; }
__declspec(dllexport) void beNotified(NMHDR* notification) {
    constexpr UINT NppReady = 1001;
    constexpr UINT NppShutdown = 1009;
    if (!notification) return;
    if (notification->code == NppReady) enable();
    if (notification->code == NppShutdown) disable();
}
__declspec(dllexport) LRESULT messageProc(UINT, WPARAM, LPARAM) { return TRUE; }
__declspec(dllexport) BOOL isUnicode() { return TRUE; }
}

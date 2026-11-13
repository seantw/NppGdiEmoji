#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>
#include <d2d1.h>
#include <dwrite.h>
#include <algorithm>
#include <bitset>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
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
    GetLength = 2006, GetCodePage = 2137, SetCodePage = 2037, GetFirstVisibleLine = 2152,
    GetCurrentPos = 2008, GetAnchor = 2009, GotoPos = 2025, SetSel = 2160,
    GetCaretFore = 2138, GetCaretWidth = 2189,
    GetStyleAt = 2010, StyleGetFore = 2481,
    GetLineCount = 2154, LineFromPosition = 2166, PositionFromLine = 2167, GetMarginLeft = 2156,
    PointXFromPosition = 2164, PointYFromPosition = 2165, LinesOnScreen = 2370,
    TextHeight = 2279, DocLineFromVisible = 2221, GetLineVisible = 2228,
    GetMargins = 2253, GetMarginWidth = 2243, GetTechnology = 2631,
    GetRangePointer = 2643, GetLineEndPosition = 2136,
    TextWidth = 2276, GetWrapMode = 2269, GetDocPointer = 2357,
    SetExtraAscent = 2525, GetExtraAscent = 2526,
    PositionBefore = 2417, PositionAfter = 2418, GetSelectionMode = 2423,
    GetSelections = 2570, GetSelectionNCaretVirtualSpace = 2581, GetSelectionNAnchorVirtualSpace = 2583,
    SetRepresentation = 2665, GetRepresentation = 2666, ClearRepresentation = 2667,
    SetRepresentationAppearance = 2766, ClearAllRepresentations = 2770
};
constexpr UINT_PTR SubclassId = 1;
constexpr unsigned ColorFont = 4; // D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT
#include "EmojiRanges.h"

constexpr int DefaultStyle = 32;
constexpr int ControlCharacterStyle = 36;
constexpr char ChineseSample[] = "\xE4\xB8\xAD"; // U+4E2D
constexpr char IdeographicSpace[] = "\xE3\x80\x80"; // U+3000
constexpr char EmojiSelector[] = "\xEF\xB8\x8F"; // U+FE0F
constexpr char EnSpace[] = "\xE2\x80\x82"; // U+2002
constexpr char ZeroWidthSpace[] = "\xE2\x80\x8B"; // U+200B
constexpr char LaptopKey[] = "\xF0\x9F\x92\xBB";
constexpr char RainbowKey[] = "\xF0\x9F\x8C\x88";
constexpr char WhiteFlagKey[] = "\xF0\x9F\x8F\xB3";
constexpr size_t MaxRepresentationBytes = 200;
constexpr int MaxEmojiElements = 8;
constexpr char32_t FirstCompactComponent = 0x1F1E6;
constexpr size_t RegionalIndicatorCount = 26;
constexpr size_t SkinToneCount = 5;
constexpr size_t TagCount = 96;
constexpr size_t KeycapComponentIndex = RegionalIndicatorCount + SkinToneCount + TagCount;
constexpr size_t CompactComponentCount = KeycapComponentIndex + 1;
struct CompactSequences {
    bool laptop = false;
    bool rainbow = false;
    std::bitset<CompactComponentCount> components;
};
struct Widths {
    std::string placeholder;
    std::string selectorPlaceholder;
    LRESULT width = 0;
    LRESULT size = 0;
    LRESULT lineHeight = 0;
    bool supportsZeroWidth = false;
};
NppData windows{};
struct ViewState {
    std::vector<char32_t> owned;
    std::string placeholder;
    std::string selectorPlaceholder;
    std::string laptopPlaceholder;
    std::string rainbowPlaceholder;
    std::string whiteFlagPlaceholder;
    std::string componentPlaceholders[CompactComponentCount];
    POINT clickStart{};
    bool trackingClick = false;
    LRESULT document = 0;
    LRESULT documentLength = -1;
    bool scanDirty = true;
    bool representationsDirty = true;
    bool syncingRepresentations = false;
    CompactSequences compact;
    HDC buffer = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ previousBitmap = nullptr;
    LONG bufferWidth = 0;
    LONG bufferHeight = 0;
    IDWriteTextFormat* format = nullptr;
    IDWriteTextFormat* symbolFormat = nullptr;
    float formatSize = 0;
    float layoutHeight = 0;
    std::unordered_map<std::wstring, IDWriteTextLayout*> layouts;
};
ViewState mainState;
ViewState secondState;
bool enabled = false;
ID2D1Factory* d2d = nullptr;
IDWriteFactory* dwrite = nullptr;
IDWriteFont* symbolFont = nullptr;

LRESULT sci(HWND view, Sci message, WPARAM w = 0, LPARAM l = 0) {
    return SendMessageW(view, message, w, l);
}

LRESULT textWidth(HWND view, int style, const char* text) {
    return sci(view, TextWidth, style, reinterpret_cast<LPARAM>(text));
}

int utf8Length(unsigned char lead) {
    if (lead >= 0xC2 && lead <= 0xDF) return 2;
    if (lead >= 0xE0 && lead <= 0xEF) return 3;
    if (lead >= 0xF0 && lead <= 0xF4) return 4;
    return 1;
}

char32_t readEmojiCandidate(const unsigned char* bytes, int length) {
    char32_t codePoint = bytes[0] & (0x7F >> length);
    for (int i = 1; i < length; ++i) {
        if ((bytes[i] & 0xC0) != 0x80) return 0;
        codePoint = (codePoint << 6) | (bytes[i] & 0x3F);
    }
    if ((length == 3 && codePoint < 0x800) || (length == 4 && codePoint < 0x10000) ||
        (codePoint >= 0xD800 && codePoint <= 0xDFFF) || codePoint > 0x10FFFF)
        return 0;
    return codePoint;
}

template<size_t count>
bool inEmojiRanges(char32_t codePoint, const EmojiRange (&ranges)[count]) {
    const auto range = std::lower_bound(ranges, ranges + count, codePoint,
        [](const EmojiRange& candidate, char32_t value) { return candidate.last < value; });
    return range != ranges + count && codePoint >= range->first;
}

bool isRegionalIndicator(char32_t codePoint) {
    return codePoint >= 0x1F1E6 && codePoint <= 0x1F1FF;
}

char32_t decode(const unsigned char*& cursor, const unsigned char* end) {
    if (cursor == end) return 0;
    const int length = utf8Length(*cursor);
    if (length == 1) {
        const auto byte = *cursor++;
        return byte < 0x80 ? byte : 0;
    }
    if (end - cursor < length) {
        cursor = end;
        return 0;
    }
    const auto value = readEmojiCandidate(cursor, length);
    cursor += value ? length : 1;
    return value;
}

bool consume(const unsigned char*& cursor, const unsigned char* end, char32_t codePoint) {
    auto next = cursor;
    if (decode(next, end) != codePoint) return false;
    cursor = next;
    return true;
}

bool isSkinTone(char32_t value) {
    return value >= 0x1F3FB && value <= 0x1F3FF;
}

const unsigned char* emojiElementEnd(char32_t base, const unsigned char* cursor,
    const unsigned char* end) {
    const bool emoji = inEmojiRanges(base, EmojiRanges);
    const bool variation = inEmojiRanges(base, EmojiVariationBases) &&
        consume(cursor, end, 0xFE0F);
    if (inEmojiRanges(base, EmojiModifierBases)) {
        auto next = cursor;
        if (isSkinTone(decode(next, end))) return next;
    }
    return emoji || variation ? cursor : nullptr;
}

const unsigned char* emojiSequenceEnd(char32_t base, const unsigned char* afterBase,
    const unsigned char* end) {
    auto cursor = afterBase;
    if (base == '#' || base == '*' || (base >= '0' && base <= '9')) {
        consume(cursor, end, 0xFE0F);
        return consume(cursor, end, 0x20E3) ? cursor : afterBase;
    }
    if (base < 0x80) return afterBase;
    if (isRegionalIndicator(base))
        return isRegionalIndicator(decode(cursor, end)) ? cursor : afterBase;
    if (base == 0x1F3F4) {
        for (const auto* tag : {U"\U000E0067\U000E0062\U000E0065\U000E006E\U000E0067",
                                U"\U000E0067\U000E0062\U000E0073\U000E0063\U000E0074",
                                U"\U000E0067\U000E0062\U000E0077\U000E006C\U000E0073"}) {
            cursor = afterBase;
            while (*tag && consume(cursor, end, *tag)) ++tag;
            if (!*tag && consume(cursor, end, 0xE007F)) return cursor;
        }
    }
    cursor = emojiElementEnd(base, afterBase, end);
    if (!cursor) return afterBase;
    // Cap the lookahead and UTF-16 buffer for deliberately long joiner chains.
    for (int elements = 1; elements < MaxEmojiElements; ++elements) {
        auto next = cursor;
        if (!consume(next, end, 0x200D)) break;
        const auto joinedBase = decode(next, end);
        next = emojiElementEnd(joinedBase, next, end);
        if (!next) break;
        cursor = next;
    }
    return cursor;
}

size_t compactComponentIndex(char32_t value) {
    if (value == 0x20E3) return KeycapComponentIndex;
    if (isRegionalIndicator(value)) return value - FirstCompactComponent;
    if (isSkinTone(value)) return RegionalIndicatorCount + value - 0x1F3FB;
    if (value >= 0xE0020 && value <= 0xE007F)
        return RegionalIndicatorCount + SkinToneCount + value - 0xE0020;
    return CompactComponentCount;
}

CompactSequences compactSequencesInDocument(HWND view) {
    const auto length = sci(view, GetLength);
    // Width substitutions are view-wide. Only use them after checking the entire document.
    if (length <= 0 || length > 256 * 1024) return {};
    const auto* bytes = reinterpret_cast<const unsigned char*>(sci(view, GetRangePointer, 0, length));
    if (!bytes) return {};

    constexpr char rainbowSequence[] = "\xF0\x9F\x8F\xB3\xEF\xB8\x8F\xE2\x80\x8D\xF0\x9F\x8C\x88";
    CompactSequences found;
    CompactSequences conflict;
    const auto* end = bytes + length;
    for (auto* cursor = bytes; cursor < end;) {
        const auto* first = cursor;
        const auto base = decode(cursor, end);
        const auto* afterBase = cursor;
        cursor = emojiSequenceEnd(base, cursor, end);
        if (base < 0x80 && cursor == afterBase) continue;
        const auto count = cursor - first;
        bool laptopSequence = false;
        if (base == 0x1F469) {
            auto suffix = afterBase;
            const auto* afterPerson = emojiElementEnd(base, suffix, cursor);
            if (afterPerson) {
                suffix = afterPerson;
                laptopSequence = consume(suffix, cursor, 0x200D) &&
                    consume(suffix, cursor, 0x1F4BB) && suffix == cursor;
            }
        }
        found.laptop |= laptopSequence;
        if (count == sizeof rainbowSequence - 1 && std::memcmp(first, rainbowSequence, count) == 0) {
            found.rainbow = true;
        } else {
            for (auto* part = first; part < cursor;) {
                const bool leading = part == first;
                const auto value = decode(part, cursor);
                if (value == 0x1F4BB && !laptopSequence) conflict.laptop = true;
                if (value == 0x1F3F3 || value == 0x1F308) conflict.rainbow = true;
                const auto index = compactComponentIndex(value);
                if (index == CompactComponentCount) continue;
                // The sequence parser only consumes skin tones and tags in valid positions.
                const bool inSequence = cursor != afterBase && (!leading || isRegionalIndicator(base)) &&
                    (value != 0x20E3 || cursor - afterBase == 6); // Keep the width of keycaps without VS16.
                (inSequence ? found.components : conflict.components).set(index);
            }
        }
    }
    // Keep both halves of every country flag at their normal width if any indicator is standalone.
    for (size_t i = 0; i < RegionalIndicatorCount; ++i) {
        if (!conflict.components.test(i)) continue;
        for (size_t j = 0; j < RegionalIndicatorCount; ++j) found.components.reset(j);
        break;
    }
    found.laptop = found.laptop && !conflict.laptop;
    found.rainbow = found.rainbow && !conflict.rainbow;
    found.components &= ~conflict.components;
    return found;
}

bool sequencePart(char32_t value) {
    return value == 0xFE0F || value == 0x20E3 || value == 0x200D ||
        isSkinTone(value) || isRegionalIndicator(value) ||
        (value >= 0xE0020 && value <= 0xE007F);
}

char32_t codePointAt(HWND view, LRESULT start, LRESULT end) {
    const auto length = end - start;
    if (length < 2 || length > 4) return 0;
    const auto* bytes = reinterpret_cast<const unsigned char*>(sci(view, GetRangePointer, start, length));
    return bytes ? decode(bytes, bytes + length) : 0;
}

struct SequenceBounds {
    LRESULT start = 0;
    LRESULT end = 0;
};

SequenceBounds emojiSequenceAt(HWND view, LRESULT position) {
    const auto before = sci(view, PositionBefore, position);
    const auto after = sci(view, PositionAfter, position);
    if (!sequencePart(codePointAt(view, position, after)) &&
        !sequencePart(codePointAt(view, before, position))) return {};

    const auto line = sci(view, LineFromPosition, position);
    const auto start = sci(view, PositionFromLine, line);
    const auto length = std::min<LRESULT>(sci(view, GetLineEndPosition, line) - start, 256 * 1024);
    if (position <= start || position - start >= length) return {};
    const auto* bytes = reinterpret_cast<const unsigned char*>(sci(view, GetRangePointer, start, length));
    if (!bytes) return {};
    const auto* end = bytes + length;
    for (auto* cursor = bytes; cursor < bytes + position - start;) {
        const auto* first = cursor;
        const auto base = decode(cursor, end);
        cursor = emojiSequenceEnd(base, cursor, end);
        if (cursor > bytes + position - start)
            return {start + (first - bytes), start + (cursor - bytes)};
    }
    return {};
}

void snapClickedCaret(HWND view, POINT click) {
    if (sci(view, GetTechnology) != 0 || sci(view, GetCodePage) != CP_UTF8 ||
        sci(view, GetSelections) != 1 || sci(view, GetSelectionMode) != 0 ||
        sci(view, GetSelectionNCaretVirtualSpace, 0) != 0)
        return;

    const auto position = sci(view, GetCurrentPos);
    if (position != sci(view, GetAnchor)) return;
    const auto [start, end] = emojiSequenceAt(view, position);
    if (!end) return;
    const auto y = sci(view, PointYFromPosition, 0, start);
    if (sci(view, PointYFromPosition, 0, end) != y || click.y < y ||
        click.y - y >= sci(view, TextHeight, 0))
        return;

    const auto left = sci(view, PointXFromPosition, 0, start);
    const auto right = sci(view, PointXFromPosition, 0, end);
    if (left < right && click.x >= left && click.x <= right)
        sci(view, GotoPos, click.x - left < right - click.x ? start : end);
}

void snapSelectedEmoji(HWND view, LRESULT previousCaret = -1, bool keyboard = false) {
    if (sci(view, GetTechnology) != 0 || sci(view, GetCodePage) != CP_UTF8 ||
        sci(view, GetSelections) != 1 || sci(view, GetSelectionMode) != 0 ||
        sci(view, GetSelectionNCaretVirtualSpace, 0) != 0 ||
        sci(view, GetSelectionNAnchorVirtualSpace, 0) != 0)
        return;

    const auto caret = sci(view, GetCurrentPos);
    const auto anchor = sci(view, GetAnchor);
    if (caret == anchor || (keyboard && caret == previousCaret)) return;

    const bool forward = caret > anchor;
    const auto anchorEmoji = emojiSequenceAt(view, anchor);
    const auto caretEmoji = emojiSequenceAt(view, caret);
    const auto newAnchor = anchorEmoji.end ? (forward ? anchorEmoji.start : anchorEmoji.end) : anchor;
    // When shortening a keyboard selection, cross the entire sequence in the direction of travel.
    const bool moveRight = keyboard ? caret > previousCaret : forward;
    const auto newCaret = caretEmoji.end ? (moveRight ? caretEmoji.end : caretEmoji.start) : caret;
    if (newAnchor != anchor || newCaret != caret)
        sci(view, SetSel, newAnchor, newCaret);
}

COLORREF uniformCellBackground(HDC dc, int left, int top, int right, int bottom) {
    if (left >= right) return CLR_INVALID;
    const auto color = GetPixel(dc, left, top);
    if (color == CLR_INVALID) return color;
    for (int x = left; x < right; ++x) {
        if (GetPixel(dc, x, top) != color || GetPixel(dc, x, bottom - 1) != color)
            return CLR_INVALID;
    }
    return color;
}

void releaseLayouts(ViewState& state) {
    for (auto& entry : state.layouts) entry.second->Release();
    state.layouts.clear();
}

void paintEmoji(HWND view, HDC dc, const RECT& client, ViewState& state, const Widths& widths,
    bool nativeLayout) {
    const auto firstDisplay = sci(view, GetFirstVisibleLine);
    const auto first = sci(view, DocLineFromVisible, firstDisplay);
    const auto lineCount = sci(view, GetLineCount);
    const auto height = static_cast<float>(widths.lineHeight);
    const auto size = static_cast<float>(widths.size);
    if (first < 0 || first >= lineCount || size <= 0) return;
    const auto last = std::clamp(
        sci(view, DocLineFromVisible, firstDisplay + sci(view, LinesOnScreen)), first, lineCount - 1);

    auto left = sci(view, GetMarginLeft);
    for (LRESULT i = 0, count = sci(view, GetMargins); i < count; ++i)
        left += sci(view, GetMarginWidth, i);

    const auto caretPos = sci(view, GetCurrentPos);
    const auto caretWidth = sci(view, GetCaretWidth);
    const auto caretColor = static_cast<COLORREF>(sci(view, GetCaretFore));

    ID2D1DCRenderTarget* target = nullptr;
    ID2D1SolidColorBrush* brush = nullptr;
    const auto properties = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
        96.0f, 96.0f);

    for (auto line = first; line <= last; ++line) {
        if (!sci(view, GetLineVisible, line)) continue;
        const auto start = sci(view, PositionFromLine, line);
        const auto length = std::min<LRESULT>(sci(view, GetLineEndPosition, line) - start, 256 * 1024);
        if (length < 2) continue;
        const auto* bytes = reinterpret_cast<const unsigned char*>(sci(view, GetRangePointer, start, length));
        if (!bytes) continue;
        const auto* end = bytes + length;
        for (auto* cursor = bytes; cursor < end;) {
            const auto* first = cursor;
            const auto codePoint = decode(cursor, end);
            const auto* afterBase = cursor;
            cursor = emojiSequenceEnd(codePoint, afterBase, end);
            const bool sequence = cursor != afterBase;
            const bool textSymbol = !sequence && inEmojiRanges(codePoint, EmojiVariationBases) &&
                !inEmojiRanges(codePoint, EmojiRanges);
            const bool owned = std::binary_search(state.owned.begin(), state.owned.end(), codePoint) ||
                (codePoint == 0x1F4BB && !state.laptopPlaceholder.empty()) ||
                (codePoint == 0x1F308 && !state.rainbowPlaceholder.empty());
            if (!owned && !sequence) {
                if (!nativeLayout) continue;
                if (textSymbol) {
                    BOOL supported = FALSE;
                    if (!symbolFont || FAILED(symbolFont->HasCharacter(codePoint, &supported)) || !supported)
                        continue;
                } else if (!inEmojiRanges(codePoint, EmojiRanges)) {
                    continue;
                }
            }

            const int byteCount = static_cast<int>(cursor - first);
            const auto position = start + (first - bytes);
            wchar_t emoji[MaxEmojiElements * 6 - 1]; // Up to five UTF-16 units per element, plus joiners.
            const int utf16Length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                reinterpret_cast<const char*>(first), byteCount, emoji, ARRAYSIZE(emoji));
            if (!utf16Length) continue;
            const auto x = sci(view, PointXFromPosition, 0, position);
            const auto y = sci(view, PointYFromPosition, 0, position);
            if (x >= client.right || y >= client.bottom || y + height <= 0)
                continue;

            const auto nextX = sci(view, PointXFromPosition, 0, position + byteCount);
            const auto nextY = sci(view, PointYFromPosition, 0, position + byteCount);
            const auto width = static_cast<float>(nextY == y ? nextX - x : widths.width);
            if (width <= 0 || x + width <= left) continue;

            const bool replaceText = sequence || !owned;
            D2D1_RECT_F cell{};
            COLORREF background = CLR_INVALID;
            if (replaceText) {
                // The editor already painted this sequence. Cover its glyphs
                // only when the sequence fits on one row and has a uniform background.
                if (nextY != y || y < 0 || y + height > client.bottom || height < 3)
                    continue;
                auto cellLeft = std::max(x, left);
                auto cellRight = std::min<LRESULT>(nextX, client.right);
                const auto isCaretColumn = [&](LRESULT column) {
                    return GetPixel(dc, column, y) == caretColor &&
                        GetPixel(dc, column, y + height - 1) == caretColor;
                };
                // A line caret at either sequence boundary can overlap this cell.
                // Leave its pixels in the Scintilla frame and sample the rest.
                if (caretPos == position) {
                    for (LRESULT i = 0; i < caretWidth && cellLeft < cellRight &&
                        isCaretColumn(cellLeft); ++i)
                        ++cellLeft;
                }
                if (caretPos == position + byteCount && nextX <= client.right &&
                    cellLeft < cellRight && isCaretColumn(cellRight - 1))
                    --cellRight;
                background = uniformCellBackground(dc, cellLeft, y, cellRight, y + height);
                if (background == CLR_INVALID) continue;
                cell = D2D1::RectF(cellLeft, y, cellRight, y + height);
            }

            if (!target) {
                if (!state.format || state.formatSize != size || state.layoutHeight != height) {
                    releaseLayouts(state);
                    if (state.format) state.format->Release();
                    state.format = nullptr;
                    if (state.symbolFormat) state.symbolFormat->Release();
                    state.symbolFormat = nullptr;
                    if (FAILED(dwrite->CreateTextFormat(L"Segoe UI Emoji", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"", &state.format)))
                        return;
                    state.format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                    state.formatSize = size;
                    state.layoutHeight = height;
                }
                if (FAILED(d2d->CreateDCRenderTarget(&properties, &target)) ||
                    FAILED(target->BindDC(dc, &client)) ||
                    FAILED(target->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black), &brush))) {
                    if (brush) brush->Release();
                    if (target) target->Release();
                    return;
                }
                target->BeginDraw();
                target->PushAxisAlignedClip(D2D1::RectF(left, 0, client.right, client.bottom),
                    D2D1_ANTIALIAS_MODE_ALIASED);
            }

            if (textSymbol && !state.symbolFormat) {
                if (FAILED(dwrite->CreateTextFormat(L"Segoe UI Symbol", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                    DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"", &state.symbolFormat)))
                    continue;
                state.symbolFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            }
            std::wstring key(emoji, utf16Length);
            auto cached = state.layouts.find(key);
            if (cached == state.layouts.end()) {
                IDWriteTextLayout* layout = nullptr;
                auto* format = textSymbol ? state.symbolFormat : state.format;
                if (FAILED(dwrite->CreateTextLayout(emoji, utf16Length, format, height * 2, height, &layout)))
                    continue;
                if (state.layouts.size() >= 128) releaseLayouts(state);
                cached = state.layouts.emplace(std::move(key), layout).first;
            }
            {
                auto* layout = cached->second;
                DWRITE_TEXT_METRICS metrics{};
                if (SUCCEEDED(layout->GetMetrics(&metrics)) && metrics.width > 0 && metrics.height > 0) {
                    float inkLeft = 0;
                    float inkTop = 0;
                    float inkWidth = metrics.width;
                    float inkHeight = metrics.height;
                    DWRITE_OVERHANG_METRICS overhang{};
                    if (SUCCEEDED(layout->GetOverhangMetrics(&overhang))) {
                        const float visibleWidth = metrics.layoutWidth + overhang.right + overhang.left;
                        const float visibleHeight = metrics.layoutHeight + overhang.bottom + overhang.top;
                        if (visibleWidth > 0 && visibleHeight > 0) {
                            inkLeft = -overhang.left;
                            inkTop = -overhang.top;
                            inkWidth = visibleWidth;
                            inkHeight = visibleHeight;
                        }
                    }
                    const auto scale = std::min(std::min(width, size) / inkWidth, size / inkHeight);
                    const auto leftOffset = x + (width - inkWidth * scale) / 2 - inkLeft * scale;
                    const auto topOffset = y + (height - inkHeight * scale) / 2 - inkTop * scale;
                    if (replaceText) {
                        target->PushAxisAlignedClip(cell, D2D1_ANTIALIAS_MODE_ALIASED);
                        brush->SetColor(D2D1::ColorF(GetRValue(background) / 255.0f,
                            GetGValue(background) / 255.0f, GetBValue(background) / 255.0f));
                        target->FillRectangle(cell, brush);
                    }
                    const auto foreground = textSymbol ?
                        static_cast<COLORREF>(sci(view, StyleGetFore, sci(view, GetStyleAt, position))) : RGB(0, 0, 0);
                    brush->SetColor(D2D1::ColorF(GetRValue(foreground) / 255.0f,
                        GetGValue(foreground) / 255.0f, GetBValue(foreground) / 255.0f));
                    target->SetTransform(D2D1::Matrix3x2F(scale, 0, 0, scale, leftOffset, topOffset));
                    target->DrawTextLayout(D2D1::Point2F(), layout, brush,
                        textSymbol ? D2D1_DRAW_TEXT_OPTIONS_NONE : static_cast<D2D1_DRAW_TEXT_OPTIONS>(ColorFont));
                    target->SetTransform(D2D1::Matrix3x2F::Identity());
                    if (replaceText) target->PopAxisAlignedClip();
                }
            }
        }
    }

    if (target) {
        target->PopAxisAlignedClip();
        target->EndDraw();
        brush->Release();
        target->Release();
    }
}

void encodeEmoji(char32_t codePoint, char (&key)[5]) {
    if (codePoint < 0x800) {
        key[0] = static_cast<char>(0xC0 | (codePoint >> 6));
        key[1] = static_cast<char>(0x80 | (codePoint & 0x3F));
        key[2] = '\0';
        return;
    }
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

bool syncSpecialRepresentation(HWND view, const char* character, const std::string& placeholder,
    std::string& ownedPlaceholder) {
    const auto key = reinterpret_cast<WPARAM>(character);
    const auto length = sci(view, GetRepresentation, key);
    if (length != 0) {
        char current[MaxRepresentationBytes + 1]{};
        if (ownedPlaceholder.empty() ||
            length != static_cast<LRESULT>(ownedPlaceholder.size()) ||
            sci(view, GetRepresentation, key, reinterpret_cast<LPARAM>(current)) != length ||
            ownedPlaceholder != current) {
            ownedPlaceholder.clear();
            return false;
        }
        if (placeholder == ownedPlaceholder) return false;
    }

    ownedPlaceholder = placeholder;
    if (placeholder.empty()) {
        if (length == 0) return false;
        sci(view, ClearRepresentation, key);
    } else {
        sci(view, SetRepresentation, key, reinterpret_cast<LPARAM>(placeholder.c_str()));
        sci(view, SetRepresentationAppearance, key, 0);
    }
    return true;
}

Widths measureWidths(HWND view) {
    Widths result;
    auto& [placeholder, selectorPlaceholder, width, size, height, supportsZeroWidth] = result;
    height = sci(view, TextHeight, 0);
    const auto chineseWidth = textWidth(view, DefaultStyle, ChineseSample);
    size = std::min(height, chineseWidth);
    const auto spaceWidth = textWidth(view, ControlCharacterStyle, " ");
    if (size > 0 && spaceWidth > 0) {
        const auto halfWidth = (chineseWidth + 1) / 2;
        const auto spaces = std::clamp<LRESULT>((halfWidth + spaceWidth / 2) / spaceWidth,
            1, MaxRepresentationBytes);
        selectorPlaceholder.assign(spaces, ' ');
        const auto enWidth = textWidth(view, ControlCharacterStyle, EnSpace);
        const auto asciiWidth = textWidth(view, ControlCharacterStyle, selectorPlaceholder.c_str());
        if (enWidth > 0 && std::abs(enWidth - halfWidth) < std::abs(asciiWidth - halfWidth))
            selectorPlaceholder = EnSpace;

        placeholder.assign(std::min<size_t>(MaxRepresentationBytes,
            1 + (size - 1) / spaceWidth), ' ');
        width = placeholder.size() == static_cast<size_t>(spaces) ? asciiWidth :
            textWidth(view, ControlCharacterStyle, placeholder.c_str());

        const auto wideSpaceWidth = textWidth(view, ControlCharacterStyle, IdeographicSpace);
        if (wideSpaceWidth >= size && (width < size || wideSpaceWidth < width)) {
            placeholder = IdeographicSpace;
            width = wideSpaceWidth;
        }
        supportsZeroWidth = textWidth(view, ControlCharacterStyle, ZeroWidthSpace) == 0;
    }
    return result;
}

void syncRepresentations(HWND view, const Widths& widths, ViewState& state) {
    const auto& placeholder = widths.placeholder;
    const auto& selectorPlaceholder = widths.selectorPlaceholder;
    auto compact = CompactSequences{};
    if (!placeholder.empty() && widths.supportsZeroWidth) {
        if (state.scanDirty) {
            state.compact = compactSequencesInDocument(view);
            state.scanDirty = false;
        }
        compact = state.compact;
    }
    bool specialChanged = syncSpecialRepresentation(view, EmojiSelector, selectorPlaceholder,
        state.selectorPlaceholder);
    specialChanged |= syncSpecialRepresentation(view, LaptopKey,
        compact.laptop ? ZeroWidthSpace : placeholder, state.laptopPlaceholder);
    // Keep the existing half-width VS16 and use the other half for the white flag.
    const bool rainbowAvailable = !state.rainbowPlaceholder.empty() ||
        sci(view, GetRepresentation, reinterpret_cast<WPARAM>(RainbowKey)) == 0;
    specialChanged |= syncSpecialRepresentation(view, WhiteFlagKey,
        compact.rainbow && rainbowAvailable && !state.selectorPlaceholder.empty() ? selectorPlaceholder : "",
        state.whiteFlagPlaceholder);
    specialChanged |= syncSpecialRepresentation(view, RainbowKey,
        !state.whiteFlagPlaceholder.empty() ? ZeroWidthSpace : placeholder, state.rainbowPlaceholder);
    if (state.rainbowPlaceholder.empty() && !state.whiteFlagPlaceholder.empty())
        specialChanged |= syncSpecialRepresentation(view, WhiteFlagKey, "", state.whiteFlagPlaceholder);
    for (size_t i = 0; i < CompactComponentCount; ++i) {
        const bool compactComponent = compact.components.test(i);
        auto& ownedPlaceholder = state.componentPlaceholders[i];
        if (!compactComponent && ownedPlaceholder.empty()) continue;
        const auto codePoint = i == KeycapComponentIndex ? 0x20E3 :
            i < RegionalIndicatorCount ? FirstCompactComponent + i :
            i < RegionalIndicatorCount + SkinToneCount ? 0x1F3FB + i - RegionalIndicatorCount :
            0xE0020 + i - RegionalIndicatorCount - SkinToneCount;
        char key[5];
        encodeEmoji(static_cast<char32_t>(codePoint), key);
        const char* representation = "";
        if (compactComponent)
            representation = i < RegionalIndicatorCount ? selectorPlaceholder.c_str() : ZeroWidthSpace;
        specialChanged |= syncSpecialRepresentation(view, key, representation, ownedPlaceholder);
    }
    if (placeholder == state.placeholder && !state.representationsDirty) {
        if (specialChanged) sci(view, SetExtraAscent, sci(view, GetExtraAscent));
        return;
    }

    state.representationsDirty = false;
    bool regularChanged = false;
    if (state.placeholder.empty() && !placeholder.empty()) {
        const auto registerRanges = [&](const auto& ranges, bool textSymbols) {
        for (const auto& range : ranges) {
            for (auto codePoint = range.first; codePoint <= range.last; ++codePoint) {
                if (codePoint == 0x1F4BB || codePoint == 0x1F308) continue;
                if (textSymbols) {
                    BOOL supported = FALSE;
                    if (inEmojiRanges(codePoint, EmojiRanges) || !symbolFont ||
                        FAILED(symbolFont->HasCharacter(codePoint, &supported)) || !supported)
                        continue;
                }
                char encoded[5];
                encodeEmoji(codePoint, encoded);
                const auto key = reinterpret_cast<WPARAM>(encoded);
                if (sci(view, GetRepresentation, key) != 0) continue;
                sci(view, SetRepresentation, key, reinterpret_cast<LPARAM>(placeholder.c_str()));
                sci(view, SetRepresentationAppearance, key, 0);
                state.owned.push_back(codePoint);
                regularChanged = true;
            }
        }
        };
        registerRanges(EmojiRanges, false);
        registerRanges(EmojiVariationBases, true);
        std::sort(state.owned.begin(), state.owned.end());
    } else {
        const auto remove = [&](char32_t codePoint) {
            char encoded[5];
            encodeEmoji(codePoint, encoded);
            const auto key = reinterpret_cast<WPARAM>(encoded);
            char current[MaxRepresentationBytes + 1]{};
            const auto length = sci(view, GetRepresentation, key);
            if (length != 0 && (length != static_cast<LRESULT>(state.placeholder.size()) ||
                sci(view, GetRepresentation, key, reinterpret_cast<LPARAM>(current)) != length ||
                state.placeholder != current))
                return true;
            if (placeholder.empty()) {
                if (length != 0) {
                    sci(view, ClearRepresentation, key);
                    regularChanged = true;
                }
                return true;
            }
            if (length != 0 && placeholder == state.placeholder) return false;
            sci(view, SetRepresentation, key, reinterpret_cast<LPARAM>(placeholder.c_str()));
            sci(view, SetRepresentationAppearance, key, 0);
            regularChanged = true;
            return false;
        };
        state.owned.erase(std::remove_if(state.owned.begin(), state.owned.end(), remove), state.owned.end());
    }
    state.placeholder = placeholder;

    // Representation changes alone do not invalidate Scintilla's cached line layout.
    if (regularChanged || specialChanged) sci(view, SetExtraAscent, sci(view, GetExtraAscent));
}

void releaseBuffer(ViewState& state) {
    if (!state.buffer) return;
    SelectObject(state.buffer, state.previousBitmap);
    DeleteObject(state.bitmap);
    DeleteDC(state.buffer);
    state.buffer = nullptr;
}

HDC frameBuffer(HWND view, const RECT& client, ViewState& state) {
    if (state.buffer && state.bufferWidth == client.right && state.bufferHeight == client.bottom)
        return state.buffer;
    releaseBuffer(state);
    const auto dc = GetDC(view);
    if (!dc) return nullptr;
    const auto buffer = CreateCompatibleDC(dc);
    const auto bitmap = buffer ? CreateCompatibleBitmap(dc, client.right, client.bottom) : nullptr;
    ReleaseDC(view, dc);
    const auto previous = bitmap ? SelectObject(buffer, bitmap) : nullptr;
    if (!previous) {
        if (bitmap) DeleteObject(bitmap);
        if (buffer) DeleteDC(buffer);
        return nullptr;
    }
    state.buffer = buffer;
    state.bitmap = bitmap;
    state.previousBitmap = previous;
    state.bufferWidth = client.right;
    state.bufferHeight = client.bottom;
    return buffer;
}

void releaseGraphics(ViewState& state) {
    releaseBuffer(state);
    releaseLayouts(state);
    if (state.format) state.format->Release();
    state.format = nullptr;
    if (state.symbolFormat) state.symbolFormat->Release();
    state.symbolFormat = nullptr;
}

LRESULT CALLBACK editorProc(HWND view, UINT message, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR) {
    auto& state = view == windows.mainView ? mainState : secondState;
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(view, editorProc, id);
        releaseGraphics(state);
    }
    if (!state.syncingRepresentations && (message == ClearAllRepresentations ||
        message == ClearRepresentation || message == SetRepresentation ||
        message == SetRepresentationAppearance || message == SetCodePage))
        state.representationsDirty = true;
    if (message == WM_LBUTTONDOWN) {
        state.clickStart = {GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        state.trackingClick = !(w & (MK_SHIFT | MK_CONTROL)) && !(GetKeyState(VK_MENU) & 0x8000);
    } else if (message == WM_LBUTTONDBLCLK || message == WM_CAPTURECHANGED ||
        message == WM_CANCELMODE) {
        state.trackingClick = false;
    } else if ((state.trackingClick && message == WM_MOUSEMOVE) || message == WM_LBUTTONUP) {
        const POINT point{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        if (std::abs(point.x - state.clickStart.x) >= GetSystemMetrics(SM_CXDRAG) ||
            std::abs(point.y - state.clickStart.y) >= GetSystemMetrics(SM_CYDRAG))
            state.trackingClick = false;
        if (message == WM_LBUTTONUP) {
            const bool click = state.trackingClick && !(w & (MK_SHIFT | MK_CONTROL)) &&
                !(GetKeyState(VK_MENU) & 0x8000);
            state.trackingClick = false;
            const auto result = DefSubclassProc(view, message, w, l);
            snapSelectedEmoji(view);
            if (click) snapClickedCaret(view, point);
            return result;
        }
    }
    if (message == WM_KEYDOWN && (GetKeyState(VK_SHIFT) & 0x8000) &&
        !(GetKeyState(VK_MENU) & 0x8000) &&
        (w == VK_LEFT || w == VK_RIGHT || w == VK_UP || w == VK_DOWN ||
         w == VK_HOME || w == VK_END || w == VK_PRIOR || w == VK_NEXT)) {
        const auto previousCaret = sci(view, GetCurrentPos);
        const auto result = DefSubclassProc(view, message, w, l);
        snapSelectedEmoji(view, previousCaret, true);
        return result;
    }
    if (message == WM_KEYDOWN && (w == VK_LEFT || w == VK_RIGHT) &&
        !(GetKeyState(VK_SHIFT) & 0x8000) && !(GetKeyState(VK_CONTROL) & 0x8000) &&
        !(GetKeyState(VK_MENU) & 0x8000) && sci(view, GetTechnology) == 0 &&
        sci(view, GetCodePage) == CP_UTF8 && sci(view, GetSelections) == 1 &&
        sci(view, GetSelectionMode) == 0 && sci(view, GetSelectionNCaretVirtualSpace, 0) == 0) {
        const auto position = sci(view, GetCurrentPos);
        if (position == sci(view, GetAnchor)) {
            const auto step = w == VK_LEFT ? PositionBefore : PositionAfter;
            const auto next = sci(view, step, position);
            const auto [start, end] = emojiSequenceAt(view, next);
            if (next != position && end) {
                const auto result = DefSubclassProc(view, message, w, l);
                if (sci(view, GetCurrentPos) == next && sci(view, GetAnchor) == next &&
                    sci(view, GetSelections) == 1)
                    sci(view, GotoPos, w == VK_LEFT ? start : end);
                return result;
            }
        }
    }
    if (message != WM_PAINT)
        return DefSubclassProc(view, message, w, l);

    const bool active = sci(view, GetTechnology) == 0 && sci(view, GetCodePage) == CP_UTF8;
    const auto document = sci(view, GetDocPointer);
    const auto documentLength = sci(view, GetLength);
    if (document != state.document) {
        state.document = document;
        state.scanDirty = true;
        state.representationsDirty = true;
    }
    if (documentLength != state.documentLength) {
        state.documentLength = documentLength;
        state.scanDirty = true;
    }
    const auto widths = active ? measureWidths(view) : Widths{};
    const bool wrap = active && sci(view, GetWrapMode) != 0;
    // Wrapped sequences can straddle two rows. Keep native glyphs available as fallback.
    state.syncingRepresentations = true;
    syncRepresentations(view, wrap ? Widths{} : widths, state);
    state.syncingRepresentations = false;
    if (!active || widths.placeholder.empty())
        return DefSubclassProc(view, message, w, l);

    if (!GetUpdateRect(view, nullptr, FALSE))
        return DefSubclassProc(view, message, w, l);

    RECT client{};
    if (!GetClientRect(view, &client) || client.right <= 0 || client.bottom <= 0)
        return DefSubclassProc(view, message, w, l);

    const auto buffer = frameBuffer(view, client, state);
    if (!buffer)
        return DefSubclassProc(view, message, w, l);

    // Rebuild the whole frame because Scintilla can scroll existing screen pixels.
    // Compose the text and overlay before presenting either of them.
    RedrawWindow(view, nullptr, nullptr, RDW_INVALIDATE | RDW_NOERASE | RDW_NOCHILDREN);
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(view, &paint);
    DefSubclassProc(view, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(buffer), PRF_CLIENT);
    paintEmoji(view, buffer, client, state, widths, wrap);
    if (dc) BitBlt(dc, 0, 0, client.right, client.bottom, buffer, 0, 0, SRCCOPY);
    EndPaint(view, &paint);

    return 0;
}

void disable() {
    enabled = false;
    for (HWND view : {windows.mainView, windows.secondView}) {
        if (view && RemoveWindowSubclass(view, editorProc, SubclassId)) {
            auto& state = view == windows.mainView ? mainState : secondState;
            syncRepresentations(view, Widths{}, state);
            InvalidateRect(view, nullptr, FALSE);
        }
    }
    releaseGraphics(mainState);
    releaseGraphics(secondState);
    if (d2d) d2d->Release();
    if (symbolFont) symbolFont->Release();
    symbolFont = nullptr;
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
    IDWriteFontCollection* fonts = nullptr;
    if (SUCCEEDED(dwrite->GetSystemFontCollection(&fonts))) {
        UINT32 index = 0;
        BOOL exists = FALSE;
        IDWriteFontFamily* family = nullptr;
        if (SUCCEEDED(fonts->FindFamilyName(L"Segoe UI Symbol", &index, &exists)) && exists &&
            SUCCEEDED(fonts->GetFontFamily(index, &family))) {
            family->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                DWRITE_FONT_STYLE_NORMAL, &symbolFont);
            family->Release();
        }
        fonts->Release();
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
    constexpr UINT NppBufferActivated = 1010;
    if (!notification) return;
    if (notification->code == NppBufferActivated) {
        mainState.scanDirty = secondState.scanDirty = true;
        mainState.representationsDirty = secondState.representationsDirty = true;
    }
    constexpr UINT ScintillaModified = 2008;
    if (notification->code == ScintillaModified &&
        (notification->hwndFrom == windows.mainView || notification->hwndFrom == windows.secondView)) {
        struct ModifiedNotification {
            NMHDR header;
            INT_PTR position;
            int character;
            int modifiers;
            int modificationType;
        };
        constexpr int InsertOrDeleteText = 3;
        if (reinterpret_cast<const ModifiedNotification*>(notification)->modificationType & InsertOrDeleteText) {
            // A clone may report its changes through only one view.
            mainState.scanDirty = true;
            secondState.scanDirty = true;
        }
    }
    if (notification->code == NppReady) enable();
    if (notification->code == NppShutdown) disable();
}
__declspec(dllexport) LRESULT messageProc(UINT, WPARAM, LPARAM) { return TRUE; }
__declspec(dllexport) BOOL isUnicode() { return TRUE; }
}

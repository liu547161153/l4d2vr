
namespace
{
    inline bool SafeReadU8(const unsigned char* base, int off, unsigned char& out)
    {
        __try { out = *reinterpret_cast<const unsigned char*>(base + off); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { out = 0; return false; }
    }

    inline bool SafeReadInt(const unsigned char* base, int off, int& out)
    {
        __try { out = *reinterpret_cast<const int*>(base + off); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { out = 0; return false; }
    }

    inline bool SafeReadFloat(const unsigned char* base, int off, float& out)
    {
        __try { out = *reinterpret_cast<const float*>(base + off); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { out = 0.0f; return false; }
    }

    // NOTE: This file uses a tiny 5x7 glyph set for UI labels (LC/RC, item abbreviations, etc.).
    // For player names (teammates / aim target), we also support UTF-8 via a GDI fallback renderer
    // so Chinese/JP/KR/etc names can display correctly on the HUD overlay.

    struct Rgba
    {
        unsigned char r, g, b, a;
    };

    struct HudSurface
    {
        unsigned char* pixels = nullptr; // RGBA
        int w = 0;
        int h = 0;
        int stride = 0; // bytes per row
    };

    inline void FillRect(const HudSurface& s, int x, int y, int w, int h, const Rgba& c)
    {
        if (!s.pixels || w <= 0 || h <= 0)
            return;
        const int x0 = (std::max)(0, x);
        const int y0 = (std::max)(0, y);
        const int x1 = (std::min)(s.w, x + w);
        const int y1 = (std::min)(s.h, y + h);
        for (int yy = y0; yy < y1; ++yy)
        {
            unsigned char* row = s.pixels + yy * s.stride;
            for (int xx = x0; xx < x1; ++xx)
            {
                unsigned char* p = row + xx * 4;
                p[0] = c.r;
                p[1] = c.g;
                p[2] = c.b;
                p[3] = c.a;
            }
        }
    }

    inline void Clear(const HudSurface& s, const Rgba& c)
    {
        FillRect(s, 0, 0, s.w, s.h, c);
    }

    inline void DrawRect(const HudSurface& s, int x, int y, int w, int h, const Rgba& c, int thickness = 1)
    {
        if (w <= 0 || h <= 0 || thickness <= 0)
            return;
        FillRect(s, x, y, w, thickness, c);
        FillRect(s, x, y + h - thickness, w, thickness, c);
        FillRect(s, x, y, thickness, h, c);
        FillRect(s, x + w - thickness, y, thickness, h, c);
    }

    inline const unsigned char* Glyph5x7(char ch)
    {
        // 7 rows, 5 bits wide. Uppercase-only mini font; unknown -> '?'.
        static const unsigned char kBlank[7] = { 0,0,0,0,0,0,0 };
        static const unsigned char kQMark[7] = { 0x1E,0x21,0x01,0x06,0x08,0x00,0x08 };
        static const unsigned char kSpace[7] = { 0,0,0,0,0,0,0 };
        static const unsigned char kColon[7] = { 0x00,0x04,0x00,0x00,0x04,0x00,0x00 };
        static const unsigned char kPercent[7] = { 0x19,0x1A,0x04,0x08,0x16,0x13,0x00 };
        static const unsigned char kDash[7] = { 0x00,0x00,0x00,0x1F,0x00,0x00,0x00 };
        static const unsigned char kSlash[7] = { 0x01,0x02,0x04,0x08,0x10,0x20,0x00 };
        static const unsigned char kPlus[7] = { 0x00,0x04,0x04,0x1F,0x04,0x04,0x00 };

        static const unsigned char kDigits[10][7] = {
            { 0x1E,0x21,0x23,0x25,0x31,0x21,0x1E },
            { 0x08,0x18,0x08,0x08,0x08,0x08,0x1C },
            { 0x1E,0x21,0x01,0x06,0x18,0x20,0x3F },
            { 0x1E,0x21,0x01,0x0E,0x01,0x21,0x1E },
            { 0x02,0x06,0x0A,0x12,0x3F,0x02,0x02 },
            { 0x3F,0x20,0x3E,0x01,0x01,0x21,0x1E },
            { 0x0E,0x10,0x20,0x3E,0x21,0x21,0x1E },
            { 0x3F,0x01,0x02,0x04,0x08,0x10,0x10 },
            { 0x1E,0x21,0x21,0x1E,0x21,0x21,0x1E },
            { 0x1E,0x21,0x21,0x1F,0x01,0x02,0x1C },
        };

        static const unsigned char kLetters[26][7] = {
            { 0x0E,0x11,0x11,0x1F,0x11,0x11,0x11 }, { 0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E },
            { 0x0F,0x10,0x20,0x20,0x20,0x10,0x0F }, { 0x1E,0x11,0x11,0x11,0x11,0x11,0x1E },
            { 0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F }, { 0x1F,0x10,0x10,0x1E,0x10,0x10,0x10 },
            { 0x0F,0x10,0x20,0x27,0x21,0x11,0x0F }, { 0x11,0x11,0x11,0x1F,0x11,0x11,0x11 },
            { 0x1F,0x04,0x04,0x04,0x04,0x04,0x1F }, { 0x1F,0x02,0x02,0x02,0x12,0x12,0x0C },
            { 0x11,0x12,0x14,0x18,0x14,0x12,0x11 }, { 0x10,0x10,0x10,0x10,0x10,0x10,0x1F },
            { 0x11,0x1B,0x15,0x15,0x11,0x11,0x11 }, { 0x11,0x19,0x15,0x13,0x11,0x11,0x11 },
            { 0x0E,0x11,0x11,0x11,0x11,0x11,0x0E }, { 0x1E,0x11,0x11,0x1E,0x10,0x10,0x10 },
            { 0x0E,0x11,0x11,0x11,0x15,0x12,0x0D }, { 0x1E,0x11,0x11,0x1E,0x14,0x12,0x11 },
            { 0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E }, { 0x1F,0x04,0x04,0x04,0x04,0x04,0x04 },
            { 0x11,0x11,0x11,0x11,0x11,0x11,0x0E }, { 0x11,0x11,0x11,0x11,0x11,0x0A,0x04 },
            { 0x11,0x11,0x11,0x15,0x15,0x15,0x0A }, { 0x11,0x11,0x0A,0x04,0x0A,0x11,0x11 },
            { 0x11,0x11,0x0A,0x04,0x04,0x04,0x04 }, { 0x1F,0x01,0x02,0x04,0x08,0x10,0x1F },
        };

        if (ch == ' ') return kSpace;
        if (ch == ':') return kColon;
        if (ch == '%') return kPercent;
        if (ch == '-') return kDash;
        if (ch == '/') return kSlash;
        if (ch == '+') return kPlus;
        if (ch >= '0' && ch <= '9') return kDigits[ch - '0'];
        if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
        if (ch >= 'A' && ch <= 'Z') return kLetters[ch - 'A'];
        return kQMark;
    }

    inline void DrawChar5x7(const HudSurface& s, int x, int y, char ch, const Rgba& c, int scale = 1)
    {
        const unsigned char* rows = Glyph5x7(ch);
        for (int yy = 0; yy < 7; ++yy)
        {
            const unsigned char bits = rows[yy];
            for (int xx = 0; xx < 5; ++xx)
            {
                if (bits & (1u << (4 - xx)))
                    FillRect(s, x + xx * scale, y + yy * scale, scale, scale, c);
            }
        }
    }

    inline void DrawText5x7(const HudSurface& s, int x, int y, const char* text, const Rgba& c, int scale = 1)
    {
        if (!text)
            return;
        int penX = x;
        for (const char* p = text; *p; ++p)
        {
            DrawChar5x7(s, penX, y, *p, c, scale);
            penX += (6 * scale);
        }
    }


    inline void DrawText5x7Outlined(const HudSurface& s, int x, int y, const char* text, const Rgba& c, int scale = 1)
    {
        // Cheap readability boost: draw a 1px outline (4-neighborhood) then the text.
        // Works well for thin 5x7 glyphs on bright/complex backgrounds.
        if (!text)
            return;
        const int off = (std::max)(1, scale / 2);
        const Rgba o{ 0, 0, 0, (unsigned char)std::min<int>(255, (int)c.a) };
        DrawText5x7(s, x - off, y, text, o, scale);
        DrawText5x7(s, x + off, y, text, o, scale);
        DrawText5x7(s, x, y - off, text, o, scale);
        DrawText5x7(s, x, y + off, text, o, scale);
        DrawText5x7(s, x, y, text, c, scale);
    }


    inline bool ContainsNonAscii(const char* s)
    {
        if (!s) return false;
        for (const unsigned char* p = (const unsigned char*)s; *p; ++p)
        {
            if (*p >= 0x80)
                return true;
        }
        return false;
    }

    inline uint32_t Fnv1a32(const void* data, size_t len, uint32_t h = 2166136261u)
    {
        const unsigned char* p = (const unsigned char*)data;
        for (size_t i = 0; i < len; ++i)
        {
            h ^= (uint32_t)p[i];
            h *= 16777619u;
        }
        return h;
    }

    inline uint32_t Fnv1aStr32(const char* s, uint32_t h = 2166136261u)
    {
        if (!s) return h;
        return Fnv1a32(s, strlen(s), h);
    }

    // ----------------------------
    // UTF-8 text rendering fallback (GDI -> alpha mask -> blend into HudSurface)
    // Only used for non-ASCII names so we can support Chinese/JP/KR/etc.
    // ----------------------------
        inline std::wstring Utf8ToWideFallback(const char* s)
    {
        if (!s || !*s) return L"";

        auto decode = [&](UINT cp, DWORD flags) -> std::wstring
        {
            int len = MultiByteToWideChar(cp, flags, s, -1, nullptr, 0);
            if (len <= 0) return L"";
            std::wstring out((size_t)len - 1, L'\0');
            MultiByteToWideChar(cp, flags, s, -1, out.data(), len);
            return out;
        };

        // 1) Strict UTF-8 first (reject invalid sequences).
        std::wstring w = decode(CP_UTF8, MB_ERR_INVALID_CHARS);
        if (!w.empty())
            return w;

        // 2) Score candidates from a few likely encodings.
        auto score = [&](const std::wstring& ws) -> int
        {
            int sc = 0;
            for (wchar_t ch : ws)
            {
                const unsigned int c = (unsigned int)ch;
                if (c == 0xFFFD) { sc -= 20; continue; } // replacement char
                if (c < 0x20 && ch != L' ' && ch != L'\t') { sc -= 10; continue; } // control
                if ((c >= 0x4E00 && c <= 0x9FFF) || (c >= 0x3400 && c <= 0x4DBF)) sc += 10; // CJK
                else if (c >= 0x3040 && c <= 0x30FF) sc += 10; // Kana
                else if (c >= 0xAC00 && c <= 0xD7AF) sc += 10; // Hangul
                else if (c < 0x80) sc += 2;
                else sc += 3;
            }
            return sc;
        };

        struct Cand { UINT cp; DWORD flags; int bonus; };
        const Cand cands[] =
        {
            { CP_UTF8, 0, 0 },      // permissive UTF-8
            { CP_ACP,  0, 2 },      // system ANSI codepage
            { 936u,    0, 6 },      // GBK
            { 950u,    0, 6 },      // Big5
            { 932u,    0, 6 },      // Shift-JIS
            { 949u,    0, 6 },      // Korean (EUC-KR)
        };

        int bestScore = -1000000000;
        std::wstring best;
        for (const auto& c : cands)
        {
            std::wstring cand = decode(c.cp, c.flags);
            if (cand.empty())
                continue;

            const int sc = score(cand) + c.bonus;
            if (sc > bestScore)
            {
                bestScore = sc;
                best.swap(cand);
            }
        }
        return best;
    }

    inline size_t Utf8SafePrefixBytes(const char* s, size_t maxBytes)
    {
        if (!s) return 0;
        size_t i = 0;
        const unsigned char* p = (const unsigned char*)s;
        while (i < maxBytes && p[i])
        {
            unsigned char c = p[i];
            size_t clen = 1;
            if (c < 0x80) clen = 1;
            else if ((c & 0xE0) == 0xC0) clen = 2;
            else if ((c & 0xF0) == 0xE0) clen = 3;
            else if ((c & 0xF8) == 0xF0) clen = 4;
            else clen = 1; // invalid lead byte, treat as single

            if (i + clen > maxBytes)
                break;

            // Basic continuation validation (avoid copying broken sequences when the src itself is truncated)
            if (clen > 1)
            {
                bool ok = true;
                for (size_t j = 1; j < clen; ++j)
                {
                    unsigned char cc = p[i + j];
                    if ((cc & 0xC0) != 0x80) { ok = false; break; }
                }
                if (!ok)
                {
                    // Broken UTF-8 sequence in the source; stop here to avoid feeding invalid UTF-8 to the renderer.
                    break;
                }
            }
            i += clen;
        }
        return i;
    }

    inline void Utf8SafeCopy(char* dst, size_t dstSize, const char* src)
    {
        if (!dst || dstSize == 0) return;
        dst[0] = 0;
        if (!src) return;

        const size_t maxCopy = dstSize - 1;
        const size_t n = Utf8SafePrefixBytes(src, maxCopy);
        if (n > 0)
            memcpy(dst, src, n);
        dst[n] = 0;
    }

    inline void ByteSafeCopy(char* dst, size_t dstSize, const char* src)
    {
        if (!dst || dstSize == 0) return;
        dst[0] = 0;
        if (!src) return;

        const size_t maxCopy = dstSize - 1;
        size_t n = 0;
        for (; n < maxCopy && src[n]; ++n) {}
        if (n > 0)
            memcpy(dst, src, n);
        dst[n] = 0;
    }

inline const wchar_t* PickHudFontFaceForText(const std::wstring& ws)
    {
        // Best-effort font picking for broad Unicode coverage on Windows.
        // We still rely on GDI font linking/fallback if a glyph isn't in the chosen face.
        bool hasCJK = false;
        bool hasKana = false;
        bool hasHangul = false;
        bool hasThai = false;
        bool hasDevanagari = false;

        for (wchar_t ch : ws)
        {
            const unsigned int c = (unsigned int)ch;
            if (c >= 0x4E00 && c <= 0x9FFF) hasCJK = true;            // CJK Unified Ideographs
            else if (c >= 0x3040 && c <= 0x30FF) hasKana = true;       // Hiragana/Katakana
            else if (c >= 0xAC00 && c <= 0xD7AF) hasHangul = true;     // Hangul syllables
            else if (c >= 0x0E00 && c <= 0x0E7F) hasThai = true;       // Thai
            else if (c >= 0x0900 && c <= 0x097F) hasDevanagari = true; // Devanagari
        }

        if (hasHangul) return L"Malgun Gothic";
        if (hasKana) return L"Meiryo UI";
        if (hasCJK) return L"Microsoft YaHei UI";
        if (hasThai) return L"Leelawadee UI";
        if (hasDevanagari) return L"Nirmala UI";
        return L"Segoe UI";
    }

    // Name fitting policy for HUD:
    // - Budget is 12 "units" (≈ 12 ASCII chars or 6 CJK chars).
    // - For every extra 2 units, shrink by 10%.
    // - Cap shrink at 40% (min scale 60%).
    // - If still longer, hard-truncate.
    inline int Utf8HudUnits(const char* s)
    {
        if (!s) return 0;
        int units = 0;
        const unsigned char* p = (const unsigned char*)s;
        while (*p)
        {
            if (*p < 0x80)
            {
                units += 1;
                ++p;
                continue;
            }

            // Skip a UTF-8 sequence (best-effort; treat invalid bytes as 1 codepoint).
            unsigned char lead = *p;
            int n = 1;
            if ((lead & 0xE0) == 0xC0) n = 2;
            else if ((lead & 0xF0) == 0xE0) n = 3;
            else if ((lead & 0xF8) == 0xF0) n = 4;

            int avail = 0;
            while (avail < n && p[avail])
                ++avail;
            if (avail <= 0)
                break;
            p += avail;
            units += 2;
        }
        return units;
    }

    inline std::string Utf8TruncateHudUnits(const char* s, int maxUnits)
    {
        if (!s || maxUnits <= 0) return std::string();
        std::string out;
        out.reserve(strlen(s));

        int units = 0;
        const unsigned char* p = (const unsigned char*)s;
        while (*p)
        {
            if (*p < 0x80)
            {
                if (units + 1 > maxUnits) break;
                out.push_back((char)*p);
                units += 1;
                ++p;
                continue;
            }

            unsigned char lead = *p;
            int n = 1;
            if ((lead & 0xE0) == 0xC0) n = 2;
            else if ((lead & 0xF0) == 0xE0) n = 3;
            else if ((lead & 0xF8) == 0xF0) n = 4;

            if (units + 2 > maxUnits) break;

            int avail = 0;
            while (avail < n && p[avail])
                ++avail;
            if (avail <= 0)
                break;
            for (int i = 0; i < avail; ++i)
                out.push_back((char)p[i]);
            p += avail;
            units += 2;
        }

        return out;
    }

    inline float HudNameScaleForUnits(int units, int baseUnits = 12)
    {
        if (units <= baseUnits) return 1.0f;
        const int over = units - baseUnits;
        const int steps = (over + 1) / 2; // ceil(over/2)
        const float shrink = (std::min)(0.4f, 0.1f * (float)steps);
        return 1.0f - shrink;
    }
    struct GdiTextMask
    {
        HDC hdc = nullptr;
        HBITMAP bmp = nullptr;
        void* bits = nullptr;
        int w = 0;
        int h = 0;
        HFONT font = nullptr;
        int fontPx = 0;
        bool bold = false;
        wchar_t face[64] = L"Segoe UI";

        ~GdiTextMask() { destroy(); }

        void destroy()
        {
            if (font) { DeleteObject(font); font = nullptr; }
            if (hdc) { DeleteDC(hdc); hdc = nullptr; }
            if (bmp) { DeleteObject(bmp); bmp = nullptr; }
            bits = nullptr; w = h = 0;
        }

        void ensure(int W, int H)
        {
            if (W <= w && H <= h && hdc && bmp && bits)
                return;

            W = (std::max)(W, w);
            H = (std::max)(H, h);
            if (W <= 0) W = 1;
            if (H <= 0) H = 1;

            if (hdc) { DeleteDC(hdc); hdc = nullptr; }
            if (bmp) { DeleteObject(bmp); bmp = nullptr; }
            bits = nullptr;

            BITMAPINFO bmi{};
            bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth = W;
            bmi.bmiHeader.biHeight = -H;
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = BI_RGB;

            hdc = CreateCompatibleDC(nullptr);
            bmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
            SelectObject(hdc, bmp);
            w = W;
            h = H;
        }

        void ensureFont(int px, bool isBold, const wchar_t* fontFace)
        {
            if (px <= 0) px = 12;
            if (!fontFace || !*fontFace) fontFace = L"Segoe UI";

            if (font && fontPx == px && bold == isBold && wcscmp(face, fontFace) == 0)
                return;

            if (font) { DeleteObject(font); font = nullptr; }
            wcsncpy_s(face, fontFace, _TRUNCATE);
            bold = isBold;
            fontPx = px;

            font = CreateFontW(
                -px, 0, 0, 0,
                isBold ? FW_BOLD : FW_NORMAL,
                FALSE, FALSE, FALSE,
                DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_DONTCARE,
                face);
        }

        void clear()
        {
            if (bits)
                memset(bits, 0, (size_t)w * (size_t)h * 4);
        }
    };

    inline void BlendMaskToHud(const HudSurface& dst, int x, int y, const GdiTextMask& mask, int srcW, int srcH, const Rgba& col)
    {
        if (!dst.pixels || !mask.bits || srcW <= 0 || srcH <= 0)
            return;

        const uint8_t* src = (const uint8_t*)mask.bits; // BGRA
        for (int yy = 0; yy < srcH; ++yy)
        {
            const int dy = y + yy;
            if (dy < 0 || dy >= dst.h) continue;
            uint8_t* drow = dst.pixels + dy * dst.stride;
            const uint8_t* srow = src + (size_t)yy * (size_t)mask.w * 4;
            for (int xx = 0; xx < srcW; ++xx)
            {
                const int dx = x + xx;
                if (dx < 0 || dx >= dst.w) continue;

                const uint8_t sb = srow[xx * 4 + 0];
                const uint8_t sg = srow[xx * 4 + 1];
                const uint8_t sr = srow[xx * 4 + 2];
                const uint8_t m = (uint8_t)std::max<int>(sr, std::max<int>(sg, sb));
                if (m == 0) continue;

                const int a = (m * (int)col.a) / 255;
                const int inv = 255 - a;

                uint8_t* dp = drow + dx * 4;
                dp[0] = (uint8_t)((col.r * a + dp[0] * inv) / 255);
                dp[1] = (uint8_t)((col.g * a + dp[1] * inv) / 255);
                dp[2] = (uint8_t)((col.b * a + dp[2] * inv) / 255);
                dp[3] = (uint8_t)(std::min)(255, (int)dp[3] + a);
            }
        }
    }

    inline void DrawTextUtf8OutlinedGdiClippedEx(const HudSurface& dst, int x, int y, int maxW, const char* utf8, int fontPx, const Rgba& col, bool ellipsis)
    {
        if (!utf8 || !*utf8 || !dst.pixels || maxW <= 0)
            return;

        static thread_local GdiTextMask g;
        const std::wstring ws = Utf8ToWideFallback(utf8);
        if (ws.empty())
            return;

        const int pad = 6;
        const int surfW = (std::max)(16, maxW + pad);
        const int surfH = (std::max)(16, fontPx + pad);
        g.ensure(surfW, surfH);
        g.ensureFont(fontPx, true, PickHudFontFaceForText(ws));
        g.clear();

        SelectObject(g.hdc, g.font);
        SetBkMode(g.hdc, TRANSPARENT);

        RECT rc{ 0, 0, maxW, surfH };
        const UINT flags = DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | (ellipsis ? DT_END_ELLIPSIS : 0);

        SetTextColor(g.hdc, RGB(0, 0, 0));
        RECT r1 = rc; OffsetRect(&r1, -1, 0); DrawTextW(g.hdc, ws.c_str(), (int)ws.size(), &r1, flags);
        RECT r2 = rc; OffsetRect(&r2, +1, 0); DrawTextW(g.hdc, ws.c_str(), (int)ws.size(), &r2, flags);
        RECT r3 = rc; OffsetRect(&r3, 0, -1); DrawTextW(g.hdc, ws.c_str(), (int)ws.size(), &r3, flags);
        RECT r4 = rc; OffsetRect(&r4, 0, +1); DrawTextW(g.hdc, ws.c_str(), (int)ws.size(), &r4, flags);

        SetTextColor(g.hdc, RGB(255, 255, 255));
        DrawTextW(g.hdc, ws.c_str(), (int)ws.size(), &rc, flags);

        const int blendW = (std::min)(maxW, g.w);
        const int blendH = (std::min)(surfH, g.h);
        BlendMaskToHud(dst, x, y, g, blendW, blendH, col);
    }

    inline void DrawTextUtf8OutlinedGdiClipped(const HudSurface& dst, int x, int y, int maxW, const char* utf8, int fontPx, const Rgba& col)
    {
        DrawTextUtf8OutlinedGdiClippedEx(dst, x, y, maxW, utf8, fontPx, col, true);
    }

    inline void DrawHudTextAuto(const HudSurface& s, int x, int y, int maxW, const char* text, const Rgba& c, int scaleFor5x7 = 1, int gdiFontPx = 14)
    {
        if (!text) return;
        if (ContainsNonAscii(text))
        {
            DrawTextUtf8OutlinedGdiClipped(s, x, y, maxW, text, gdiFontPx, c);
            return;
        }
        DrawText5x7Outlined(s, x, y, text, c, scaleFor5x7);
    }

    inline void DrawInfinity(const HudSurface& s, int x, int y, int w, int h, const Rgba& c)
    {
        // Minimal ∞ icon: two loops with a crossing.
        const int t = (std::max)(1, h / 4);
        const int midY = y + h / 2;
        const int leftCx = x + w / 4;
        const int rightCx = x + (3 * w) / 4;
        const int rX = (std::max)(2, w / 6);
        const int rY = (std::max)(2, h / 3);

        auto ring = [&](int cx)
        {
            DrawRect(s, cx - rX, midY - rY, 2 * rX, 2 * rY, c, t);
        };
        ring(leftCx);
        ring(rightCx);
        FillRect(s, x + w / 2 - 1, midY - 1, 2, 2, c);
    }

    inline void DrawIconCross(const HudSurface& s, int x, int y, int size, const Rgba& c)
    {
        const int t = (std::max)(1, size / 5);
        const int mid = size / 2;
        FillRect(s, x + mid - t, y + t, t * 2, size - t * 2, c);
        FillRect(s, x + t, y + mid - t, size - t * 2, t * 2, c);
    }

    inline void DrawIconPills(const HudSurface& s, int x, int y, int size, const Rgba& c)
    {
        const int w = size;
        const int h = size / 2;
        FillRect(s, x, y + h / 2, w, h, c);
        FillRect(s, x + w / 2 - 1, y + h / 2, 2, h, { 0,0,0,180 });
    }

    inline void DrawIconSyringe(const HudSurface& s, int x, int y, int size, const Rgba& c)
    {
        const int w = size;
        const int h = size;
        const int bodyH = (std::max)(2, h / 4);
        FillRect(s, x + w / 4, y + h / 2 - bodyH / 2, w / 2, bodyH, c);
        FillRect(s, x + w / 4 - 2, y + h / 2 - bodyH / 2, 2, bodyH, c);
        FillRect(s, x + 3 * w / 4, y + h / 2 - 1, w / 4, 2, c);
        FillRect(s, x + 3 * w / 4 + w / 4 - 2, y + h / 2 - 1, 2, 2, c);
    }

    inline void DrawIconFlame(const HudSurface& s, int x, int y, int size)
    {
        const Rgba c1{ 255, 120, 0, 255 };
        const Rgba c2{ 255, 220, 40, 255 };
        FillRect(s, x + size / 3, y + size / 3, size / 3, size / 2, c1);
        FillRect(s, x + size / 2 - 2, y + size / 4, 4, size / 3, c2);
    }

    inline void DrawIconBomb(const HudSurface& s, int x, int y, int size)
    {
        const Rgba c{ 210, 210, 210, 255 };
        FillRect(s, x + size / 4, y + size / 3, size / 2, size / 2, c);
        FillRect(s, x + size / 2, y + size / 4, size / 4, 2, c);
    }

    inline void DrawIconJar(const HudSurface& s, int x, int y, int size)
    {
        const Rgba c{ 0, 255, 120, 255 };
        DrawRect(s, x + size / 3, y + size / 3, size / 3, size / 2, c, 2);
        FillRect(s, x + size / 3, y + size / 3, size / 3, 2, c);
    }

    inline void DrawCornerBrackets(const HudSurface& s, int x, int y, int w, int h, const Rgba& c)
    {
        const int t = 2;
        const int L = 10;
        FillRect(s, x, y, L, t, c);
        FillRect(s, x, y, t, L, c);
        FillRect(s, x + w - L, y, L, t, c);
        FillRect(s, x + w - t, y, t, L, c);
        FillRect(s, x, y + h - t, L, t, c);
        FillRect(s, x, y + h - L, t, L, c);
        FillRect(s, x + w - L, y + h - t, L, t, c);
        FillRect(s, x + w - t, y + h - L, t, L, c);
    }

    struct SevenSegStyle
    {
        int len = 12;      // segment length in pixels
        int thick = 3;    // segment thickness
        int gap = 2;      // inner gap around segments
        int digitGap = 5; // spacing between digits
    };

    inline unsigned char SevenSegMaskForDigit(int d)
    {
        // Bits: 0=A(top),1=B(upper-right),2=C(lower-right),3=D(bottom),4=E(lower-left),5=F(upper-left),6=G(mid)
        static const unsigned char kMap[10] = {
            0b0111111, // 0
            0b0000110, // 1
            0b1011011, // 2
            0b1001111, // 3
            0b1100110, // 4
            0b1101101, // 5
            0b1111101, // 6
            0b0000111, // 7
            0b1111111, // 8
            0b1101111, // 9
        };
        if (d < 0 || d > 9)
            return 0;
        return kMap[d];
    }

    inline void Draw7SegDigit(const HudSurface& s, int x, int y, int digit, const SevenSegStyle& st, const Rgba& c)
    {
        const unsigned char m = SevenSegMaskForDigit(digit);
        const int L = st.len;
        const int T = st.thick;

        // Horizontal segments are L x T, vertical segments are T x L
        const int x0 = x;
        const int y0 = y;
        const int x1 = x0 + T + L;
        const int y1 = y0 + T + L;
        const int y2 = y0 + 2 * T + 2 * L;

        // A
        if (m & (1u << 0)) FillRect(s, x0 + T, y0, L, T, c);
        // B
        if (m & (1u << 1)) FillRect(s, x1, y0 + T, T, L, c);
        // C
        if (m & (1u << 2)) FillRect(s, x1, y1 + T, T, L, c);
        // D
        if (m & (1u << 3)) FillRect(s, x0 + T, y2, L, T, c);
        // E
        if (m & (1u << 4)) FillRect(s, x0, y1 + T, T, L, c);
        // F
        if (m & (1u << 5)) FillRect(s, x0, y0 + T, T, L, c);
        // G
        if (m & (1u << 6)) FillRect(s, x0 + T, y1, L, T, c);
    }

    inline int SevenSegDigitW(const SevenSegStyle& st) { return st.len + 2 * st.thick; }
    inline int SevenSegDigitH(const SevenSegStyle& st) { return 2 * st.len + 3 * st.thick; }

    inline int Draw7SegInt(const HudSurface& s, int x, int y, int value, const SevenSegStyle& st, const Rgba& c)
    {
        // Returns drawn width.
        if (value < 0)
            value = 0;

        char buf[16];
        std::snprintf(buf, sizeof(buf), "%d", value);

        int penX = x;
        for (const char* p = buf; *p; ++p)
        {
            const char ch = *p;
            if (ch >= '0' && ch <= '9')
            {
                Draw7SegDigit(s, penX, y, ch - '0', st, c);
                penX += SevenSegDigitW(st) + st.digitGap;
            }
        }
        return penX - x;
    }

    inline void Draw7SegPlus(const HudSurface& s, int x, int y, int size, const Rgba& c)
    {
        const int t = (std::max)(2, size / 5);
        FillRect(s, x + size / 2 - t, y + t, t * 2, size - t * 2, c);
        FillRect(s, x + t, y + size / 2 - t, size - t * 2, t * 2, c);
    }

    inline void DrawBatteryBar(const HudSurface& s, int x, int y, int w, int h, int percent, bool charging)
    {
        if (percent < 0)
            return;
        percent = (std::max)(0, (std::min)(100, percent));
        const Rgba frame{ 80, 120, 140, 220 };
        const Rgba fill{ 60, 220, 255, 220 };
        const Rgba warn{ 255, 120, 60, 220 };
        const int capW = (std::max)(2, w / 10);
        DrawRect(s, x, y, w, h, frame, 1);
        FillRect(s, x + w, y + h / 3, capW, h / 3, frame);

        const int innerW = w - 2;
        const int innerH = h - 2;
        const int fillW = (innerW * percent) / 100;
        FillRect(s, x + 1, y + 1, fillW, innerH, percent <= 20 ? warn : fill);

        if (charging)
        {
            // Tiny lightning bolt
            const Rgba bolt{ 255, 255, 255, 220 };
            FillRect(s, x + w / 2 - 1, y + 2, 3, h - 4, bolt);
            FillRect(s, x + w / 2 - 4, y + h / 2 - 1, 6, 3, bolt);
        }
    }

    inline const char* WeaponShortTag(int weaponId)
    {
        using W = C_WeaponCSBase::WeaponID;
        switch ((W)weaponId)
        {
        case W::PISTOL: return "PST";
        case W::MAGNUM: return "MAG";
        case W::UZI: return "SMG";
        case W::MAC10: return "SMG";
        case W::MP5: return "MP5";
        case W::SG552: return "SG5";
        case W::M16A1: return "M16";
        case W::AK47: return "AK";
        case W::SCAR: return "SCAR";
        case W::HUNTING_RIFLE: return "HUNT";
        case W::SNIPER_MILITARY: return "MIL";
        case W::AWP: return "AWP";
        case W::SCOUT: return "SCOUT";
        case W::M60: return "M60";
        case W::PUMPSHOTGUN: return "PUMP";
        case W::SHOTGUN_CHROME: return "CHR";
        case W::AUTOSHOTGUN: return "AUTO";
        case W::SPAS: return "SPAS";
        case W::GRENADE_LAUNCHER: return "GL";
        case W::MELEE: return "MELEE";
        case W::CHAINSAW: return "SAW";
        default: return "";
        }
    }

}

VR::VR(Game* game)
{
    m_Game = game;

    char errorString[MAX_STR_LEN];

    vr::HmdError error = vr::VRInitError_None;
    m_System = vr::VR_Init(&error, vr::VRApplication_Scene);

    if (error != vr::VRInitError_None)
    {
        snprintf(errorString, MAX_STR_LEN, "VR_Init failed: %s", vr::VR_GetVRInitErrorAsEnglishDescription(error));
        Game::errorMsg(errorString);
        return;
    }

    m_Compositor = vr::VRCompositor();
    if (!m_Compositor)
    {
        Game::errorMsg("Compositor initialization failed.");
        return;
    }

    char currentDir[MAX_STR_LEN];
    GetCurrentDirectory(MAX_STR_LEN, currentDir);
    m_ViewmodelAdjustmentSavePath = std::string(currentDir) + "\\viewmodel_adjustments.txt";
    LoadViewmodelAdjustments();

    ConfigureExplicitTiming();

    m_Input = vr::VRInput();
    m_System = vr::OpenVRInternal_ModuleContext().VRSystem();

    m_System->GetRecommendedRenderTargetSize(&m_RenderWidth, &m_RenderHeight);
    m_AntiAliasing = 0;

    float l_left = 0.0f, l_right = 0.0f, l_top = 0.0f, l_bottom = 0.0f;
    m_System->GetProjectionRaw(vr::EVREye::Eye_Left, &l_left, &l_right, &l_top, &l_bottom);

    float r_left = 0.0f, r_right = 0.0f, r_top = 0.0f, r_bottom = 0.0f;
    m_System->GetProjectionRaw(vr::EVREye::Eye_Right, &r_left, &r_right, &r_top, &r_bottom);

    float tanHalfFov[2];

    tanHalfFov[0] = (std::max)({ -l_left, l_right, -r_left, r_right });
    tanHalfFov[1] = (std::max)({ -l_top, l_bottom, -r_top, r_bottom });
    // For some headsets, the driver provided texture size doesn't match the geometric aspect ratio of the lenses.
    // In this case, we need to adjust the vertical tangent while still rendering to the recommended RT size.
    m_TextureBounds[0].uMin = 0.5f + 0.5f * l_left / tanHalfFov[0];
    m_TextureBounds[0].uMax = 0.5f + 0.5f * l_right / tanHalfFov[0];
    m_TextureBounds[0].vMin = 0.5f - 0.5f * l_bottom / tanHalfFov[1];
    m_TextureBounds[0].vMax = 0.5f - 0.5f * l_top / tanHalfFov[1];

    m_TextureBounds[1].uMin = 0.5f + 0.5f * r_left / tanHalfFov[0];
    m_TextureBounds[1].uMax = 0.5f + 0.5f * r_right / tanHalfFov[0];
    m_TextureBounds[1].vMin = 0.5f - 0.5f * r_bottom / tanHalfFov[1];
    m_TextureBounds[1].vMax = 0.5f - 0.5f * r_top / tanHalfFov[1];

    m_Aspect = tanHalfFov[0] / tanHalfFov[1];
    m_Fov = 2.0f * atan(tanHalfFov[0]) * 360 / (3.14159265358979323846 * 2);

    InstallApplicationManifest("manifest.vrmanifest");
    SetActionManifest("action_manifest.json");

    std::thread configParser(&VR::WaitForConfigUpdate, this);
    configParser.detach();

    while (!g_D3DVR9)
        Sleep(10);

    g_D3DVR9->GetBackBufferData(&m_VKBackBuffer);
    m_Overlay = vr::VROverlay();
    m_Overlay->CreateOverlay("MenuOverlayKey", "MenuOverlay", &m_MainMenuHandle);
    m_Overlay->CreateOverlay("HUDOverlayTopKey", "HUDOverlayTop", &m_HUDTopHandle);

    const char* bottomOverlayKeys[4] = { "HUDOverlayBottom1", "HUDOverlayBottom2", "HUDOverlayBottom3", "HUDOverlayBottom4" };
    for (int i = 0; i < 4; ++i)
    {
        m_Overlay->CreateOverlay(bottomOverlayKeys[i], bottomOverlayKeys[i], &m_HUDBottomHandles[i]);
    }

    // Gun-mounted scope lens overlay (render-to-texture)
    m_Overlay->CreateOverlay("ScopeOverlayKey", "ScopeOverlay", &m_ScopeHandle);
    m_Overlay->CreateOverlay("RearMirrorOverlayKey", "RearMirrorOverlay", &m_RearMirrorHandle);
    // Hand HUD overlays (raw textures, controller anchored)
    m_Overlay->CreateOverlay("LeftWristHudOverlayKey", "LeftWristHUD", &m_LeftWristHudHandle);
    m_Overlay->CreateOverlay("RightAmmoHudOverlayKey", "RightAmmoHUD", &m_RightAmmoHudHandle);

    m_Overlay->SetOverlayInputMethod(m_MainMenuHandle, vr::VROverlayInputMethod_Mouse);
    m_Overlay->SetOverlayInputMethod(m_HUDTopHandle, vr::VROverlayInputMethod_Mouse);
    for (vr::VROverlayHandle_t& overlay : m_HUDBottomHandles)
    {
        m_Overlay->SetOverlayInputMethod(overlay, vr::VROverlayInputMethod_Mouse);
    }

    // Scope overlay is purely visual
    m_Overlay->SetOverlayInputMethod(m_ScopeHandle, vr::VROverlayInputMethod_None);
    m_Overlay->SetOverlayInputMethod(m_RearMirrorHandle, vr::VROverlayInputMethod_None);
    m_Overlay->SetOverlayInputMethod(m_LeftWristHudHandle, vr::VROverlayInputMethod_None);
    m_Overlay->SetOverlayInputMethod(m_RightAmmoHudHandle, vr::VROverlayInputMethod_None);
    m_Overlay->SetOverlayFlag(m_ScopeHandle, vr::VROverlayFlags_IgnoreTextureAlpha, true);
    m_Overlay->SetOverlayFlag(m_RearMirrorHandle, vr::VROverlayFlags_IgnoreTextureAlpha, true);


    m_Overlay->SetOverlayFlag(m_MainMenuHandle, vr::VROverlayFlags_SendVRDiscreteScrollEvents, true);
    m_Overlay->SetOverlayFlag(m_HUDTopHandle, vr::VROverlayFlags_SendVRDiscreteScrollEvents, true);
    for (vr::VROverlayHandle_t& overlay : m_HUDBottomHandles)
    {
        m_Overlay->SetOverlayFlag(overlay, vr::VROverlayFlags_SendVRDiscreteScrollEvents, true);
    }

    int windowWidth, windowHeight;
    m_Game->m_MaterialSystem->GetRenderContext()->GetWindowSize(windowWidth, windowHeight);

    const vr::HmdVector2_t mouseScaleHUD = { windowWidth, windowHeight };
    m_Overlay->SetOverlayMouseScale(m_HUDTopHandle, &mouseScaleHUD);
    for (vr::VROverlayHandle_t& overlay : m_HUDBottomHandles)
    {
        m_Overlay->SetOverlayMouseScale(overlay, &mouseScaleHUD);
    }

    const vr::HmdVector2_t mouseScaleMenu = { m_RenderWidth, m_RenderHeight };
    m_Overlay->SetOverlayMouseScale(m_MainMenuHandle, &mouseScaleMenu);

    if (!m_RenderFrameReadyEvent)
        m_RenderFrameReadyEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);

    UpdatePosesAndActions();
    FinishFrame();

    m_IsInitialized = true;
    m_IsVREnabled = true;
}

void VR::ConfigureExplicitTiming()
{
    if (!m_Compositor)
        return;

    m_Compositor->SetExplicitTimingMode(
        vr::VRCompositorTimingMode_Explicit_ApplicationPerformsPostPresentHandoff);

    m_CompositorExplicitTiming = true;
}

int VR::SetActionManifest(const char* fileName)
{
    char currentDir[MAX_STR_LEN];
    GetCurrentDirectory(MAX_STR_LEN, currentDir);
    char path[MAX_STR_LEN];
    sprintf_s(path, MAX_STR_LEN, "%s\\VR\\SteamVRActionManifest\\%s", currentDir, fileName);

    if (m_Input->SetActionManifestPath(path) != vr::VRInputError_None)
    {
        Game::errorMsg("SetActionManifestPath failed");
    }

    m_Input->GetActionHandle("/actions/main/in/ActivateVR", &m_ActionActivateVR);
    m_Input->GetActionHandle("/actions/main/in/Jump", &m_ActionJump);
    m_Input->GetActionHandle("/actions/main/in/PrimaryAttack", &m_ActionPrimaryAttack);
    m_Input->GetActionHandle("/actions/main/in/Reload", &m_ActionReload);
    m_Input->GetActionHandle("/actions/main/in/Use", &m_ActionUse);
    m_Input->GetActionHandle("/actions/main/in/Walk", &m_ActionWalk);
    m_Input->GetActionHandle("/actions/main/in/Turn", &m_ActionTurn);
    m_Input->GetActionHandle("/actions/main/in/SecondaryAttack", &m_ActionSecondaryAttack);
    m_Input->GetActionHandle("/actions/main/in/NextItem", &m_ActionNextItem);
    m_Input->GetActionHandle("/actions/main/in/PrevItem", &m_ActionPrevItem);
    m_Input->GetActionHandle("/actions/main/in/ResetPosition", &m_ActionResetPosition);
    m_Input->GetActionHandle("/actions/main/in/Crouch", &m_ActionCrouch);
    m_Input->GetActionHandle("/actions/main/in/Flashlight", &m_ActionFlashlight);
    m_Input->GetActionHandle("/actions/main/in/InventoryGripLeft", &m_ActionInventoryGripLeft);
    m_Input->GetActionHandle("/actions/main/in/InventoryGripRight", &m_ActionInventoryGripRight);
    m_Input->GetActionHandle("/actions/main/in/InventoryQuickSwitch", &m_ActionInventoryQuickSwitch);
    m_Input->GetActionHandle("/actions/main/in/SpecialInfectedAutoAimToggle", &m_ActionSpecialInfectedAutoAimToggle);
    m_Input->GetActionHandle("/actions/main/in/MenuSelect", &m_MenuSelect);
    m_Input->GetActionHandle("/actions/main/in/MenuBack", &m_MenuBack);
    m_Input->GetActionHandle("/actions/main/in/MenuUp", &m_MenuUp);
    m_Input->GetActionHandle("/actions/main/in/MenuDown", &m_MenuDown);
    m_Input->GetActionHandle("/actions/main/in/MenuLeft", &m_MenuLeft);
    m_Input->GetActionHandle("/actions/main/in/MenuRight", &m_MenuRight);
    m_Input->GetActionHandle("/actions/main/in/Spray", &m_Spray);
    m_Input->GetActionHandle("/actions/main/in/Scoreboard", &m_Scoreboard);
    m_Input->GetActionHandle("/actions/main/in/ToggleHUD", &m_ToggleHUD);
    m_Input->GetActionHandle("/actions/main/in/Pause", &m_Pause);
    m_Input->GetActionHandle("/actions/main/in/NonVRServerMovementAngleToggle", &m_NonVRServerMovementAngleToggle);
    m_Input->GetActionHandle("/actions/main/in/ScopeMagnificationToggle", &m_ActionScopeMagnificationToggle);
    // Aim-line friendly-fire guard toggle (bindable in SteamVR)
    m_Input->GetActionHandle("/actions/main/in/FriendlyFireBlockToggle", &m_ActionFriendlyFireBlockToggle);
    m_Input->GetActionHandle("/actions/main/in/CustomAction1", &m_CustomAction1);
    m_Input->GetActionHandle("/actions/main/in/CustomAction2", &m_CustomAction2);
    m_Input->GetActionHandle("/actions/main/in/CustomAction3", &m_CustomAction3);
    m_Input->GetActionHandle("/actions/main/in/CustomAction4", &m_CustomAction4);
    m_Input->GetActionHandle("/actions/main/in/CustomAction5", &m_CustomAction5);

    m_Input->GetActionSetHandle("/actions/main", &m_ActionSet);
    m_ActiveActionSet = {};
    m_ActiveActionSet.ulActionSet = m_ActionSet;

    return 0;
}

void VR::InstallApplicationManifest(const char* fileName)
{
    char currentDir[MAX_STR_LEN];
    GetCurrentDirectory(MAX_STR_LEN, currentDir);
    char path[MAX_STR_LEN];
    sprintf_s(path, MAX_STR_LEN, "%s\\VR\\%s", currentDir, fileName);

    vr::VRApplications()->AddApplicationManifest(path);
}


void VR::UpdateAutoMatQueueMode()
{
    // Mouse-mode (keyboard/mouse): do NOT auto-manage multicore.
    // Only enforce a safe mat_queue_mode in the main menu.
    if (m_MouseModeEnabled)
    {
        if (!m_IsVREnabled)
            return;

        if (!m_Game || !m_Game->m_EngineClient)
            return;

        const bool inGame = m_Game->m_EngineClient->IsInGame();
        if (inGame)
        {
            // Reset so the next trip back to the main menu re-applies the safety clamp.
            if (m_AutoMatQueueModeLastRequested == 0)
                m_AutoMatQueueModeLastRequested = -999;
            return;
        }

        // Main menu: force once per menu entry (no spam).
        if (m_AutoMatQueueModeLastRequested == 0)
            return;

        m_Game->ClientCmd_Unrestricted("mat_queue_mode 0");
        m_AutoMatQueueModeLastRequested = 0;
        m_AutoMatQueueModeLastCmdTime = std::chrono::steady_clock::now();

        Game::logMsg("[VR] MouseMode menu: mat_queue_mode -> 0");
        return;
    }


    if (!m_AutoMatQueueMode)
    {
        // AutoMatQueueMode=false: do NOT auto-manage multicore, but keep the main menu safe.
        // We force mat_queue_mode 0 once each time we are in the main menu (i.e. not in-game).
        if (!m_IsVREnabled)
            return;

        if (!m_Game || !m_Game->m_EngineClient)
            return;

        const bool inGame = m_Game->m_EngineClient->IsInGame();
        if (inGame)
        {
            // Reset so the next trip back to the main menu re-applies the safety clamp.
            if (m_AutoMatQueueModeLastRequested == 0)
                m_AutoMatQueueModeLastRequested = -999;
            return;
        }

        // Main menu: force once per menu entry (no spam).
        if (m_AutoMatQueueModeLastRequested == 0)
            return;

        m_Game->ClientCmd_Unrestricted("mat_queue_mode 0");
        m_AutoMatQueueModeLastRequested = 0;
        m_AutoMatQueueModeLastCmdTime = std::chrono::steady_clock::now();

        Game::logMsg("[VR] Menu safety: mat_queue_mode -> 0 (AutoMatQueueMode=false)");
        return;
    }


    // Avoid changing engine threading mode when VR rendering is not active.
    if (!m_IsVREnabled)
        return;

    if (!m_Game || !m_Game->m_EngineClient)
        return;

    const bool inGame = m_Game->m_EngineClient->IsInGame();

    // AutoMatQueueMode=true: in the main menu, set fps_max once to match the HMD refresh rate.
    // This helps avoid "menu stuck at 60 FPS" when the headset runs at 90/100/120+.
    if (!inGame)
    {
        if (!m_MenuFpsMaxSent)
        {
            float hmdHz = GetHmdDisplayFrequencyHz(true);
            int targetHz = (hmdHz > 1.0f) ? (int)(hmdHz + 0.5f) : 0;
            if (targetHz > 0)
            {
                targetHz = std::clamp(targetHz, 30, 360);

                // Only issue the command if the target changed. fps_max persists across map loads.
                if (m_MenuFpsMaxLastHz != targetHz)
                {
                    std::string cmd = std::string("fps_max ") + std::to_string(targetHz);
                    m_Game->ClientCmd_Unrestricted(cmd.c_str());
                    m_MenuFpsMaxLastHz = targetHz;

                    Game::logMsg("[VR] Menu: fps_max -> %d (HMD %.1fHz)", targetHz, hmdHz);
                }

                m_MenuFpsMaxSent = true;
            }
        }
    }
    else
    {
        // Reset when leaving the menu so we can re-check next time we return (e.g., refresh rate changed).
        m_MenuFpsMaxSent = false;
    }

    const bool paused = m_Game->m_EngineClient->IsPaused();
    const bool cursorVisible = (m_Game->m_VguiSurface) ? m_Game->m_VguiSurface->IsCursorVisible() : false;
    const bool scoreboardHeld = PressedDigitalAction(m_Scoreboard, false);

    // "Loading map": IsInGame can be true while the client entities are not ready yet.
    bool hasLocalPlayer = false;
    if (inGame)
    {
        const int playerIndex = m_Game->m_EngineClient->GetLocalPlayer();
        C_BasePlayer* localPlayer = (C_BasePlayer*)m_Game->GetClientEntity(playerIndex);
        hasLocalPlayer = (localPlayer != nullptr);
    }
    const bool loadingMap = inGame && !hasLocalPlayer;

    const int desiredMode = (!inGame || loadingMap || paused || cursorVisible || scoreboardHeld) ? 0 : 2;
    const int currentMode = m_Game->GetMatQueueMode();

    if (currentMode == desiredMode)
    {
        m_AutoMatQueueModeLastRequested = desiredMode;
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const float secondsSinceCmd = (m_AutoMatQueueModeLastCmdTime.time_since_epoch().count() == 0)
        ? 9999.0f
        : std::chrono::duration<float>(now - m_AutoMatQueueModeLastCmdTime).count();

    const bool isNewTarget = (m_AutoMatQueueModeLastRequested != desiredMode);

    // Throttle retries to avoid spamming the command if the engine temporarily rejects changes (e.g., during level transitions).
    if (m_AutoMatQueueModeLastRequested == desiredMode && secondsSinceCmd < 0.5f)
        return;

    std::string cmd = std::string("mat_queue_mode ") + std::to_string(desiredMode);
    m_Game->ClientCmd_Unrestricted(cmd.c_str());

    m_AutoMatQueueModeLastRequested = desiredMode;
    m_AutoMatQueueModeLastCmdTime = now;

    const char* reason = "in-game";
    if (!inGame) reason = "menu";
    else if (loadingMap) reason = "loading";
    else if (paused) reason = "paused";
    else if (scoreboardHeld) reason = "scoreboard";
    else if (cursorVisible) reason = "cursor";
    if (isNewTarget) Game::logMsg("[VR] AutoMatQueueMode -> mat_queue_mode %d (%s)", desiredMode, reason);
}

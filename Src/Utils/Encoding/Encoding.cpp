#include "Utils/Encoding/Encoding.h"
#include <array>
#include <cstdint>
#include <string>
#include <string_view>

// !!!!!!!!!!!
// GEN BY AI
// !!!!!!!!!!!

static const std::array<unsigned char, 0x10000> &getCp1251Lut()
{
    static const std::array<unsigned char, 0x10000> table = []
    {
        std::array<unsigned char, 0x10000> t;
        t.fill('?');

        for (int i = 0; i < 0x80; ++i)
            t[i] = static_cast<unsigned char>(i);

        t[0x0402] = 0x80;
        t[0x0403] = 0x81;
        t[0x201A] = 0x82;
        t[0x0453] = 0x83;
        t[0x201E] = 0x84;
        t[0x2026] = 0x85;
        t[0x2020] = 0x86;
        t[0x2021] = 0x87;
        t[0x20AC] = 0x88;
        t[0x2030] = 0x89;
        t[0x0409] = 0x8A;
        t[0x2039] = 0x8B;
        t[0x040A] = 0x8C;
        t[0x040C] = 0x8D;
        t[0x040B] = 0x8E;
        t[0x040F] = 0x8F;
        t[0x0452] = 0x90;
        t[0x2018] = 0x91;
        t[0x2019] = 0x92;
        t[0x201C] = 0x93;
        t[0x201D] = 0x94;
        t[0x2022] = 0x95;
        t[0x2013] = 0x96;
        t[0x2014] = 0x97;
        t[0x2122] = 0x98;
        t[0x0459] = 0x9A;
        t[0x203A] = 0x9B;
        t[0x045A] = 0x9C;
        t[0x045C] = 0x9D;
        t[0x045B] = 0x9E;
        t[0x045F] = 0x9F;

        t[0x00A0] = 0xA0;
        t[0x040E] = 0xA1;
        t[0x045E] = 0xA2;
        t[0x0408] = 0xA3;
        t[0x00A4] = 0xA4;
        t[0x0490] = 0xA5;
        t[0x00A6] = 0xA6;
        t[0x00A7] = 0xA7;
        t[0x0401] = 0xA8;
        t[0x00A9] = 0xA9;
        t[0x0404] = 0xAA;
        t[0x00AB] = 0xAB;
        t[0x00AC] = 0xAC;
        t[0x00AD] = 0xAD;
        t[0x00AE] = 0xAE;
        t[0x0407] = 0xAF;
        t[0x00B0] = 0xB0;
        t[0x00B1] = 0xB1;
        t[0x0406] = 0xB2;
        t[0x0456] = 0xB3;
        t[0x0491] = 0xB4;
        t[0x00B5] = 0xB5;
        t[0x00B6] = 0xB6;
        t[0x00B7] = 0xB7;
        t[0x0451] = 0xB8;
        t[0x2116] = 0xB9;
        t[0x0454] = 0xBA;
        t[0x00BB] = 0xBB;
        t[0x0458] = 0xBC;
        t[0x0405] = 0xBD;
        t[0x0455] = 0xBE;
        t[0x0457] = 0xBF;

        for (int i = 0; i < 32; ++i)
        {
            t[0x0410 + i] = 0xC0 + i;
            t[0x0430 + i] = 0xE0 + i;
        }

        return t;
    }();
    return table;
}

static const auto &getUtf8First()
{
    struct Utf8Info
    {
        signed char add_bytes;
        unsigned char mask;
        uint32_t min_val;
    };

    static const std::array<Utf8Info, 256> table = []
    {
        std::array<Utf8Info, 256> t{};
        for (auto &x : t)
            x.add_bytes = -1;

        for (int i = 0; i < 0x80; ++i)
        {
            t[i] = {0, 0x7F, 0};
        }

        for (int i = 0xC2; i <= 0xDF; ++i)
        {
            t[i] = {1, 0x1F, 0x80};
        }

        for (int i = 0xE0; i <= 0xEF; ++i)
        {
            t[i] = {2, 0x0F, 0x800};
        }

        for (int i = 0xF0; i <= 0xF4; ++i)
        {
            t[i] = {3, 0x07, 0x10000};
        }

        return t;
    }();

    return table;
}

// Обратная таблица строится сканом прямой — гарантирует консистентный
// roundtrip utf8 -> cp1251 -> utf8 для всех отображаемых символов.
static const std::array<uint32_t, 256> &getCp1251ToUnicode()
{
    static const std::array<uint32_t, 256> table = []
    {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 0x80; ++i)
            t[i] = i;
        for (uint32_t i = 0x80; i < 0x100; ++i)
            t[i] = '?';

        const auto &lut = getCp1251Lut();
        for (uint32_t cp = 0x80; cp <= 0xFFFF; ++cp)
        {
            const unsigned char b = lut[cp];
            if (b >= 0x80 && t[b] == '?')
                t[b] = cp;
        }
        return t;
    }();
    return table;
}

std::string Encoding::cp1251Toutf8(std::string_view input)
{
    const auto &table = getCp1251ToUnicode();

    std::string output;
    output.reserve(input.size() * 2); // кириллица — 2 байта в utf-8

    for (const char raw : input)
    {
        const uint32_t cp = table[static_cast<unsigned char>(raw)];
        if (cp <= 0x7F)
        {
            output += static_cast<char>(cp);
        }
        else if (cp <= 0x7FF)
        {
            output += static_cast<char>(0xC0 | (cp >> 6));
            output += static_cast<char>(0x80 | (cp & 0x3F));
        }
        else
        {
            output += static_cast<char>(0xE0 | (cp >> 12));
            output += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            output += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    return output;
}

std::string Encoding::utf8Tocp1251(std::string_view input)
{
    std::string output;
    output.resize(input.size());

    const auto *data = reinterpret_cast<const unsigned char *>(input.data());
    const size_t size = input.size();

    const auto &utf8_first = getUtf8First();
    const auto &lut = getCp1251Lut();

    size_t i = 0;
    size_t o = 0;

    while (i < size)
    {
        const unsigned char b = data[i];
        const auto &info = utf8_first[b];

        if (info.add_bytes < 0)
        {
            output[o++] = '?';
            ++i;
            continue;
        }

        const int len = info.add_bytes + 1;
        if (i + len > size)
        {
            output[o++] = '?';
            ++i;
            continue;
        }

        uint32_t cp = b & info.mask;
        bool valid = true;

        for (int j = 1; j < len; ++j)
        {
            const unsigned char cb = data[i + j];
            if ((cb & 0xC0) != 0x80)
            {
                valid = false;
                break;
            }
            cp = (cp << 6) | (cb & 0x3F);
        }

        if (!valid || cp < info.min_val || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
        {
            output[o++] = '?';
            i += len;
            continue;
        }

        if (cp <= 0x7F)
        {
            output[o++] = static_cast<char>(cp);
        }
        else if (cp <= 0xFFFF)
        {
            output[o++] = static_cast<char>(lut[cp]);
        }
        else
        {
            output[o++] = '?';
        }

        i += len;
    }

    output.resize(o);
    return output;
}

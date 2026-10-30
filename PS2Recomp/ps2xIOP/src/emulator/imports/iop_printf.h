#pragma once

#include "../core/iop_memory.h"

#include <cstdint>
#include <cstdio>
#include <string>

namespace ps2x::iop::detail
{
    // printf-style formatting for IOP guest calls (sprintf, vsprintf, printf, Kprintf ...). The IOP is a 32-bit
    // MIPS (o32): every vararg is one 32-bit word (`nextArg` returns them in order). Floating point conversions are
    // not supported (the IOP has no FPU and its libc has none), %ll takes two words (low, high).
    template <typename NextArg>
    [[nodiscard]] std::string formatIopPrintf(const IopMemory &memory, uint32_t formatAddress, NextArg &&nextArg)
    {
        std::string out;
        const std::string format = memory.readString(formatAddress, 4096u);
        for (size_t i = 0; i < format.size(); ++i)
        {
            const char ch = format[i];
            if (ch != '%')
            {
                out.push_back(ch);
                continue;
            }
            if (++i >= format.size())
                break;

            bool left = false, plus = false, space = false, alt = false, zero = false;
            for (; i < format.size(); ++i)
            {
                const char flag = format[i];
                if (flag == '-')
                    left = true;
                else if (flag == '+')
                    plus = true;
                else if (flag == ' ')
                    space = true;
                else if (flag == '#')
                    alt = true;
                else if (flag == '0')
                    zero = true;
                else
                    break;
            }
            int width = 0;
            if (i < format.size() && format[i] == '*')
            {
                width = static_cast<int32_t>(nextArg());
                if (width < 0)
                {
                    left = true;
                    width = -width;
                }
                ++i;
            }
            else
            {
                while (i < format.size() && format[i] >= '0' && format[i] <= '9')
                    width = width * 10 + (format[i++] - '0');
            }
            int precision = -1;
            if (i < format.size() && format[i] == '.')
            {
                ++i;
                precision = 0;
                if (i < format.size() && format[i] == '*')
                {
                    precision = static_cast<int32_t>(nextArg());
                    ++i;
                }
                else
                {
                    while (i < format.size() && format[i] >= '0' && format[i] <= '9')
                        precision = precision * 10 + (format[i++] - '0');
                }
            }
            int longs = 0;
            bool shortArg = false, charArg = false;
            for (; i < format.size(); ++i)
            {
                const char length = format[i];
                if (length == 'l')
                    ++longs;
                else if (length == 'h')
                {
                    if (shortArg)
                        charArg = true;
                    shortArg = true;
                }
                else if (length != 'z' && length != 't' && length != 'j')
                    break;
            }
            if (i >= format.size())
                break;

            const char conversion = format[i];
            std::string body;
            std::string prefix;
            bool numeric = false;
            switch (conversion)
            {
            case '%':
                out.push_back('%');
                continue;
            case 'c':
                body.push_back(static_cast<char>(nextArg() & 0xFFu));
                break;
            case 's':
            {
                const uint32_t pointer = nextArg();
                body = pointer == 0u ? std::string("(null)") : memory.readString(pointer, 4096u);
                if (precision >= 0 && static_cast<size_t>(precision) < body.size())
                    body.resize(static_cast<size_t>(precision));
                break;
            }
            case 'd':
            case 'i':
            {
                int64_t value = static_cast<int32_t>(nextArg());
                if (longs >= 2)
                    value = static_cast<int64_t>((static_cast<uint64_t>(nextArg()) << 32u) | static_cast<uint32_t>(value));
                else if (charArg)
                    value = static_cast<int8_t>(value);
                else if (shortArg)
                    value = static_cast<int16_t>(value);
                numeric = true;
                const bool negative = value < 0;
                const uint64_t magnitude = negative ? 0u - static_cast<uint64_t>(value) : static_cast<uint64_t>(value);
                body = std::to_string(magnitude);
                if (negative)
                    prefix = "-";
                else if (plus)
                    prefix = "+";
                else if (space)
                    prefix = " ";
                break;
            }
            case 'u':
            case 'x':
            case 'X':
            case 'o':
            case 'p':
            {
                uint64_t value = nextArg();
                if (conversion == 'p')
                {
                    alt = true;
                }
                else if (longs >= 2)
                    value |= static_cast<uint64_t>(nextArg()) << 32u;
                else if (charArg)
                    value &= 0xFFu;
                else if (shortArg)
                    value &= 0xFFFFu;
                numeric = true;
                char digits[32];
                if (conversion == 'u')
                    std::snprintf(digits, sizeof(digits), "%llu", static_cast<unsigned long long>(value));
                else if (conversion == 'o')
                    std::snprintf(digits, sizeof(digits), "%llo", static_cast<unsigned long long>(value));
                else if (conversion == 'X')
                    std::snprintf(digits, sizeof(digits), "%llX", static_cast<unsigned long long>(value));
                else
                    std::snprintf(digits, sizeof(digits), "%llx", static_cast<unsigned long long>(value));
                body = digits;
                if (alt && value != 0u)
                {
                    if (conversion == 'x' || conversion == 'p')
                        prefix = "0x";
                    else if (conversion == 'X')
                        prefix = "0X";
                    else if (conversion == 'o')
                        prefix = "0";
                }
                break;
            }
            default: // unsupported conversion: keep it visible
                out.push_back('%');
                out.push_back(conversion);
                continue;
            }

            if (numeric && precision >= 0)
            {
                if (static_cast<int>(body.size()) < precision)
                    body.insert(0, static_cast<size_t>(precision) - body.size(), '0');
                zero = false;
            }
            const int length = static_cast<int>(prefix.size() + body.size());
            const int padding = width > length ? width - length : 0;
            if (left)
                out += prefix + body + std::string(static_cast<size_t>(padding), ' ');
            else if (zero && numeric)
                out += prefix + std::string(static_cast<size_t>(padding), '0') + body;
            else
                out += std::string(static_cast<size_t>(padding), ' ') + prefix + body;
        }
        return out;
    }

    // o32 varargs of a call: argument `index` (a0 = 0) lives in a0..a3 (index < 4) or on the stack at sp + 4 * index.
    struct IopCallVarArgs
    {
        const IopMemory &memory;
        const uint32_t *gpr;
        uint32_t index;

        uint32_t operator()()
        {
            const uint32_t k = index++;
            return k < 4u ? gpr[4u + k] : memory.read32(gpr[29] + 4u * k);
        }
    };

    // vsprintf-style va_list: a pointer to consecutive 32-bit argument words.
    struct IopVaList
    {
        const IopMemory &memory;
        uint32_t pointer;

        uint32_t operator()()
        {
            const uint32_t value = memory.read32(pointer);
            pointer += 4u;
            return value;
        }
    };
}

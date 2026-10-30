#pragma once
// Save-state byte streams shared by the EE runtime and the IOP library (format: see ps2xRuntime/src/lib/ps2_savestate.h).

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

namespace ps2_state
{
    constexpr uint32_t kMagic = 0x54535850u;   // "PXST"
    constexpr uint32_t kFormatVersion = 1u;

    constexpr uint32_t tag(const char (&s)[5])
    {
        return static_cast<uint32_t>(static_cast<uint8_t>(s[0])) | (static_cast<uint32_t>(static_cast<uint8_t>(s[1])) << 8) |
               (static_cast<uint32_t>(static_cast<uint8_t>(s[2])) << 16) | (static_cast<uint32_t>(static_cast<uint8_t>(s[3])) << 24);
    }

    class Writer
    {
    public:
        void bytes(const void *data, size_t size)
        {
            const auto *p = static_cast<const uint8_t *>(data);
            m_data.insert(m_data.end(), p, p + size);
        }
        template <typename T>
        void pod(const T &value)
        {
            static_assert(std::is_trivially_copyable_v<T>, "pod() needs a trivially copyable type");
            bytes(&value, sizeof(T));
        }
        void u8(uint8_t v) { pod(v); }
        void u32(uint32_t v) { pod(v); }
        void u64(uint64_t v) { pod(v); }
        void i32(int32_t v) { pod(v); }
        void boolean(bool v) { u8(v ? 1u : 0u); }
        void str(const std::string &s)
        {
            u32(static_cast<uint32_t>(s.size()));
            bytes(s.data(), s.size());
        }
        template <typename T>
        void podVector(const std::vector<T> &v)
        {
            static_assert(std::is_trivially_copyable_v<T>, "podVector() needs a trivially copyable element");
            u64(v.size());
            if (!v.empty())
                bytes(v.data(), v.size() * sizeof(T));
        }
        void blob(const void *data, size_t size)
        {
            u64(size);
            bytes(data, size);
        }
        // Map whose keys and values are trivially copyable (std::map / unordered_map).
        template <typename Map>
        void podMap(const Map &map)
        {
            u64(map.size());
            for (const auto &[key, value] : map)
            {
                pod(key);
                pod(value);
            }
        }

        void beginSection(uint32_t sectionTag, uint32_t version)
        {
            u32(sectionTag);
            u32(version);
            m_sectionSizeAt = m_data.size();
            u64(0u);
        }
        void endSection()
        {
            const uint64_t size = m_data.size() - m_sectionSizeAt - sizeof(uint64_t);
            std::memcpy(m_data.data() + m_sectionSizeAt, &size, sizeof(size));
        }

        std::vector<uint8_t> &data() { return m_data; }

    private:
        std::vector<uint8_t> m_data;
        size_t m_sectionSizeAt = 0;
    };

    class Reader
    {
    public:
        Reader(const uint8_t *data, size_t size) : m_data(data), m_size(size) {}

        bool ok() const { return m_ok; }
        const std::string &error() const { return m_error; }
        void fail(const std::string &why)
        {
            if (m_ok)
                m_error = why;
            m_ok = false;
        }

        void bytes(void *out, size_t size)
        {
            if (!m_ok || size > m_size - m_pos)
            {
                fail("state file is truncated");
                std::memset(out, 0, size);
                return;
            }
            std::memcpy(out, m_data + m_pos, size);
            m_pos += size;
        }
        template <typename T>
        void pod(T &value)
        {
            static_assert(std::is_trivially_copyable_v<T>, "pod() needs a trivially copyable type");
            bytes(&value, sizeof(T));
        }
        template <typename T>
        T get()
        {
            T v{};
            pod(v);
            return v;
        }
        uint8_t u8() { return get<uint8_t>(); }
        uint32_t u32() { return get<uint32_t>(); }
        uint64_t u64() { return get<uint64_t>(); }
        int32_t i32() { return get<int32_t>(); }
        bool boolean() { return u8() != 0u; }
        std::string str()
        {
            const uint32_t n = u32();
            if (!m_ok || n > m_size - m_pos)
            {
                fail("state file is truncated (string)");
                return {};
            }
            std::string s(reinterpret_cast<const char *>(m_data + m_pos), n);
            m_pos += n;
            return s;
        }
        template <typename T>
        void podVector(std::vector<T> &v)
        {
            static_assert(std::is_trivially_copyable_v<T>, "podVector() needs a trivially copyable element");
            const uint64_t n = u64();
            if (!m_ok || n > (m_size - m_pos) / (sizeof(T) ? sizeof(T) : 1u))
            {
                fail("state file is truncated (vector)");
                v.clear();
                return;
            }
            v.resize(static_cast<size_t>(n));
            if (n != 0u)
                bytes(v.data(), static_cast<size_t>(n) * sizeof(T));
        }
        // A blob whose size must match the destination exactly.
        void blob(void *out, size_t size)
        {
            const uint64_t n = u64();
            if (n != size)
            {
                fail("state blob size mismatch (" + std::to_string(n) + " vs " + std::to_string(size) + ")");
                return;
            }
            bytes(out, size);
        }

        template <typename Map>
        void podMap(Map &map)
        {
            map.clear();
            const uint64_t n = u64();
            for (uint64_t i = 0; i < n && m_ok; ++i)
            {
                typename Map::key_type key{};
                typename Map::mapped_type value{};
                pod(key);
                pod(value);
                map.emplace(key, value);
            }
        }

        // Returns the section version, or 0 (and fails) when the next section is not `sectionTag`.
        uint32_t beginSection(uint32_t sectionTag)
        {
            const uint32_t t = u32();
            const uint32_t version = u32();
            const uint64_t size = u64();
            if (!m_ok)
                return 0u;
            if (t != sectionTag)
            {
                char want[5] = {}, got[5] = {};
                std::memcpy(want, &sectionTag, 4);
                std::memcpy(got, &t, 4);
                fail(std::string("expected section '") + want + "', found '" + got + "'");
                return 0u;
            }
            if (size > m_size - m_pos)
            {
                fail(std::string("section '") + reinterpret_cast<const char *>(&t) + "' is truncated");
                return 0u;
            }
            m_sectionEnd = m_pos + static_cast<size_t>(size);
            return version;
        }
        void endSection(const char *name)
        {
            if (m_ok && m_pos != m_sectionEnd)
                fail(std::string("section '") + name + "' size mismatch: read " + std::to_string(m_pos) + " of " +
                     std::to_string(m_sectionEnd));
        }

    private:
        const uint8_t *m_data;
        size_t m_size;
        size_t m_pos = 0;
        size_t m_sectionEnd = 0;
        bool m_ok = true;
        std::string m_error;
    };
}

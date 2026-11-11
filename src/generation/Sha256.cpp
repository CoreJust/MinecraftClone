#include "Sha256.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <span>

namespace generation::detail
{
namespace
{

// Adapted from CoreProject2026/src/lang/compiler/Bytecode_001.inc at
// commit 69e3e1b1ff5b4f4ade4808f88e1d6614ceb17d55.
// The standalone generation target keeps SHA-256 local instead of linking CoreLang.
class Sha256State final
{
public:
    [[nodiscard]]
    static std::array<uint8_t, 32U> hash(std::span<uint8_t const> bytes)
    {
        Sha256State state;
        state.update(bytes);
        return state.finish();
    }

private:
    void update(std::span<uint8_t const> bytes)
    {
        for (uint8_t const value : bytes)
        {
            m_block[static_cast<std::array<uint8_t, 64U>::size_type>(m_used)] = value;
            ++m_used;
            if (m_used == 64U)
            {
                transform();
                m_bit_count += 512U;
                m_used = 0U;
            }
        }
    }

    [[nodiscard]]
    std::array<uint8_t, 32U> finish()
    {
        m_bit_count += m_used * 8U;
        m_block[static_cast<std::array<uint8_t, 64U>::size_type>(m_used)] = 0x80U;
        ++m_used;
        if (m_used > 56U)
        {
            while (m_used < 64U)
            {
                m_block[static_cast<std::array<uint8_t, 64U>::size_type>(m_used)] = 0U;
                ++m_used;
            }
            transform();
            m_used = 0U;
        }
        while (m_used < 56U)
        {
            m_block[static_cast<std::array<uint8_t, 64U>::size_type>(m_used)] = 0U;
            ++m_used;
        }
        for (uint64_t index = 0U; index < 8U; ++index)
        {
            uint64_t const shift = 56U - index * 8U;
            m_block[static_cast<std::array<uint8_t, 64U>::size_type>(56U + index)] =
                static_cast<uint8_t>(m_bit_count >> shift);
        }
        m_used = 64U;
        transform();

        std::array<uint8_t, 32U> result{};
        for (uint64_t index = 0U; index < 8U; ++index)
        {
            for (uint64_t byte = 0U; byte < 4U; ++byte)
            {
                uint64_t const shift = (3U - byte) * 8U;
                result[static_cast<std::array<uint8_t, 32U>::size_type>(index * 4U + byte)] =
                    static_cast<uint8_t>(
                        m_state[static_cast<std::array<uint32_t, 8U>::size_type>(index)] >> shift
                    );
            }
        }
        return result;
    }

    void transform()
    {
        static constexpr std::array<uint32_t, 64U> ROUND_CONSTANTS{
            0x428a'2f98U, 0x7137'4491U, 0xb5c0'fbcfU, 0xe9b5'dba5U, 0x3956'c25bU, 0x59f1'11f1U, 0x923f'82a4U,
            0xab1c'5ed5U, 0xd807'aa98U, 0x1283'5b01U, 0x2431'85beU, 0x550c'7dc3U, 0x72be'5d74U, 0x80de'b1feU,
            0x9bdc'06a7U, 0xc19b'f174U, 0xe49b'69c1U, 0xefbe'4786U, 0x0fc1'9dc6U, 0x240c'a1ccU, 0x2de9'2c6fU,
            0x4a74'84aaU, 0x5cb0'a9dcU, 0x76f9'88daU, 0x983e'5152U, 0xa831'c66dU, 0xb003'27c8U, 0xbf59'7fc7U,
            0xc6e0'0bf3U, 0xd5a7'9147U, 0x06ca'6351U, 0x1429'2967U, 0x27b7'0a85U, 0x2e1b'2138U, 0x4d2c'6dfcU,
            0x5338'0d13U, 0x650a'7354U, 0x766a'0abbU, 0x81c2'c92eU, 0x9272'2c85U, 0xa2bf'e8a1U, 0xa81a'664bU,
            0xc24b'8b70U, 0xc76c'51a3U, 0xd192'e819U, 0xd699'0624U, 0xf40e'3585U, 0x106a'a070U, 0x19a4'c116U,
            0x1e37'6c08U, 0x2748'774cU, 0x34b0'bcb5U, 0x391c'0cb3U, 0x4ed8'aa4aU, 0x5b9c'ca4fU, 0x682e'6ff3U,
            0x748f'82eeU, 0x78a5'636fU, 0x84c8'7814U, 0x8cc7'0208U, 0x90be'fffaU, 0xa450'6cebU, 0xbef9'a3f7U,
            0xc671'78f2U,
        };

        std::array<uint32_t, 64U> words{};
        for (uint64_t index = 0U; index < 16U; ++index)
        {
            uint64_t const offset = index * 4U;
            words[static_cast<std::array<uint32_t, 64U>::size_type>(index)] =
                (static_cast<uint32_t>(m_block[static_cast<std::array<uint8_t, 64U>::size_type>(offset)]) << 24U)
                | (static_cast<uint32_t>(m_block[static_cast<std::array<uint8_t, 64U>::size_type>(offset + 1U)]) << 16U)
                | (static_cast<uint32_t>(m_block[static_cast<std::array<uint8_t, 64U>::size_type>(offset + 2U)]) << 8U)
                | static_cast<uint32_t>(m_block[static_cast<std::array<uint8_t, 64U>::size_type>(offset + 3U)]);
        }
        for (uint64_t index = 16U; index < words.size(); ++index)
        {
            uint32_t const a = words[static_cast<std::array<uint32_t, 64U>::size_type>(index - 15U)];
            uint32_t const b = words[static_cast<std::array<uint32_t, 64U>::size_type>(index - 2U)];
            words[static_cast<std::array<uint32_t, 64U>::size_type>(index)] =
                words[static_cast<std::array<uint32_t, 64U>::size_type>(index - 16U)]
                + (std::rotr(a, 7) ^ std::rotr(a, 18) ^ (a >> 3U))
                + words[static_cast<std::array<uint32_t, 64U>::size_type>(index - 7U)]
                + (std::rotr(b, 17) ^ std::rotr(b, 19) ^ (b >> 10U));
        }

        uint32_t a = m_state[0U];
        uint32_t b = m_state[1U];
        uint32_t c = m_state[2U];
        uint32_t d = m_state[3U];
        uint32_t e = m_state[4U];
        uint32_t f = m_state[5U];
        uint32_t g = m_state[6U];
        uint32_t h = m_state[7U];
        for (uint64_t index = 0U; index < words.size(); ++index)
        {
            uint32_t const first = h + (std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25))
                + ((e & f) ^ (~e & g))
                + ROUND_CONSTANTS[static_cast<std::array<uint32_t, 64U>::size_type>(index)]
                + words[static_cast<std::array<uint32_t, 64U>::size_type>(index)];
            uint32_t const second = (std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22))
                + ((a & b) ^ (a & c) ^ (b & c));
            h = g;
            g = f;
            f = e;
            e = d + first;
            d = c;
            c = b;
            b = a;
            a = first + second;
        }
        m_state[0U] += a;
        m_state[1U] += b;
        m_state[2U] += c;
        m_state[3U] += d;
        m_state[4U] += e;
        m_state[5U] += f;
        m_state[6U] += g;
        m_state[7U] += h;
    }

    std::array<uint32_t, 8U> m_state{
        0x6a09'e667U, 0xbb67'ae85U, 0x3c6e'f372U, 0xa54f'f53aU,
        0x510e'527fU, 0x9b05'688cU, 0x1f83'd9abU, 0x5be0'cd19U,
    };
    std::array<uint8_t, 64U> m_block{};
    uint64_t m_bit_count = 0U;
    uint64_t m_used = 0U;
};

} // namespace

std::array<uint8_t, 32U> sha256Digest(std::span<uint8_t const> bytes)
{
    return Sha256State::hash(bytes);
}

} // namespace generation::detail

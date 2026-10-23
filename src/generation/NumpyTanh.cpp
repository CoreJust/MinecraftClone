// NumPy 2.5.4 tanh coefficients and Horner-FMA evaluation adapted from
// numpy/_core/src/umath/loops_hyperbolic.dispatch.cpp.src. Explicit binary64
// fused operations preserve its rounding path without using platform libm.
//
// Copyright (c) 2005-2025, NumPy Developers.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice,
//   this list of conditions and the following disclaimer.
// * Redistributions in binary form must reproduce the above copyright notice,
//   this list of conditions and the following disclaimer in the documentation
//   and/or other materials provided with the distribution.
// * Neither the name of the NumPy Developers nor the names of any contributors
//   may be used to endorse or promote products derived from this software
//   without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#include "Numerics.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>

namespace generation::detail
{
namespace
{

constexpr std::array<std::array<uint64_t, 18U>, 16U> TANH_LUT{{
    // interval 0
    {
        0x0ULL, 0x0ULL, 0x3ff0'0000'0000'0000ULL, 0xbbf0'b3ea'3fdf'aa19ULL,
        0xbfd5'5555'5555'5555ULL, 0xbce6'863e'e44e'd636ULL, 0x3fc1'1111'1111'2ab5ULL, 0xbda1'ea19'dddd'b3b4ULL,
        0xbfab'a1ba'1990'520bULL, 0xbe35'1ca7'f096'011fULL, 0x3f96'64f9'4e6a'c14eULL, 0xbea8'c4c1'fd78'52feULL,
        0xbf82'2404'577a'a9ddULL, 0xbefd'd99a'221e'd573ULL, 0x3f6e'3be6'8942'3841ULL, 0xbf2a'1306'713a'4f3aULL,
        0xbf55'd7e7'6dc5'6871ULL, 0x3f35'e67a'b76a'26e7ULL,
    },
    // interval 1
    {
        0x3fcc'0000'0000'0000ULL, 0x3fcb'8fd0'416a'7c92ULL, 0x3fee'842c'a3f0'8532ULL, 0xbfca'48aa'eb53'bc21ULL,
        0xbfd1'83af'c292'ba11ULL, 0x3fc0'4dcd'0476'c75eULL, 0x3fb5'c19e'fdfc'08adULL, 0xbfb0'b8df'995c'e4dfULL,
        0xbf96'e37b'ba52'f6fcULL, 0x3f9e'aaf3'320c'3851ULL, 0xbf94'd334'3bae'39ddULL, 0xbfcc'ce16'b104'6f13ULL,
        0x403d'8b07'f7a8'2aa3ULL, 0x4070'593a'3735'bab4ULL, 0xc0d2'6351'1f5b'aac1ULL, 0xc104'5e50'9116'b066ULL,
        0x4152'8c38'809c'90c7ULL, 0x4184'8ee0'627d'8206ULL,
    },
    // interval 2
    {
        0x3fd4'0000'0000'0000ULL, 0x3fd3'5f98'a0ea'650eULL, 0x3fed'1157'4af5'8f1bULL, 0xbfd1'9921'f432'9916ULL,
        0xbfcc'1a4b'039c'9bfaULL, 0x3fc4'3d34'49a8'0f08ULL, 0x3fa7'4c98'dc34'fbacULL, 0xbfb2'955c'f41e'8164ULL,
        0x3ecf'f7df'1845'5399ULL, 0x3f9c'f823'fe76'1fc1ULL, 0xbf7b'c748'e60d'f843ULL, 0xbf81'a16f'224b'b7b6ULL,
        0xbf9f'44ab'92fb'ab0aULL, 0xbfcc'ab65'4e44'835eULL, 0x4016'9f73'b15e'be5cULL, 0x4041'fab9'2509'84ceULL,
        0xc076'd57f'b519'0b02ULL, 0xc0a2'16d6'18b4'89ecULL,
    },
    // interval 3
    {
        0x3fdc'0000'0000'0000ULL, 0x3fda'5729'ee48'8037ULL, 0x3fea'945b'9c24'e4f9ULL, 0xbfd5'e0f0'9bef'8011ULL,
        0xbfc1'6e1e'6d8d'0be6ULL, 0x3fc5'c26f'3699'b7e7ULL, 0xbf79'0d6a'8eff'0a77ULL, 0xbfaf'9d05'c309'f7c6ULL,
        0x3f97'3628'34d3'3a4eULL, 0x3f90'2227'1754'ff1fULL, 0xbf8c'8937'2b43'ba85ULL, 0xbf62'cbf0'0406'bc09ULL,
        0x3fb2'eac6'0447'3d6aULL, 0x3fd1'3ed8'0037'dbacULL, 0xc025'c1dd'41cd'6cb5ULL, 0xc045'8d09'0ec3'de95ULL,
        0x4085'f09f'888f'8adaULL, 0x40a5'b891'07c8'af4fULL,
    },
    // interval 4
    {
        0x3fe4'0000'0000'0000ULL, 0x3fe1'bf47'eabb'8f95ULL, 0x3fe6'284c'3374'f815ULL, 0xbfd8'93b5'9c35'c882ULL,
        0xbf92'426c'751e'48a2ULL, 0x3fc1'a686'f6ab'2533ULL, 0xbfac'3c02'1789'a786ULL, 0xbf98'7d27'ccff'4291ULL,
        0x3f9e'7f83'8018'4b45ULL, 0xbf73'1fe7'7c9c'60afULL, 0xbf81'29a0'92de'747aULL, 0x3f75'b29b'b02c'f69bULL,
        0x3f45'f87d'903a'aac8ULL, 0xbf60'45b9'076c'c487ULL, 0xbf58'fd89'fe05'e0d1ULL, 0xbf74'949d'6011'3d63ULL,
        0x3fa2'4633'2a2f'cba5ULL, 0x3fb6'9d83'7452'0edaULL,
    },
    // interval 5
    {
        0x3fec'0000'0000'0000ULL, 0x3fe6'8665'0b8c'2015ULL, 0x3fe0'2500'a09f'8d6eULL, 0xbfd6'ba7c'b757'6538ULL,
        0x3fb4'f152'b2ba'd124ULL, 0x3faf'203c'316c'e730ULL, 0xbfae'2196'b732'6859ULL, 0x3f8b'2ca6'2572'b098ULL,
        0x3f86'9543'e7c4'20d4ULL, 0xbf84'a604'6865'ec7dULL, 0x3f60'c85b'4d53'8746ULL, 0x3f60'7df0'f9f9'0c17ULL,
        0xbf5e'1046'7103'6300ULL, 0x3f20'85ee'7e8a'c170ULL, 0x3f73'f7af'01d5'af7aULL, 0x3f7c'9fd6'200d'0adeULL,
        0xbfb2'9d85'1a89'6fcdULL, 0xbfbd'ed51'9f98'1716ULL,
    },
    // interval 6
    {
        0x3ff4'0000'0000'0000ULL, 0x3feb'2523'bb6b'2deeULL, 0x3fd1'f251'31e3'a8c0ULL, 0xbfce'7291'743d'7555ULL,
        0x3fbb'ba40'cbef'72beULL, 0xbf89'c7a0'2788'557cULL, 0xbf93'a7a0'11ff'8c2aULL, 0x3f8f'1cf6'c7f5'b00aULL,
        0xbf73'26bd'4914'222aULL, 0xbf4c'a3f1'f2b9'192bULL, 0x3f5b'e939'2199'ec18ULL, 0xbf4b'852a'6e07'58d5ULL,
        0x3f19'bc98'ddf0'f340ULL, 0x3f23'5246'2261'0430ULL, 0xbf1e'40bd'ead1'7e6bULL, 0x3f02'cd40'e0ad'0a9fULL,
        0x3ed9'065a'e369'b212ULL, 0xbef0'2d28'8b5b'3371ULL,
    },
    // interval 7
    {
        0x3ffc'0000'0000'0000ULL, 0x3fee'1fbf'97e3'3527ULL, 0x3fbd'22ca'1c24'a139ULL, 0xbfbb'6d85'a01e'fb80ULL,
        0x3fb0'1ba0'38be'6a3dULL, 0xbf98'157e'26e0'd541ULL, 0x3f6e'4709'c7e8'430eULL, 0x3f60'3798'11e4'3dd5ULL,
        0xbf5f'c15b'0a9d'98faULL, 0x3f4c'77de'e0af'd227ULL, 0xbf2a'0c68'a448'9f10ULL, 0xbf00'78c6'3d1b'8445ULL,
        0x3f0d'4304'bc92'46e8ULL, 0xbeff'12a6'6269'11b4ULL, 0x3ee2'24cd'6c45'13e5ULL, 0xbe85'8ab8'e019'f311ULL,
        0xbeb8'e1ba'4c98'a030ULL, 0x3eb2'9098'1209'c1a6ULL,
    },
    // interval 8
    {
        0x4004'0000'0000'0000ULL, 0x3fef'9258'260a'71c2ULL, 0x3f9b'3afe'1fba'5c76ULL, 0xbf9a'ddae'58c7'141aULL,
        0x3f91'6df4'4871'efc8ULL, 0xbf80'7b55'c1c7'd278ULL, 0x3f67'682a'fa61'1151ULL, 0xbf47'9382'6f78'537eULL,
        0x3f14'cffc'fa69'fbb6ULL, 0x3f04'055b'ce68'597aULL, 0xbf00'4626'01dc'2faaULL, 0x3eec'12ea'dd55'be7aULL,
        0xbed1'3c41'5f7b'9d41ULL, 0x3eab'9008'bca4'08afULL, 0xbe24'b645'e68e'eaa3ULL, 0xbe79'2fa6'323b'7cf8ULL,
        0x3e6f'fd07'66ad'4016ULL, 0xbe56'7e92'4bf5'ff6eULL,
    },
    // interval 9
    {
        0x400c'0000'0000'0000ULL, 0x3fef'f112'c63a'9077ULL, 0x3f6d'd37d'19b2'2b21ULL, 0xbf6d'c593'76c7'aa19ULL,
        0x3f63'c686'9dfc'8870ULL, 0xbf53'a18d'5843'190fULL, 0x3f3e'f2ee'7771'7cbfULL, 0xbf24'0569'5e36'240fULL,
        0x3f05'7e48'e5b7'9d10ULL, 0xbee2'bf0c'b4a7'1647ULL, 0x3eb7'b6a2'19de'a9f4ULL, 0xbe6f'a600'f593'181bULL,
        0xbe72'2b8d'9720'cdb0ULL, 0x3e63'4df7'1865'f620ULL, 0xbe4a'bfeb'fb72'bc83ULL, 0x3e2d'f04d'6787'6402ULL,
        0xbe0c'63c2'9f50'5f5bULL, 0x3de3'f7f7'de6b'0eb6ULL,
    },
    // interval 10
    {
        0x4014'0000'0000'0000ULL, 0x3fef'ff41'9668'df11ULL, 0x3f27'ccec'13a9'ef96ULL, 0xbf27'cc5e'7467'7410ULL,
        0x3f1f'b9ae'f915'd828ULL, 0xbf0f'b6bb'c89b'1a5bULL, 0x3ef9'5a44'82f1'80b7ULL, 0xbee0'e08d'e39c'e756ULL,
        0x3ec3'3b66'd7d7'7264ULL, 0xbea3'1eaa'fe73'efd5ULL, 0x3e80'cbcc'8d4c'5c8aULL, 0xbe5a'3c93'5dce'3f7dULL,
        0x3e32'2666'd739'bec0ULL, 0xbe05'bb1b'cf83'ca73ULL, 0x3dd5'1c38'f869'5ed3ULL, 0xbd95'c72b'e95e'4d2cULL,
        0xbd7f'ab21'6b9e'0e49ULL, 0x3d69'ed18'bae3'ebbcULL,
    },
    // interval 11
    {
        0x401c'0000'0000'0000ULL, 0x3fef'fffc'8327'50f2ULL, 0x3ecb'e6c3'f332'50aeULL, 0xbecb'e6c0'e8b4'cc87ULL,
        0x3ec2'99d1'e27c'6e11ULL, 0xbeb2'99c9'c684'a963ULL, 0x3e9d'c2c2'7da3'b603ULL, 0xbe83'd709'ba5f'714eULL,
        0x3e66'ac4e'578b'9b10ULL, 0xbe46'abb0'2c43'68edULL, 0x3e24'25bb'231a'5e29ULL, 0xbe00'1c6d'95e3'ae96ULL,
        0x3dd7'6a55'3d7e'7918ULL, 0xbdaf'2ac1'43fb'6762ULL, 0x3d83'13ac'38c6'832bULL, 0xbd55'a89c'3020'3106ULL,
        0x3d28'26b6'2056'aa27ULL, 0xbcf7'534c'4f3d'fa71ULL,
    },
    // interval 12
    {
        0x4024'0000'0000'0000ULL, 0x3fef'ffff'fdc9'6f35ULL, 0x3e41'b486'5394'f75fULL, 0xbe41'b486'526b'0565ULL,
        0x3e37'9b5d'dcca'334cULL, 0xbe27'9b5d'd4fb'3d01ULL, 0x3e12'e2af'd9f7'433eULL, 0xbdf9'2e3f'c5ee'63e0ULL,
        0x3ddc'c74b'8d3d'5c42ULL, 0xbdbc'c749'ca80'79ddULL, 0x3d99'92a4'beac'8662ULL, 0xbd74'755a'00ea'1fd3ULL,
        0x3d4d'e0fa'5941'6a39ULL, 0xbd23'eae5'2a3d'bf57ULL, 0x3cf7'7879'3562'6685ULL, 0xbcca'd6b3'bb9e'ff65ULL,
        0x3ca3'13e3'1762'f523ULL, 0xbc73'0b73'f1ea'ff20ULL,
    },
    // interval 13
    {
        0x402c'0000'0000'0000ULL, 0x3fef'ffff'ffff'cf58ULL, 0x3d88'53f0'1bda'5f28ULL, 0xbd88'53f0'1bef'63a4ULL,
        0x3d80'37f5'7bc6'2c9aULL, 0xbd70'37f5'7ae7'2aa6ULL, 0x3d59'f320'3486'79baULL, 0xbd41'4cc0'30f2'110eULL,
        0x3d23'c589'137f'92b4ULL, 0xbd03'c588'3836'b9d2ULL, 0x3ce1'91ba'5ed3'fb67ULL, 0xbcbc'1c6c'063b'b7acULL,
        0x3c94'8716'cf36'81b4ULL, 0xbc6b'5e3e'9ca0'955eULL, 0x3c40'1ffc'49c6'bc29ULL, 0xbc12'705c'cd3d'd884ULL,
        0x3bea'37aa'2189'5319ULL, 0xbbba'2cff'8135'd462ULL,
    },
    // interval 14
    {
        0x4034'0000'0000'0000ULL, 0x3ff0'0000'0000'0000ULL, 0x3c73'953c'0197'ef58ULL, 0xbc73'955b'e519'be31ULL,
        0x3c6a'2d4b'50a2'cff7ULL, 0xbc5a'2ca2'bba7'8e86ULL, 0x3c44'b61d'9bbc'c940ULL, 0xbc2b'a022'e8d8'2a87ULL,
        0x3c10'7f8e'2c87'07a1ULL, 0xbbf0'7a54'1626'4aecULL, 0x3bc8'9245'0bad'44c4ULL, 0xbba3'be9a'4460'fe00ULL,
        0x3b87'3f9f'2d2f'da99ULL, 0xbb5e'ca68'e2c1'ba2eULL, 0xbabf'0b21'acfa'52abULL, 0xba8e'0a4c'47ae'75f5ULL,
        0x3ae5'c7f1'fd87'1496ULL, 0xbab5'a71b'5f7d'9035ULL,
    },
    // interval 15
    {
        0x0ULL, 0x3ff0'0000'0000'0000ULL, 0x0ULL, 0x0ULL,
        0x0ULL, 0x0ULL, 0x0ULL, 0x0ULL,
        0x0ULL, 0x0ULL, 0x0ULL, 0x0ULL,
        0x0ULL, 0x0ULL, 0x0ULL, 0x0ULL,
        0x0ULL, 0x0ULL,
    }
}};

constexpr std::array<double, 15U> TANH_INTERVAL_LIMITS{
    0.1875,
    0.25,
    0.375,
    0.5,
    0.75,
    1.0,
    1.5,
    2.0,
    3.0,
    4.0,
    6.0,
    8.0,
    12.0,
    16.0,
    24.0,
};

} // namespace

double numpyTanh(double const value)
{
    if (std::isnan(value))
    {
        return std::numeric_limits<double>::quiet_NaN();
    }

    double const magnitude = std::abs(value);
    if (magnitude >= TANH_INTERVAL_LIMITS.back())
    {
        return std::copysign(1.0, value);
    }

    size_t const interval = static_cast<size_t>(
        std::upper_bound(TANH_INTERVAL_LIMITS.begin(), TANH_INTERVAL_LIMITS.end(), magnitude)
        - TANH_INTERVAL_LIMITS.begin()
    );
    std::array<uint64_t, 18U> const& row = TANH_LUT[interval];
    double const y = magnitude - std::bit_cast<double>(row[0U]);
    double result = std::bit_cast<double>(row[17U]);
    for (size_t coefficient = 17U; coefficient > 1U; --coefficient)
    {
        result = std::fma(result, y, std::bit_cast<double>(row[coefficient - 1U]));
    }
    return std::copysign(result, value);
}

} // namespace generation::detail

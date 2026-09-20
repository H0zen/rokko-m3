/**
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * MaNGOS is a full featured server for World of Warcraft, supporting
 * the following clients: 1.12.x, 2.4.3, 3.3.5a, 4.3.4a and 5.4.8
 *
 * Copyright (C) 2005-2026 MaNGOS <https://www.getmangos.eu>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 *
 * World of Warcraft, and all World of Warcraft or Warcraft art, images,
 * and lore are copyrighted by Blizzard Entertainment, Inc.
 */

#ifndef MANGOS_H_CRYPTO_PRIME
#define MANGOS_H_CRYPTO_PRIME

#include "Crypto/BigInt.h"
#include "Crypto/SystemRandom.h"

#include <cstddef>

namespace MaNGOS
{
    namespace Crypto
    {
        /**
         * @brief Primality, for key generation -- run once per realm, offline.
         *
         * Nothing here is on a protocol path: the cost is paid at `mep mint` time,
         * so the tests are generous (64 Miller-Rabin rounds where FIPS asks for 5)
         * and the code is the textbook one.
         */

        /// gcd(a, b) by Euclid. gcd(0, b) = b.
        BigInt Gcd(const BigInt& a, const BigInt& b);

        /// False when a prime below 2^16 divides n (and n is not that prime itself):
        /// the cheap filter that rejects most odd candidates before Miller-Rabin.
        bool PassesSmallPrimeSieve(const BigInt& n);

        /// Miller-Rabin with `rounds` bases drawn uniformly from [2, n - 2] by the
        /// CSPRNG. n must be odd and above 3, rounds at least 1 (CryptoFatal otherwise).
        /// A single witness is final; a survivor of 64 rounds is composite with
        /// probability below 4^-64.
        bool IsProbablePrime(const BigInt& n, unsigned rounds, SystemRandom& random);

        /**
         * @brief A random prime of exactly `bits` bits, fit for an RSA factor.
         *
         * FIPS 186-4 B.3.3 conditions: drawn from [ceil(sqrt(2) * 2^(bits - 1)), 2^bits - 1]
         * (so a product of two such primes has exactly 2 * bits bits), odd, with
         * gcd(p - 1, e) = 1. Candidates step by two under an incremental small-prime
         * sieve; the survivors take Miller-Rabin with `rounds` bases.
         */
        BigInt GeneratePrime(size_t bits, const BigInt& e, SystemRandom& random, unsigned rounds = 64);
    }
}

#endif

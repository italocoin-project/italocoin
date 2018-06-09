// Copyright (c) 2018 The Monero And Italocoin Project
//
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without modification, are
// permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this list of
//    conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice, this list
//    of conditions and the following disclaimer in the documentation and/or other
//    materials provided with the distribution.
//
// 3. Neither the name of the copyright holder nor the names of its contributors may be
//    used to endorse or promote products derived from this software without specific
//    prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
// THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
// STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
// THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// Parts of this file are originally copyright (c) 2012-2013 The Cryptonote developers

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>
#include <boost/math/special_functions/round.hpp>
#include "common/int-util.h"
#include "crypto/hash.h"
#include "cryptonote_config.h"
#include "difficulty.h"

#undef ITALOCOIN_DEFAULT_LOG_CATEGORY
#define ITALOCOIN_DEFAULT_LOG_CATEGORY "difficulty"

namespace cryptonote {

  using std::size_t;
  using std::uint64_t;
  using std::vector;

#if defined(__x86_64__)
  static inline void mul(uint64_t a, uint64_t b, uint64_t &low, uint64_t &high) {
    low = mul128(a, b, &high);
  }

#else

  static inline void mul(uint64_t a, uint64_t b, uint64_t &low, uint64_t &high) {
    // __int128 isn't part of the standard, so the previous function wasn't portable. mul128() in Windows is fine,
    // but this portable function should be used elsewhere. Credit for this function goes to latexi95.

    uint64_t aLow = a & 0xFFFFFFFF;
    uint64_t aHigh = a >> 32;
    uint64_t bLow = b & 0xFFFFFFFF;
    uint64_t bHigh = b >> 32;

    uint64_t res = aLow * bLow;
    uint64_t lowRes1 = res & 0xFFFFFFFF;
    uint64_t carry = res >> 32;

    res = aHigh * bLow + carry;
    uint64_t highResHigh1 = res >> 32;
    uint64_t highResLow1 = res & 0xFFFFFFFF;

    res = aLow * bHigh;
    uint64_t lowRes2 = res & 0xFFFFFFFF;
    carry = res >> 32;

    res = aHigh * bHigh + carry;
    uint64_t highResHigh2 = res >> 32;
    uint64_t highResLow2 = res & 0xFFFFFFFF;

    //Addition

    uint64_t r = highResLow1 + lowRes2;
    carry = r >> 32;
    low = (r << 32) | lowRes1;
    r = highResHigh1 + highResLow2 + carry;
    uint64_t d3 = r & 0xFFFFFFFF;
    carry = r >> 32;
    r = highResHigh2 + carry;
    high = d3 | (r << 32);
  }

#endif

  static inline bool cadd(uint64_t a, uint64_t b) {
    return a + b < a;
  }

  static inline bool cadc(uint64_t a, uint64_t b, bool c) {
    return a + b < a || (c && a + b == (uint64_t) -1);
  }

  bool check_hash(const crypto::hash &hash, difficulty_type difficulty) {
    uint64_t low, high, top, cur;
    // First check the highest word, this will most likely fail for a random hash.
    mul(swap64le(((const uint64_t *) &hash)[3]), difficulty, top, high);
    if (high != 0) {
      return false;
    }
    mul(swap64le(((const uint64_t *) &hash)[0]), difficulty, low, cur);
    mul(swap64le(((const uint64_t *) &hash)[1]), difficulty, low, high);
    bool carry = cadd(cur, low);
    cur = high;
    mul(swap64le(((const uint64_t *) &hash)[2]), difficulty, low, high);
    carry = cadc(cur, low, carry);
    carry = cadc(high, top, carry);
    return !carry;
  }

  difficulty_type next_difficulty(std::vector<std::uint64_t> timestamps, std::vector<difficulty_type> cumulative_difficulties, size_t target_seconds,uint64_t height, uint64_t last_diff_reset_height, difficulty_type last_diff_reset_value) {
	  
	  bool is_diff_reset = false;
    if (last_diff_reset_height != 0 && height >= last_diff_reset_height && height - last_diff_reset_height < timestamps.size())
    {
      is_diff_reset = true;
      const uint64_t num_ignored_blocks = timestamps.size() - (height - last_diff_reset_height);
      timestamps.erase(timestamps.begin(), timestamps.begin() + num_ignored_blocks);
      cumulative_difficulties.erase(cumulative_difficulties.begin(), cumulative_difficulties.begin() + num_ignored_blocks);
    }

    if(timestamps.size() > DIFFICULTY_WINDOW)
    {
      timestamps.resize(DIFFICULTY_WINDOW);
      cumulative_difficulties.resize(DIFFICULTY_WINDOW);
    }


    size_t length = timestamps.size();
    assert(length == cumulative_difficulties.size());
    if (length <= 1) {
		if (is_diff_reset)
      {
        return last_diff_reset_value;
      }
      return 1;
    }
    static_assert(DIFFICULTY_WINDOW >= 2, "Window is too small");
    assert(length <= DIFFICULTY_WINDOW);
    sort(timestamps.begin(), timestamps.end());
    size_t cut_begin, cut_end;
    static_assert(2 * DIFFICULTY_CUT <= DIFFICULTY_WINDOW - 2, "Cut length is too large");
    if (length <= DIFFICULTY_WINDOW - 2 * DIFFICULTY_CUT) {
      cut_begin = 0;
      cut_end = length;
    } else {
      cut_begin = (length - (DIFFICULTY_WINDOW - 2 * DIFFICULTY_CUT) + 1) / 2;
      cut_end = cut_begin + (DIFFICULTY_WINDOW - 2 * DIFFICULTY_CUT);
    }
    assert(/*cut_begin >= 0 &&*/ cut_begin + 2 <= cut_end && cut_end <= length);
    uint64_t time_span = timestamps[cut_end - 1] - timestamps[cut_begin];
    if (time_span == 0) {
      time_span = 1;
    }
    difficulty_type total_work = cumulative_difficulties[cut_end - 1] - cumulative_difficulties[cut_begin];
    assert(total_work > 0);
    uint64_t low, high;
    mul(total_work, target_seconds, low, high);
    // blockchain errors "difficulty overhead" if this function returns zero.
    // TODO: consider throwing an exception instead
    if (high != 0 || low + time_span - 1 < low) {
      return 0;
    }
    return (low + time_span - 1) / time_span;
  }
difficulty_type next_difficulty_v2(std::vector<std::uint64_t> timestamps, std::vector<difficulty_type> cumulative_difficulties, size_t target_seconds, uint64_t height, uint64_t last_diff_reset_height, difficulty_type last_diff_reset_value) {
    bool is_diff_reset = false;
    if (last_diff_reset_height != 0 && height >= last_diff_reset_height && height - last_diff_reset_height < timestamps.size())
    {
      is_diff_reset = true;
      const uint64_t num_ignored_blocks = timestamps.size() - (height - last_diff_reset_height);
      timestamps.erase(timestamps.begin(), timestamps.begin() + num_ignored_blocks);
      cumulative_difficulties.erase(cumulative_difficulties.begin(), cumulative_difficulties.begin() + num_ignored_blocks);
    }
	
    if(timestamps.size() > DIFFICULTY_WINDOW_V1)
    {
      timestamps.resize(DIFFICULTY_WINDOW_V1);
      cumulative_difficulties.resize(DIFFICULTY_WINDOW_V1);
    }


    size_t length = timestamps.size();
    assert(length == cumulative_difficulties.size());
    if (length <= 1) {
		if (is_diff_reset)
      {
        return last_diff_reset_value;
      }
      return 1;
    }
    static_assert(DIFFICULTY_WINDOW_V1 >= 2, "Window is too small");
    assert(length <= DIFFICULTY_WINDOW_V1);
    sort(timestamps.begin(), timestamps.end());
    size_t cut_begin, cut_end;
    static_assert(2 * DIFFICULTY_CUT <= DIFFICULTY_WINDOW_V1 - 2, "Cut length is too large");
    if (length <= DIFFICULTY_WINDOW_V1 - 2 * DIFFICULTY_CUT) {
      cut_begin = 0;
      cut_end = length;
    } else {
      cut_begin = (length - (DIFFICULTY_WINDOW_V1 - 2 * DIFFICULTY_CUT) + 1) / 2;
      cut_end = cut_begin + (DIFFICULTY_WINDOW_V1 - 2 * DIFFICULTY_CUT);
    }
    assert(/*cut_begin >= 0 &&*/ cut_begin + 2 <= cut_end && cut_end <= length);
    uint64_t time_span = timestamps[cut_end - 1] - timestamps[cut_begin];
    if (time_span == 0) {
      time_span = 1;
    }
    difficulty_type total_work = cumulative_difficulties[cut_end - 1] - cumulative_difficulties[cut_begin];
    assert(total_work > 0);
    uint64_t low, high;
    mul(total_work, target_seconds, low, high);
    // blockchain errors "difficulty overhead" if this function returns zero.
    // TODO: consider throwing an exception instead
    if (high != 0 || low + time_span - 1 < low) {
      return 0;
    }
	//return (low + time_span - 1) / time_span;
return (low + time_span - 1) / time_span;
  }
   difficulty_type next_difficulty_v7(std::vector<std::uint64_t> timestamps, std::vector<difficulty_type> cumulative_difficulties, size_t target_seconds, uint64_t height, uint64_t last_diff_reset_height, difficulty_type last_diff_reset_value) {
	   
    bool is_diff_reset = false;
    if (last_diff_reset_height != 0 && height >= last_diff_reset_height && height - last_diff_reset_height < timestamps.size())
    {
      is_diff_reset = true;
      const uint64_t num_ignored_blocks = timestamps.size() - (height - last_diff_reset_height);
      timestamps.erase(timestamps.begin(), timestamps.begin() + num_ignored_blocks);
      cumulative_difficulties.erase(cumulative_difficulties.begin(), cumulative_difficulties.begin() + num_ignored_blocks);
    }
	
    const int64_t T = static_cast<int64_t>(target_seconds);

    size_t N = DIFFICULTY_WINDOW_V7 - 1;

    // Return a difficulty of 1 for first 3 blocks if it's the start of the chain.
    if (timestamps.size() < 4) {
		if (is_diff_reset)
      {
        return last_diff_reset_value;
      }
      return 1;
    }
    // Otherwise, use a smaller N if the start of the chain is less than N+1.
    else if ( timestamps.size()-1 < N ) {
      N = timestamps.size() - 1;
    }
    // Otherwise make sure timestamps and cumulative_difficulties are correct size.
    else {
      // TODO: put asserts here, so that the difficulty algorithm is never called with an oversized window
      //       OR make this use the last N+1 timestamps and cum_diff, not the first.
      timestamps.resize(N+1);
      cumulative_difficulties.resize(N+1);
    }
    // To get an average solvetime to within +/- ~0.1%, use an adjustment factor.
    // adjust=0.999 for 80 < N < 120(?)
    const double adjust = 0.998;
    // The divisor k normalizes the LWMA sum to a standard LWMA.
    const double k = N * (N + 1) / 2;

    double LWMA(0), sum_inverse_D(0), harmonic_mean_D(0), nextDifficulty(0);
    int64_t solveTime(0);
    uint64_t difficulty(0), next_difficulty(0);

    // Loop through N most recent blocks. N is most recently solved block.
    for (int64_t i = 1; i <= (int64_t)N; i++) {
      solveTime = static_cast<int64_t>(timestamps[i]) - static_cast<int64_t>(timestamps[i - 1]);
      solveTime = std::min<int64_t>((T * 7), std::max<int64_t>(solveTime, (-7 * T)));
      difficulty = cumulative_difficulties[i] - cumulative_difficulties[i - 1];
      LWMA += (solveTime * i) / k;
      sum_inverse_D += 1 / static_cast<double>(difficulty);
    }

    harmonic_mean_D = N / sum_inverse_D;

    // Keep LWMA sane in case something unforeseen occurs.
    if (static_cast<int64_t>(boost::math::round(LWMA)) < T / 20)
      LWMA = static_cast<double>(T / 20);

    nextDifficulty = harmonic_mean_D * T / LWMA * adjust;

    // No limits should be employed, but this is correct way to employ a 20% symmetrical limit:
    // nextDifficulty=max(previous_Difficulty*0.8,min(previous_Difficulty/0.8, next_Difficulty));
    next_difficulty = static_cast<uint64_t>(nextDifficulty);
    return next_difficulty;
  }

}

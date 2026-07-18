/*
 * QuickView Mini TIFF Decoder - LZW decompression implementation
 * Copyright (C) 2026-Present QuickView Contributors
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
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "pch.h"
#include "MiniTiff.h"
#include <cstdio>
#include <cstring>

namespace QuickView::MiniTiff {

bool DecompressLzw(const uint8_t* src, size_t srcLen, uint8_t* dst, size_t dstLen) {
    if (srcLen == 0 || dstLen == 0 || !src || !dst) {
        return false;
    }

    // Stack-allocated LZW dictionary (No heap allocations)
    uint16_t prefix[4096];
    uint8_t character[4096];
    uint8_t stack[4096];

    auto resetDictionary = [&](uint16_t& nextCode, uint32_t& codeSize) {
        for (uint16_t i = 0; i < 256; ++i) {
            prefix[i] = 65535; // None
            character[i] = static_cast<uint8_t>(i);
        }
        nextCode = 258;
        codeSize = 9;
    };

    uint16_t nextCode = 258;
    uint32_t codeSize = 9;
    resetDictionary(nextCode, codeSize);

    size_t srcIdx = 0;
    size_t dstIdx = 0;

    uint64_t bitBuffer = 0;
    uint32_t bitCount = 0;

    auto readCode = [&]() -> int32_t {
        while (bitCount < codeSize) {
            if (srcIdx >= srcLen) {
                return -1; // Out of bitstream bytes
            }
            bitBuffer = (bitBuffer << 8) | src[srcIdx++];
            bitCount += 8;
        }
        uint32_t shift = bitCount - codeSize;
        uint32_t mask = (1 << codeSize) - 1;
        uint32_t code = (bitBuffer >> shift) & mask;
        bitCount = shift;
        return static_cast<int32_t>(code);
    };

    int32_t oldCode = -1;
    uint8_t firstChar = 0;

    while (dstIdx < dstLen) {
        int32_t code = readCode();
        if (code < 0 || code == 257) { // EOF or error
            break;
        }

        if (code == 256) { // Clear Code
            resetDictionary(nextCode, codeSize);
            oldCode = -1;
            continue;
        }

        if (oldCode == -1) {
            if (code >= 256) {
                return false; // Invalid first code
            }
            dst[dstIdx++] = static_cast<uint8_t>(code);
            firstChar = static_cast<uint8_t>(code);
            oldCode = code;
            continue;
        }

        if (code < nextCode) {
            // Translate code to stack
            uint32_t stackIdx = 0;
            int32_t curr = code;
            uint32_t iter = 0;
            while (curr >= 258) {
                if (curr >= 4096 || ++iter > 4096 || stackIdx >= 4096) {
                    return false; // Cycle or stack overflow
                }
                stack[stackIdx++] = character[curr];
                curr = prefix[curr];
            }
            stack[stackIdx++] = static_cast<uint8_t>(curr);
            firstChar = static_cast<uint8_t>(curr);

            if (dstIdx + stackIdx > dstLen) {
                return false; // Out of output bounds
            }

            while (stackIdx > 0) {
                dst[dstIdx++] = stack[--stackIdx];
            }

            // Add translation to dictionary
            if (nextCode < 4096) {
                prefix[nextCode] = static_cast<uint16_t>(oldCode);
                character[nextCode] = firstChar;
                nextCode++;

                // TIFF LZW bit-width adjustment
                if (nextCode == 511 && codeSize < 12) {
                    codeSize = 10;
                } else if (nextCode == 1023 && codeSize < 12) {
                    codeSize = 11;
                } else if (nextCode == 2047 && codeSize < 12) {
                    codeSize = 12;
                }
            }

            oldCode = code;
        } else if (code == nextCode) {
            // Translate oldCode
            uint32_t stackIdx = 0;
            int32_t curr = oldCode;
            uint32_t iter = 0;
            while (curr >= 258) {
                if (curr >= 4096 || ++iter > 4096 || stackIdx >= 4096) {
                    return false;
                }
                stack[stackIdx++] = character[curr];
                curr = prefix[curr];
            }
            stack[stackIdx++] = static_cast<uint8_t>(curr);
            firstChar = static_cast<uint8_t>(curr);

            if (dstIdx + stackIdx + 1 > dstLen) {
                return false;
            }

            // Write translation of oldCode
            uint32_t s = stackIdx;
            while (s > 0) {
                dst[dstIdx++] = stack[--s];
            }
            // Write K (firstChar)
            dst[dstIdx++] = firstChar;

            // Add translation to dictionary
            if (nextCode < 4096) {
                prefix[nextCode] = static_cast<uint16_t>(oldCode);
                character[nextCode] = firstChar;
                nextCode++;

                if (nextCode == 511 && codeSize < 12) {
                    codeSize = 10;
                } else if (nextCode == 1023 && codeSize < 12) {
                    codeSize = 11;
                } else if (nextCode == 2047 && codeSize < 12) {
                    codeSize = 12;
                }
            }

            oldCode = code;
        } else {
            return false; // Code > nextCode (Corrupt)
        }
    }

    if (dstIdx != dstLen) {
        std::memset(dst + dstIdx, 0, dstLen - dstIdx);
    }

    return true;
}

} // namespace QuickView::MiniTiff

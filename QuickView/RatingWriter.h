/*
 * QuickView Star Ratings - writing a rating into an image file
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

#pragma once

#include <string>

namespace QuickView::Rating {

enum class WriteStatus {
    WrittenInPlace,   // patched into existing metadata padding; pixels untouched
    WrittenTranscode, // rebuilt losslessly because there was no room to patch
    NeedsTranscode,   // only a rebuild would do, and the caller asked not to
    Failed,
};

// Store `stars` (0..5) in an image file. 0 removes the rating rather than
// writing a zero, so that clearing restores the file to never-rated.
//
// The fast path patches the value into the metadata padding already in the
// file, which leaves every pixel byte alone. When there is no room, the file
// has to be rebuilt; that rebuild copies the compressed frame verbatim, so it
// is still lossless, but it does replace the file. `allowTranscode` == false
// makes the function report NeedsTranscode instead, which lets the caller
// postpone the rebuild while the file is memory-mapped for display.
WriteStatus WriteRatingToImage(const std::wstring& path, int stars, bool allowTranscode);

} // namespace QuickView::Rating

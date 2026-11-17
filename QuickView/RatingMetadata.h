/*
 * QuickView Star Ratings - metadata parsing (pure, no WIC / no Win32)
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

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Reading ratings must never touch the decode pipeline, so these parsers work
// on a plain byte prefix of a file (a header read) and pull in nothing but the
// standard library -- which also makes them directly unit-testable.
namespace QuickView::Rating {

inline constexpr int MIN_STARS = 0;
inline constexpr int MAX_STARS = 5;
// Adobe writes xmp:Rating="-1" to mark a photo rejected. QuickView never
// authors that value, but it must be recognized instead of being mistaken for
// a star count.
inline constexpr int REJECTED = -1;

// True for a value we are willing to surface (-1 rejected, or 0..5 stars).
constexpr bool IsValidRating(int value) {
    return value == REJECTED || (value >= MIN_STARS && value <= MAX_STARS);
}

// Parse an XMP document (a .xmp sidecar, or the XMP packet embedded in a file)
// for xmp:Rating. Both serializations are accepted:
//     xmp:Rating="3"                 (attribute form, what Lightroom writes)
//     <xmp:Rating>3</xmp:Rating>     (element form)
// Returns nothing when the property is absent or malformed.
std::optional<int> ParseXmpRating(std::string_view xmp);

// Scan the head of a JPEG for a rating, cheapest source first:
//   1. APP1 Exif -> IFD0 tag 0x4746 (SimpleRating, what Explorer writes)
//   2. APP1 XMP  -> xmp:Rating      (what Lightroom writes; Explorer may not
//                                    have written the Exif tag at all)
// `bytes` may be a prefix of the file; scanning stops at SOS, since no
// metadata follows the compressed scan. Returns nothing when absent.
std::optional<int> ParseJpegRating(std::span<const uint8_t> bytes);

// Scan the head of a bare TIFF for a rating. A TIFF stream carries its IFD
// directly (no APP1 wrapper), so it needs its own entry point.
std::optional<int> ParseTiffRating(std::span<const uint8_t> bytes);

// Scan the head of a PNG for a rating before IDAT:
//   1. eXIf chunk -> Exif IFD0 tag 0x4746 (SimpleRating)
//   2. iTXt chunk -> XML:com.adobe.xmp -> xmp:Rating
// Returns nothing when absent or before IDAT without a rating.
std::optional<int> ParsePngRating(std::span<const uint8_t> bytes);

// Scan a WebP RIFF stream for a rating:
//   1. EXIF chunk -> Exif IFD0 tag 0x4746 (SimpleRating)
//   2. XMP  chunk -> xmp:Rating
// Returns nothing when absent or malformed.
std::optional<int> ParseWebpRating(std::span<const uint8_t> bytes);

// Scan an ISOBMFF container (AVIF, HEIC, HEIF) for a rating:
//   1. Exif item -> Exif IFD0 tag 0x4746 (SimpleRating)
//   2. mime item (application/rdf+xml) -> xmp:Rating
// Returns nothing when absent or malformed.
std::optional<int> ParseIsobmffRating(std::span<const uint8_t> bytes);

// Scan a JPEG XL (JXL) stream for a rating:
//   1. If bare codestream (starts with 0xFF 0x0A): returns nullopt (no metadata in codestream)
//   2. If container (starts with JXL signature box): scans boxes for Exif, xml, or brob
// Returns nothing when absent or malformed.
std::optional<int> ParseJxlRating(std::span<const uint8_t> bytes);

// Decode IFD0 tag 0x4746 (SimpleRating) value field (4 inline bytes).
std::optional<int> DecodeTiffRatingEntry(uint16_t type, uint32_t count,
                                         std::span<const uint8_t> valueField,
                                         bool bigEndian);

// Parse ISOBMFF/JXL Exif item payload ([4-byte big-endian tiffOffset][TIFF data]).
std::optional<int> ParseIsoExifPayload(std::span<const uint8_t> payload);

// Streaming Brotli decompression capped at maxOutputBytes (returns empty on error or cap exceeded).
std::vector<uint8_t> DecompressBrotliBounded(std::span<const uint8_t> compressed,
                                             size_t maxOutputBytes = 4u * 1024u * 1024u);

struct IsobmffMetaLocations {
    bool hasExif = false;
    uint64_t exifOffset = 0;
    uint64_t exifLength = 0;
    bool exifInIdat = false;

    bool hasXmp = false;
    uint64_t xmpOffset = 0;
    uint64_t xmpLength = 0;
    bool xmpInIdat = false;

    uint64_t idatOffsetInMeta = 0;
    uint64_t idatLength = 0;
};

// Parse an ISOBMFF 'meta' FullBox (including its 8/16-byte box header) to extract
// primary Exif and XMP item locations safely.
bool ParseIsobmffMetaLocations(std::span<const uint8_t> metaBox, IsobmffMetaLocations& out);

// Replace xmp:Rating inside an existing XMP document, touching nothing else.
// A sidecar written by Lightroom or Capture One carries the develop settings
// for that photo, so the update is a surgical edit of that one property and
// never a regeneration of the document.
//   - the property is rewritten in place when present, in either serialization
//   - it is inserted into the first rdf:Description when absent
//   - `stars` == 0 removes it, leaving the rest of the document intact
// Returns nothing when the document is not shaped as expected, which the
// caller must treat as "refuse to write" rather than overwriting the file.
std::optional<std::string> UpdateXmpRating(std::string_view xmp, int stars);

// A minimal sidecar for a photo that has none yet.
std::string BuildMinimalXmp(int stars);

// Update or insert xmp:Rating inside an existing XMP document while strictly
// preserving the exact byte length by expanding or shrinking the trailing
// whitespace padding before <?xpacket end=...?>.
// Returns nothing if the document does not have enough whitespace padding or is malformed.
std::optional<std::string> PatchXmpRatingInPlaceStrict(std::string_view xmp, int stars);

// Which file of a pair a displayed rating came from.
enum class Source { None, InFile, Sidecar };

struct Resolved {
    int stars = 0;              // 0..5 (a rejected -1 is surfaced as 0 stars)
    Source source = Source::None;
    bool conflict = false;      // both sides carry a rating and they differ
    int otherStars = 0;         // the losing side's value, for the EXIF panel
};

// Merge the two carriers of a folded RAW+rendered pair into the one rating the
// UI shows. Per the maintainer's ruling the sidecar always wins: a JPEG's
// mtime is churned by lossless rotation, cloud sync and Explorer edits, while
// the sidecar is the only reliable carrier of photographer intent (LR/C1).
Resolved ResolvePairRating(std::optional<int> inFile, std::optional<int> sidecar);

} // namespace QuickView::Rating

/*
 * QuickView Slim ZXing Reader Router (Popular 2D + 1D Barcodes Only)
 * Copyright (C) 2026-Present QuickView Contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "MultiFormatReader.h"

#include "Barcode.h"
#include "BarcodeFormat.h"
#include "BinaryBitmap.h"
#include "ReaderOptions.h"
#include "datamatrix/DMReader.h"
#include "oned/ODCode128Reader.h"
#include "oned/ODCode39Reader.h"
#include "oned/ODMultiUPCEANReader.h"
#include "oned/ODReader.h"
#include "qrcode/QRReader.h"

#include <algorithm>
#include <memory>
#include <utility>

namespace ZXing {

MultiFormatReader::MultiFormatReader(const ReaderOptions& opts) : _opts(opts)
{
	auto formats = opts.formats().empty() ? BarcodeFormat::Any : opts.formats();

	if (formats.testFlags(BarcodeFormat::LinearCodes) && !opts.tryHarder())
		_readers.emplace_back(new OneD::Reader(opts));

	if (formats.testFlags(BarcodeFormat::QRCode | BarcodeFormat::MicroQRCode | BarcodeFormat::RMQRCode))
		_readers.emplace_back(new QRCode::Reader(opts, true));
	if (formats.testFlag(BarcodeFormat::DataMatrix))
		_readers.emplace_back(new DataMatrix::Reader(opts, true));

	if (formats.testFlags(BarcodeFormat::LinearCodes) && opts.tryHarder())
		_readers.emplace_back(new OneD::Reader(opts));
}

MultiFormatReader::~MultiFormatReader() = default;

Barcode MultiFormatReader::read(const BinaryBitmap& image) const
{
	Barcode r;
	for (const auto& reader : _readers) {
		r = reader->decode(image);
		if (r.isValid())
			return r;
	}
	return _opts.returnErrors() ? r : Barcode();
}

Barcodes MultiFormatReader::readMultiple(const BinaryBitmap& image, int maxSymbols) const
{
	Barcodes res;

	for (const auto& reader : _readers) {
		if (image.inverted() && !reader->supportsInversion)
			continue;
		auto r = reader->decode(image, maxSymbols);
		if (!_opts.returnErrors()) {
			std::erase_if(r, [](auto&& s) { return !s.isValid(); });
		}
		maxSymbols -= Size(r);
		res.insert(res.end(), std::move_iterator(r.begin()), std::move_iterator(r.end()));
		if (maxSymbols <= 0)
			break;
	}

	std::sort(res.begin(), res.end(), [](const Barcode& l, const Barcode& r) {
		auto lp = l.position().topLeft();
		auto rp = r.position().topLeft();
		return lp.y < rp.y || (lp.y == rp.y && lp.x < rp.x);
	});

	return res;
}

void IncrementLineCount(Barcode& r)
{
	++r._lineCount;
}

} // namespace ZXing

namespace ZXing::OneD {

Reader::Reader(const ReaderOptions& opts) : ZXing::Reader(opts)
{
	_readers.reserve(3);

	auto formats = opts.formats().empty() ? BarcodeFormat::Any : opts.formats();

	if (formats.testFlags(BarcodeFormat::EAN13 | BarcodeFormat::UPCA | BarcodeFormat::EAN8 | BarcodeFormat::UPCE))
		_readers.emplace_back(new MultiUPCEANReader(opts));
	if (formats.testFlag(BarcodeFormat::Code128))
		_readers.emplace_back(new Code128Reader(opts));
	if (formats.testFlag(BarcodeFormat::Code39))
		_readers.emplace_back(new Code39Reader(opts));
}

Reader::~Reader() = default;

static Barcodes DoDecode(const std::vector<std::unique_ptr<RowReader>>& readers, const BinaryBitmap& image, bool tryHarder,
						 bool rotate, bool isPure, int maxSymbols, int minLineCount, bool returnErrors)
{
	Barcodes res;
	std::vector<std::unique_ptr<RowReader::DecodingState>> decodingState(readers.size());

	int width = image.width();
	int height = image.height();
	if (rotate)
		std::swap(width, height);

	int middle = height / 2;
	int rowStep = std::max(1, height / ((tryHarder && !isPure) ? (maxSymbols == 1 ? 256 : 512) : 32));
	int maxLines = tryHarder ? height : 15;

	if (isPure)
		minLineCount = 1;
	else
		minLineCount = std::min(minLineCount, height);

	std::vector<int> checkRows;
	PatternRow bars;
	bars.reserve(128);

	for (int i = 0; i < maxLines; i++) {
		int rowStepsAboveOrBelow = (i + 1) / 2;
		bool isAbove = (i & 0x01) == 0;
		int rowNumber = middle + rowStep * (isAbove ? rowStepsAboveOrBelow : -rowStepsAboveOrBelow);
		bool isCheckRow = false;
		if (rowNumber < 0 || rowNumber >= height)
			break;

		if (!checkRows.empty()) {
			--i;
			rowNumber = checkRows.back();
			checkRows.pop_back();
			isCheckRow = true;
			if (rowNumber < 0 || rowNumber >= height)
				continue;
		}

		if (!image.getPatternRow(rowNumber, rotate ? 90 : 0, bars))
			continue;

		for (bool upsideDown : {false, true}) {
			if (upsideDown)
				std::reverse(bars.begin(), bars.end());

			for (size_t r = 0; r < readers.size(); ++r) {
				if (isPure && i && !decodingState[r])
					continue;

				PatternView next(bars);
				do {
					Barcode result = readers[r]->decodePattern(rowNumber, next, decodingState[r]);
					if (result.isValid() || (returnErrors && result.error())) {
						IncrementLineCount(result);
						if (upsideDown) {
							auto points = result.position();
							for (auto& p : points)
								p = {width - p.x - 1, p.y};
							result.setPosition(std::move(points));
						}
						if (rotate) {
							auto points = result.position();
							for (auto& p : points)
								p = {p.y, width - p.x - 1};
							result.setPosition(std::move(points));
						}

						for (auto& other : res) {
							if (result == other) {
								auto dTop = maxAbsComponent(other.position().topLeft() - result.position().topLeft());
								auto dBot = maxAbsComponent(other.position().bottomLeft() - result.position().topLeft());
								auto points = other.position();
								if (dTop < dBot || (dTop == dBot && rotate ^ (sumAbsComponent(points[0]) >
																			  sumAbsComponent(result.position()[0])))) {
									points[0] = result.position()[0];
									points[1] = result.position()[1];
								} else {
									points[2] = result.position()[2];
									points[3] = result.position()[3];
								}
								other.setPosition(points);
								IncrementLineCount(other);
								result = Barcode();
								break;
							}
						}

						if (result.format() != BarcodeFormat::None) {
							res.push_back(std::move(result));
							if (!isCheckRow && minLineCount > 1 && rowStep > 1) {
								checkRows = {rowNumber - 1, rowNumber + 1};
								if (rowStep > 2)
									checkRows.insert(checkRows.end(), {rowNumber - 2, rowNumber + 2});
							}
						}

						if (maxSymbols && Reduce(res, 0, [&](int s, const Barcode& b) {
											  return s + (b.lineCount() >= minLineCount);
										  }) == maxSymbols) {
							goto out;
						}
					}
					next.shift(2 - (next.index() % 2));
					next.extend();
				} while (tryHarder && next.size());
			}
		}
	}

out:
	std::erase_if(res, [&](auto&& r) { return r.lineCount() < minLineCount; });

	for (auto a = res.begin(); a != res.end(); ++a)
		for (auto b = std::next(a); b != res.end(); ++b)
			if (HaveIntersectingBoundingBoxes(a->position(), b->position()))
				*(a->lineCount() < b->lineCount() ? a : b) = Barcode();

	std::erase_if(res, [](auto&& r) { return r.format() == BarcodeFormat::None; });

	return res;
}

Barcode Reader::decode(const BinaryBitmap& image) const
{
	auto result =
		DoDecode(_readers, image, _opts.tryHarder(), false, _opts.isPure(), 1, _opts.minLineCount(), _opts.returnErrors());

	if (result.empty() && _opts.tryRotate())
		result = DoDecode(_readers, image, _opts.tryHarder(), true, _opts.isPure(), 1, _opts.minLineCount(), _opts.returnErrors());

	return FirstOrDefault(std::move(result));
}

Barcodes Reader::decode(const BinaryBitmap& image, int maxSymbols) const
{
	auto resH = DoDecode(_readers, image, _opts.tryHarder(), false, _opts.isPure(), maxSymbols, _opts.minLineCount(),
						 _opts.returnErrors());
	if ((!maxSymbols || Size(resH) < maxSymbols) && _opts.tryRotate()) {
		auto resV = DoDecode(_readers, image, _opts.tryHarder(), true, _opts.isPure(), maxSymbols - Size(resH),
							 _opts.minLineCount(), _opts.returnErrors());
		resH.insert(resH.end(), resV.begin(), resV.end());
	}
	return resH;
}

} // namespace ZXing::OneD

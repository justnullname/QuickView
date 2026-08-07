/*
 * QuickView - Image Resource Management and GPU Asset Handles
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

#include "pch.h"
#include "ImageTypes.h"
#include "AnimationDecoder.h"

#include <d2d1_2.h>
#include <d2d1_3.h>
#include <dxgi1_2.h>
#include <dcomp.h>
#include <memory>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

struct ImageResource {
    ComPtr<ID2D1Bitmap> bitmap;
    ComPtr<ID2D1SvgDocument> svgDoc;
    bool isSvg = false;
    bool isWebView = false;     // [DComp] Complex SVG rendered via WebView2 composition
    ComPtr<IDCompositionVisual2> webViewVisual;
    float svgW = 0.0f;
    float svgH = 0.0f;
    std::wstring webviewHtml;   // [DComp] Pre-built HTML for WebView2 navigation

    QuickView::GpuBlendOp blendOp = QuickView::GpuBlendOp::None;
    QuickView::GpuShaderPayload shaderPayload = {};
    std::unique_ptr<QuickView::AuxLayer> auxLayer;

    std::shared_ptr<QuickView::IAnimationDecoder> animator;
    QuickView::AnimationFrameMeta frameMeta;

    void Reset() {
        bitmap.Reset();
        svgDoc.Reset();
        webViewVisual.Reset();
        isSvg = false;
        isWebView = false;
        svgW = 0.0f;
        svgH = 0.0f;
        webviewHtml.clear();
        blendOp = QuickView::GpuBlendOp::None;
        shaderPayload = {};
        auxLayer.reset();
        animator.reset();
        frameMeta = {};
    }

    ImageResource() = default;
    ImageResource(const ImageResource&) = delete;
    ImageResource& operator=(const ImageResource&) = delete;
    ImageResource(ImageResource&&) = default;
    ImageResource& operator=(ImageResource&&) = default;

    ImageResource Clone() const {
        ImageResource cloned;
        cloned.bitmap = bitmap;
        cloned.svgDoc = svgDoc;
        cloned.isSvg = isSvg;
        cloned.isWebView = isWebView;
        cloned.svgW = svgW;
        cloned.svgH = svgH;
        cloned.webviewHtml = webviewHtml;
        cloned.blendOp = blendOp;
        cloned.shaderPayload = shaderPayload;
        if (auxLayer) {
            cloned.auxLayer = auxLayer->Clone();
        }
        cloned.animator = animator;
        cloned.frameMeta = frameMeta;
        return cloned;
    }

    D2D1_SIZE_F GetSize() const {
        if (isSvg || isWebView) return D2D1::SizeF(svgW, svgH);
        if (bitmap) return bitmap->GetSize();
        return D2D1::SizeF(0.0f, 0.0f);
    }

    operator bool() const {
        return (isSvg && svgDoc) || isWebView || bitmap;
    }
};

inline DXGI_FORMAT GetImageResourceSurfaceFormat(const ImageResource& resource) {
    if (!resource.bitmap) {
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    }

    const DXGI_FORMAT bitmapFormat = resource.bitmap->GetPixelFormat().format;
    if (bitmapFormat == DXGI_FORMAT_R16G16B16A16_FLOAT ||
        bitmapFormat == DXGI_FORMAT_R32G32B32A32_FLOAT) {
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    }

    return DXGI_FORMAT_B8G8R8A8_UNORM;
}

inline D2D1_SIZE_F GetOrientedSize(const ImageResource& res, int exifOrientation) {
    D2D1_SIZE_F size = res.GetSize();
    const bool swapped = (exifOrientation >= 5 && exifOrientation <= 8);
    if (swapped) {
        return D2D1::SizeF(size.height, size.width);
    }
    return size;
}


/*
 * QuickView - Compare Mode Pane Context and State Tracking
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

#include "PaneTypes.h"
#include "ImageResource.h"
#include "EditState.h"
#include "FileNavigator.h"
#include "ImageLoader.h"

#include <cstdint>
#include <string>

struct PaneContext {
    ImageResource resource;
    std::wstring path;
    EditState editState;
    ViewState view;
    FileNavigator navigator;
    CImageLoader::ImageMetadata metadata;
    bool valid = false;
    uint64_t generationId = 0;

    int CmsModeOverride = -1;
    bool EnableSoftProofing = false;
    std::wstring SoftProofProfilePath;
    std::shared_ptr<QuickView::RawImageFrame> currentFrame;

    void Reset() {
        resource.Reset();
        path.clear();
        currentFrame.reset();
        editState.Reset();
        view.Reset();
        metadata = {};
        valid = false;
        CmsModeOverride = -1;
        EnableSoftProofing = false;
        SoftProofProfilePath.clear();
        ++generationId;
    }
};

extern PaneContext g_panes[2];

inline PaneContext& GetPaneContext(PaneSlot slot) {
    return g_panes[static_cast<int>(slot)];
}



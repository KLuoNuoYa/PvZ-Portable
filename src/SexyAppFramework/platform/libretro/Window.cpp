/*
 * Copyright (C) 2026 PvZ libretro contributors
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * This file is part of PvZ-Portable's libretro port.
 *
 * libretro window backend: there is no window.  The frontend owns the display
 * surface and hands us a GL context, so MakeWindow() only has to bring up the
 * GL interface and point the widget manager at the frontend's framebuffer.
 */

#include "LibretroBackend.h"

#include "SexyAppBase.h"
#include "graphics/GLInterface.h"
#include "graphics/GLImage.h"
#include "widget/WidgetManager.h"

using namespace Sexy;

void SexyAppBase::MakeWindow()
{
	// No SDL window and no SDL GL context: the frontend created the context
	// before calling us (retro_hw_render_callback::context_reset).
	if (mGLInterface == nullptr)
	{
		mGLInterface = std::make_unique<GLInterface>(this);
		if (!InitGLInterface())
		{
			mGLInterface = nullptr;
			return;
		}
	}

	PvzLibretro::BindFrontendFramebuffer();

	// There is no focus loss and no minimise in libretro: the frontend simply
	// stops calling retro_run().  Keep the app permanently "active" so the
	// simulation never pauses itself.
	mActive = true;
	mMinimized = false;
	mPhysMinimized = false;
	mIsWindowed = true;
	mIsPhysWindowed = true;

	ReInitImages();

	mWidgetManager->mImage = mGLInterface->GetScreenImage();
	mWidgetManager->MarkAllDirty();

	mGLInterface->UpdateViewport();
	mWidgetManager->Resize(mScreenBounds, mGLInterface->mPresentationRect);
}

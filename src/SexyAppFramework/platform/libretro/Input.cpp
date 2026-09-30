/*
 * Copyright (C) 2026 PvZ libretro contributors
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * This file is part of PvZ-Portable's libretro port.
 *
 * libretro input backend: replaces SDL event pumping with the libretro input
 * API.  InputPollFrontend() snapshots the frontend state once per retro_run()
 * and InputDispatchOne() replays the resulting events into the widget manager,
 * which is exactly the contract ProcessDeferredMessages() had with SDL.
 */

#include "LibretroBackend.h"

#include "SexyAppBase.h"
#include "widget/WidgetManager.h"

using namespace Sexy;

void SexyAppBase::InitInput()
{
	// Nothing to initialise: libretro input needs no subsystem.
}

bool SexyAppBase::ProcessDeferredMessages([[maybe_unused]] bool singleMessage)
{
	// Delivers every event captured by InputPollFrontend() and reports that no
	// further messages are pending, so UpdateAppStep() advances to PROCESS_1 in
	// the same step (matching the SDL backend's behaviour with an empty queue).
	PvzLibretro::InputDispatchOne();
	return false;
}

bool SexyAppBase::StartTextInput([[maybe_unused]] std::string& theInput)
{
	// The frontend owns the on-screen keyboard.
	return false;
}

void SexyAppBase::StopTextInput()
{
}

void SexyAppBase::SetTextInputRect([[maybe_unused]] const Rect& theRect)
{
}

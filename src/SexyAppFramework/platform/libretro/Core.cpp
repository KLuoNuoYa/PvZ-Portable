/*
 * Copyright (C) 2026 PvZ libretro contributors
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * This file is part of PvZ-Portable's libretro port.
 *
 * libretro core entry points.
 *
 * Game data resolution
 * --------------------
 * The core can run in two ways:
 *
 *   1. BIOS mode - no content is loaded.  main.pak and the properties/ folder
 *      are taken from  <system directory>/pvz/ .
 *   2. Content mode - a main.pak is loaded as content.  The core then uses the
 *      directory that contains that file, and looks for a properties/ folder
 *      next to it.  The BIOS directory is not consulted at all in this mode.
 *
 * Either way main.pak supplies everything except the string/layout overrides in
 * properties/, which are optional.
 */

#include "LibretroBackend.h"
#include "Options.h"
#include "SaveState.h"

#include <cstring>
#include <string>

#include <SDL.h>

#include "LawnApp.h"
#include "Resources.h"
#include "SexyAppBase.h"
#include "PvzpLib/PvzpStringFile.h"

using namespace Sexy;

namespace
{

void ShowMessage(const char* theText, unsigned theFrames = 360)
{
	if (PvzLibretro::EnvironCb == nullptr)
		return;

	retro_message aMessage = {};
	aMessage.msg = theText;
	aMessage.frames = theFrames;
	PvzLibretro::EnvironCb(RETRO_ENVIRONMENT_SET_MESSAGE, &aMessage);
}

} // namespace

// ===========================================================================
// SexyAppBase libretro teardown helper
// ===========================================================================
#ifdef __LIBRETRO__
void SexyAppBase::LibretroTeardown()
{
	if (!mShutdown)
		Shutdown();

	WaitForLoadingThread();
	ProcessSafeDeleteList();
	mRunning = false;
}
#endif

// ===========================================================================
// retro_* API
// ===========================================================================
extern "C"
{

RETRO_API unsigned retro_api_version(void)
{
	return RETRO_API_VERSION;
}

RETRO_API void retro_set_environment(retro_environment_t cb)
{
	PvzLibretro::EnvironCb = cb;

	// The core is happy to start with no content: the BIOS (system/pvz) then
	// supplies main.pak and properties/.
	bool aSupportNoGame = true;
	cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &aSupportNoGame);

	retro_log_callback aLogCallback = {};
	if (cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &aLogCallback))
		PvzLibretro::LogCb = aLogCallback.log;

	// The game is a point-and-click title: a mouse (or a pointer device) is the
	// primary input, with keyboard and joypad as alternatives.
	static retro_input_descriptor aDescriptors[] = {
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "Move cursor left" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "Move cursor right" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "Move cursor up" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "Move cursor down" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "Left click" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "Right click" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "Escape" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Enter" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,      "Mouse wheel up" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,      "Mouse wheel down" },
		{ 0, RETRO_DEVICE_MOUSE,  0, RETRO_DEVICE_ID_MOUSE_LEFT,    "Left click" },
		{ 0, RETRO_DEVICE_MOUSE,  0, RETRO_DEVICE_ID_MOUSE_RIGHT,   "Right click" },
		{ 0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_PRESSED, "Left click" },
		{ 0, 0, 0, 0, nullptr },
	};
	cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, aDescriptors);

	static retro_controller_description aPortDevices[] = {
		{ "Gamepad", RETRO_DEVICE_JOYPAD },
		{ "Mouse",   RETRO_DEVICE_MOUSE },
		{ "Pointer", RETRO_DEVICE_POINTER },
		{ nullptr,   0 },
	};
	// The array MUST be terminated with { NULL, 0 }: the frontend walks it until
	// info[i].types is NULL and would otherwise read past the end (RetroArch
	// dumps every port, so an unterminated array crashes it during load).
	static retro_controller_info aPortInfo[] = {
		{ aPortDevices, 3 },
		{ nullptr, 0 },
	};
	cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, aPortInfo);

	// Preferred keyboard path: the frontend pushes key and character events, which
	// is what the profile-name text fields need.  The core additionally polls the
	// raw keyboard state, because a frontend is allowed to swallow keys that are
	// bound to its own controls (see PollKeyboardKeys).
	static retro_keyboard_callback aKeyboardCallback = { PvzLibretro::KeyboardEventCallback };
	cb(RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK, &aKeyboardCallback);

	// Core options: the game's own switches are build flags and command line
	// parameters, which a frontend cannot supply.
	PvzLibretro::RegisterOptions();
}

RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb)
{
	PvzLibretro::VideoCb = cb;
}

RETRO_API void retro_set_audio_sample(retro_audio_sample_t cb)
{
	PvzLibretro::AudioSampleCb = cb;
}

RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb)
{
	PvzLibretro::AudioBatchCb = cb;
}

RETRO_API void retro_set_input_poll(retro_input_poll_t cb)
{
	PvzLibretro::InputPollCb = cb;
}

RETRO_API void retro_set_input_state(retro_input_state_t cb)
{
	PvzLibretro::InputStateCb = cb;
}

RETRO_API void retro_init(void)
{
}

RETRO_API void retro_deinit(void)
{
	PvzLibretro::ShutdownGameFiber();
	PvzLibretro::AudioStopCapture();

	// Belt and braces: a frontend that failed to load content may call this
	// without ever unloading a game, and the SDL timer thread must not outlive the
	// DLL either way (see retro_unload_game).  Idempotent.
	SDL_Quit();

	PvzLibretro::GameLoaded = false;
	PvzLibretro::GameInitialized = false;
	PvzLibretro::ShutdownRequested = false;
}

RETRO_API void retro_get_system_info(struct retro_system_info* info)
{
	std::memset(info, 0, sizeof(*info));
	info->library_name = "PvZ";
	info->library_version = "1.0.0";
	info->valid_extensions = "pak";
	// The engine needs a real path: it opens main.pak itself and looks for a
	// properties/ directory next to it.
	info->need_fullpath = true;
	info->block_extract = true;
}

RETRO_API void retro_get_system_av_info(struct retro_system_av_info* info)
{
	std::memset(info, 0, sizeof(*info));
	info->geometry.base_width = PvzLibretro::FrameWidth;
	info->geometry.base_height = PvzLibretro::FrameHeight;
	info->geometry.max_width = PvzLibretro::FrameWidth;
	info->geometry.max_height = PvzLibretro::FrameHeight;
	info->geometry.aspect_ratio = 4.0f / 3.0f;
	// One presented frame per vertical refresh.  The game's own 100 Hz logic
	// tick is driven by a wall-clock accumulator, so it stays at the correct
	// speed whatever rate the frontend runs at.
	info->timing.fps = 60.0;
	info->timing.sample_rate = 44100.0;
}

RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device)
{
	// The player picks one of the device types this core declared through
	// RETRO_ENVIRONMENT_SET_CONTROLLER_INFO (Gamepad / Mouse / Pointer).  That
	// choice is the explicit answer to "how is this player pointing?", so the
	// input layer uses it instead of guessing.
	if (port == 0)
		PvzLibretro::SetPortDevice(device);
}

RETRO_API void retro_reset(void)
{
	// The game owns its own state; a frontend reset is a no-op.
}

// The game has its own (PVZ_DEBUG) cheat keys; there is no code database to
// apply.  The entry points must still exist: frontends resolve every symbol in
// the libretro API and refuse to load a core that is missing any of them.
RETRO_API void retro_cheat_reset(void)
{
}

RETRO_API void retro_cheat_set(unsigned index, bool enabled, const char* code)
{
	(void)index;
	(void)enabled;
	(void)code;
}

RETRO_API void retro_run(void)
{
	if (!PvzLibretro::GameLoaded || PvzLibretro::ShutdownRequested)
		return;

	// The frontend's GL context is current here but its state belongs to the
	// frontend: re-establish ours before drawing.
	PvzLibretro::BindFrontendFramebuffer();
	PvzLibretro::ReapplyRendererState();

	PvzLibretro::RefreshOptions();
	PvzLibretro::InputPollFrontend();
	PvzLibretro::RunFrame();
	PvzLibretro::DrawCursor();
	PvzLibretro::PresentFrame();
	PvzLibretro::AudioFlushToFrontend();
}

// Save states cover the level in progress, through the game's own mid-level
// snapshot (see SaveState.cpp).  Outside a level there is nothing to snapshot
// that would not also roll back the player's profile, so those fail instead.
RETRO_API size_t retro_serialize_size(void)
{
	return PvzLibretro::SaveStateSize();
}

RETRO_API bool retro_serialize(void* data, size_t size)
{
	return PvzLibretro::SaveState(data, size);
}

RETRO_API bool retro_unserialize(const void* data, size_t size)
{
	// Accepted here, applied at the top of the next frame: the frontend is free
	// to call this while the game sits inside a modal dialog.
	return PvzLibretro::LoadState(data, size);
}

RETRO_API bool retro_load_game(const struct retro_game_info* theGame)
{
	std::string aError;
	const char* aPath = (theGame != nullptr) ? theGame->path : nullptr;

	if (!PvzLibretro::ResolveResources(aPath, aError))
	{
		PvzLibretro::LogError("[pvz] %s\n", aError.c_str());
		ShowMessage(aError.c_str(), 600);
		return false;
	}

	if (!PvzLibretro::RequestHardwareRender())
	{
		ShowMessage("This core requires hardware rendering (OpenGL ES 2.0 or OpenGL 2.1).\n"
			"Switch the frontend video driver to 'gl' or 'glcore'.", 600);
		return false;
	}

	// The save directory holds user data (profiles, progress, caches).
	const char* aSaveDir = nullptr;
	if (PvzLibretro::EnvironCb != nullptr &&
		PvzLibretro::EnvironCb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &aSaveDir) &&
		aSaveDir != nullptr && aSaveDir[0] != '\0')
	{
		PvzLibretro::SaveDir = std::string(aSaveDir) + "/PvZ-Portable";
	}
	else
	{
		PvzLibretro::SaveDir = PvzLibretro::ResourceDir + "/savedata";
	}

	// Build the application object now; Init() is deferred until the frontend
	// has created the GL context (context_reset), because the renderer needs it.
	PvzpStringListSetColors(gLawnStringFormats, gLawnStringFormatCount);
	gExtractResourcesByName = Sexy::ExtractResourcesByName;

	gLawnApp = new LawnApp();
	gLawnApp->mResourceDir = PvzLibretro::ResourceDir;
	gLawnApp->mCustomSaveDir = PvzLibretro::SaveDir;
	gLawnApp->SetArgs(0, nullptr);

	PvzLibretro::AudioStartCapture();
	PvzLibretro::GameLoaded = true;
	PvzLibretro::ShutdownRequested = false;
	return true;
}

RETRO_API void retro_unload_game(void)
{
	PvzLibretro::ShutdownGameFiber();

	if (gLawnApp != nullptr)
	{
		gLawnApp->LibretroTeardown();
		delete gLawnApp;
		gLawnApp = nullptr;
	}

	PvzLibretro::AudioStopCapture();

	// SDL is linked into this DLL, and the game starts its timer subsystem in the
	// SexyAppBase constructor.  SDL_TimerInit() creates a thread that parks in a
	// semaphore in this image, and nothing in the game ever stops it - so without
	// this the thread outlives the frontend's FreeLibrary(), keeps executing
	// unmapped memory and crashes the frontend: the report reads
	// "pvz_libretro.dll_unloaded", faulting in SDL_SemWait_atom, and one such
	// thread is leaked per content load (the dump showed several, one per unload).
	// SDL_Quit() runs SDL_TimerQuit(), which signals that thread and *joins* it, so
	// nothing of ours is still running when we hand control back.
	SDL_Quit();

	PvzLibretro::GameLoaded = false;
	PvzLibretro::GameInitialized = false;
}

RETRO_API unsigned retro_get_region(void)
{
	return RETRO_REGION_NTSC;
}

RETRO_API void* retro_get_memory_data(unsigned id)
{
	(void)id;
	return nullptr;
}

RETRO_API size_t retro_get_memory_size(unsigned id)
{
	(void)id;
	return 0;
}

RETRO_API bool retro_load_game_special(unsigned game_type, const struct retro_game_info* info, size_t num_info)
{
	(void)game_type;
	(void)info;
	(void)num_info;
	return false;
}

} // extern "C"

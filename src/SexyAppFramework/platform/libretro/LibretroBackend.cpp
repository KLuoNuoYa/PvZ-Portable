/*
 * Copyright (C) 2026 PvZ libretro contributors
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * This file is part of PvZ-Portable's libretro port.
 *
 * Platform backend shared services: frontend callbacks, hardware rendering,
 * the audio bridge and input translation.
 *
 * Design notes
 * ------------
 *  * Video: the game's renderer is OpenGL ES 2.0 and draws straight into the
 *    default framebuffer, so the core asks the frontend for a GL context via
 *    RETRO_ENVIRONMENT_SET_HW_RENDER and renders into the frontend's FBO.  A
 *    desktop GL 2.1 compatibility context is used as a fallback, mirroring what
 *    the SDL backend does.
 *  * Audio: SDL-Mixer-X exposes a "bring your own output" API
 *    (Mix_InitMixer + Mix_GetGeneralMixer) so no SDL audio device, and no audio
 *    thread, is needed.  The final mix is pulled synchronously once per frame
 *    and handed to the frontend.
 *  * Input: libretro pointer/keyboard/joypad state is translated into the same
 *    WidgetManager mouse/key calls the SDL backend made.
 */

#include "LibretroBackend.h"
#include "Options.h"
#include "SaveState.h"

#include <algorithm>
#include <cmath>

#ifdef _WIN32
// Fibers (for cooperative blocking waits) and K32GetProcessMemoryInfo.
// NOMINMAX/WIN32_LEAN_AND_MEAN are already set by Common.h.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "SexyAppBase.h"
#include "graphics/GLImage.h"
#include "graphics/GLInterface.h"
#include "graphics/GLPlatform.h"
#include "graphics/Graphics.h"
#include "graphics/Image.h"
#include "misc/KeyCodes.h"
#include "widget/WidgetManager.h"

// SDL-Mixer-X "bring your own output" entry points.
#include "SDL_mixer.h"

using namespace Sexy;

namespace PvzLibretro
{

// ---------------------------------------------------------------- callbacks --
retro_environment_t			EnvironCb = nullptr;
retro_video_refresh_t		VideoCb = nullptr;
retro_audio_sample_t		AudioSampleCb = nullptr;
retro_audio_sample_batch_t	AudioBatchCb = nullptr;
retro_input_poll_t			InputPollCb = nullptr;
retro_input_state_t			InputStateCb = nullptr;
retro_log_printf_t			LogCb = nullptr;

// ------------------------------------------------------------ core lifecycle --
bool		GameLoaded = false;
bool		ShutdownRequested = false;

std::string	ResourceDir;
std::string	PropertiesDir;
bool		ResourceIsBios = false;
std::string	SaveDir;
std::string	ContentPath;

// --------------------------------------------------------- hardware rendering --
retro_hw_render_callback	HwRender = {};
bool						HwRenderEnabled = false;
bool						GlFunctionsLoaded = false;
bool						GameInitialized = false;
unsigned					FrameWidth = 800;
unsigned					FrameHeight = 600;

// The game's own frame buffer size, discovered after the game initialises.
static unsigned	sGameWidth = 800;
static unsigned	sGameHeight = 600;

// -------------------------------------------------------------------- audio --
static const unsigned	kAudioSampleRate = 44100;
// 44100 Hz / 60 fps -> exactly 735 frames per retro_run().
static const unsigned	kAudioFramesPerRun = kAudioSampleRate / 60;
static bool				sMixerReady = false;
static std::vector<int16_t>	sAudioBuffer;

// -------------------------------------------------------------------- clock --
// The game's clock, in milliseconds.  It counts *frames*: one retro_run() is one
// frame of game time, exactly like the one frame of audio the bridge renders
// (kAudioFramesPerRun = 735 samples = 16.667 ms).
//
// Counting wall time instead looks equivalent - at a steady 60 Hz it is - but it
// breaks every time the frontend does not call us at 60 Hz, and it breaks against
// the audio, which is per-frame no matter what:
//   * fast-forward   - called as often as it can: the music ran ahead of the world
//   * frame advance  - called once per key press: the music advanced 16.667 ms per
//                      press while the game advanced one logic tick
//   * slow motion    - called less often: the world kept real time while the music
//                      slowed down with the frames
// With the simulation on the frame count, the picture and the sound cannot come
// apart: the frontend decides how often we are called, and both follow it.  Wall
// time is still what the diagnostics and the loading timers use.
//
// The frame length is carried as a fraction: a flat 16 ms would run the clock 4%
// behind the audio (735 samples = 16.667 ms).
static const uint32_t	kGameFrameMsNumerator = 1000;
static const uint32_t	kGameFrameMsDenominator = 60;
static uint32_t			sGameTime = 0;
static uint32_t			sFrameFraction = 0;	// carry of the division above
static bool				sClockStarted = false;

// -------------------------------------------------------------------- input --
struct InputEvent
{
	enum Kind { MOVE, DOWN, UP, WHEEL, KEYDOWN, KEYUP, TEXT };
	Kind		kind;
	int			x = 0;
	int			y = 0;
	int			value = 0;
	std::string	text;
};

static std::deque<InputEvent>	sEventQueue;
static bool		sMouseIn = false;
static float	sCursorX = 400.0f;	// virtual cursor for gamepad-only setups
static float	sCursorY = 300.0f;
static bool		sPointerWasDown = false;
static bool		sRightWasDown = false;
// Set once the frontend has reported an absolute pointer position; until then
// the relative mouse device is used instead.
static bool		sPointerSeen = false;
static int16_t	sLastPointerX = 0;
static int16_t	sLastPointerY = 0;
// Which device last moved the cursor.  The OS already draws a cursor for mouse
// users, so the core only paints its own pointer while the gamepad owns it.
static bool		sCursorFromPad = false;
// Device type the frontend selected for port 0 (RETRO_DEVICE_JOYPAD / _MOUSE /
// _POINTER / 0 when it never told us).
static unsigned	sPortDevice = 0;
static uint64_t	sPrevPadButtons = 0;
static bool		sKeyDown[RETROK_LAST] = {};
// Keys whose press was synthesized from the raw device state rather than
// delivered by the frontend's keyboard callback (see PollKeyboardKeys).
static bool		sKeySynthesized[RETROK_LAST] = {};

// ---------------------------------------------------------------------------
void Log(retro_log_level theLevel, const char* theFormat, ...)
{
	if (LogCb == nullptr)
		return;

	char aBuffer[2048];
	va_list aArgs;
	va_start(aArgs, theFormat);
	vsnprintf(aBuffer, sizeof(aBuffer), theFormat, aArgs);
	va_end(aArgs);

	LogCb(theLevel, "%s", aBuffer);
}

void LogInfo(const char* theFormat, ...)
{
	if (LogCb == nullptr)
		return;
	char aBuffer[2048];
	va_list aArgs;
	va_start(aArgs, theFormat);
	vsnprintf(aBuffer, sizeof(aBuffer), theFormat, aArgs);
	va_end(aArgs);
	LogCb(RETRO_LOG_INFO, "%s", aBuffer);
}

void LogError(const char* theFormat, ...)
{
	if (LogCb == nullptr)
		return;
	char aBuffer[2048];
	va_list aArgs;
	va_start(aArgs, theFormat);
	vsnprintf(aBuffer, sizeof(aBuffer), theFormat, aArgs);
	va_end(aArgs);
	LogCb(RETRO_LOG_ERROR, "%s", aBuffer);
}

// ===========================================================================
// Resource resolution
// ===========================================================================
namespace
{

bool IsRegularFile(const std::string& thePath)
{
	std::error_code aError;
	return std::filesystem::is_regular_file(Sexy::PathFromU8(thePath), aError);
}

bool IsDirectory(const std::string& thePath)
{
	std::error_code aError;
	return std::filesystem::is_directory(Sexy::PathFromU8(thePath), aError);
}

std::string JoinPath(const std::string& theDir, const std::string& theLeaf)
{
	if (theDir.empty())
		return theLeaf;
	return Sexy::PathToU8(Sexy::PathFromU8(theDir) / Sexy::PathFromU8(theLeaf));
}

std::string ParentDir(const std::string& thePath)
{
	return Sexy::PathToU8(Sexy::PathFromU8(thePath).parent_path());
}

std::string BaseName(const std::string& thePath)
{
	return Sexy::PathToU8(Sexy::PathFromU8(thePath).filename());
}

std::string LowerCase(std::string theValue)
{
	std::transform(theValue.begin(), theValue.end(), theValue.begin(),
		[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return theValue;
}

// The system directory that holds the BIOS-style game data: <system>/pvz/
std::string GetSystemPvzDir()
{
	const char* aSystemDir = nullptr;
	if (EnvironCb != nullptr && EnvironCb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &aSystemDir) &&
		aSystemDir != nullptr && aSystemDir[0] != '\0')
	{
		return JoinPath(aSystemDir, "pvz");
	}
	return "pvz";
}

// A directory is a usable resource directory when it holds both main.pak and a
// properties/ folder.  main.pak alone is accepted as well: the engine falls
// back to built-in English defaults when properties/ is absent.
bool LooksLikeResourceDir(const std::string& theDir, bool& hasProperties)
{
	hasProperties = IsDirectory(JoinPath(theDir, "properties"));
	return IsRegularFile(JoinPath(theDir, "main.pak"));
}

} // namespace

bool ResolveResources(const char* theContentPath, std::string& theError)
{
	ResourceDir.clear();
	PropertiesDir.clear();
	ResourceIsBios = false;
	ContentPath = (theContentPath != nullptr) ? theContentPath : "";

	// --- 1. Content-provided game data -------------------------------------
	// When content is loaded we look next to the given file.  A main.pak is
	// expected; properties/ is picked up from the same directory when present.
	// In this mode the BIOS is never consulted.
	if (!ContentPath.empty() && IsRegularFile(ContentPath))
	{
		const std::string aFileName = LowerCase(BaseName(ContentPath));
		std::string aDir = ParentDir(ContentPath);
		if (aDir.empty())
			aDir = ".";

		bool hasProperties = false;
		if (aFileName == "main.pak" || LooksLikeResourceDir(aDir, hasProperties))
		{
			ResourceDir = aDir;
			PropertiesDir = JoinPath(aDir, "properties");
			if (!LooksLikeResourceDir(aDir, hasProperties))
			{
				theError = "Content file '" + ContentPath +
					"' is not usable: expected main.pak in its directory.";
				return false;
			}
			LogInfo("[pvz] Using content-provided game data from '%s' (properties/: %s)\n",
				ResourceDir.c_str(), hasProperties ? "yes" : "no - using built-in defaults");
			return true;
		}

		theError = "Unsupported content '" + ContentPath +
			"': expected main.pak (or a directory containing it).";
		return false;
	}

	// --- 2. BIOS game data: <system>/pvz/ -----------------------------------
	const std::string aBiosDir = GetSystemPvzDir();
	bool hasProperties = false;
	if (LooksLikeResourceDir(aBiosDir, hasProperties))
	{
		ResourceDir = aBiosDir;
		PropertiesDir = JoinPath(aBiosDir, "properties");
		ResourceIsBios = true;
		LogInfo("[pvz] Using BIOS game data from '%s' (properties/: %s)\n",
			ResourceDir.c_str(), hasProperties ? "yes" : "no - using built-in defaults");
		return true;
	}

	theError = "No game data found. Place 'main.pak' and the 'properties' folder in '" +
		aBiosDir + "' (the libretro system directory + /pvz), or load a main.pak as content.";
	return false;
}

// ===========================================================================
// Hardware rendering
// ===========================================================================
static void OnContextReset(void);
static void OnContextDestroy(void);
static uintptr_t OnGetCurrentFramebuffer(void);
static retro_proc_address_t OnGetProcAddress(const char* theSymbol);

static void SetupHwRenderCallbacks()
{
	HwRender.context_reset = OnContextReset;
	HwRender.context_destroy = OnContextDestroy;
	HwRender.get_current_framebuffer = OnGetCurrentFramebuffer;
	HwRender.get_proc_address = OnGetProcAddress;
	HwRender.depth = false;
	HwRender.stencil = false;
	// The game renders with a y-down orthographic projection, which lands the
	// first game row at the top of the GL framebuffer: that is GL's native
	// orientation, so the frontend must not flip the image.
	HwRender.bottom_left_origin = true;
	HwRender.cache_context = false;
	// The render size is advertised through retro_get_system_av_info().geometry,
	// not on this struct (retro_hw_render_callback has no size fields any more).
}

bool RequestHardwareRender()
{
	// OpenGL ES 2.0 first: it is the game's native renderer.
	retro_hw_render_callback aRequest = {};
	aRequest.context_type = RETRO_HW_CONTEXT_OPENGLES2;
	aRequest.version_major = 2;
	aRequest.version_minor = 0;
	HwRender = aRequest;
	SetupHwRenderCallbacks();

	if (EnvironCb != nullptr && EnvironCb(RETRO_ENVIRONMENT_SET_HW_RENDER, &HwRender))
	{
		HwRenderEnabled = true;
		return true;
	}

	// Fallback: desktop OpenGL 2.1 compatibility profile.
	aRequest = {};
	aRequest.context_type = RETRO_HW_CONTEXT_OPENGL;
	aRequest.version_major = 2;
	aRequest.version_minor = 1;
	HwRender = aRequest;
	SetupHwRenderCallbacks();

	if (EnvironCb != nullptr && EnvironCb(RETRO_ENVIRONMENT_SET_HW_RENDER, &HwRender))
	{
		HwRenderEnabled = true;
		gDesktopGLFallback = true;
		return true;
	}

	LogError("[pvz] This core needs hardware rendering (OpenGL ES 2.0 or OpenGL 2.1). "
		"Set the frontend video driver to 'gl' or 'glcore' and restart the core.\n");
	return false;
}

static void* GlProcTrampoline(const char* theSymbol)
{
	if (HwRender.get_proc_address == nullptr)
		return nullptr;
	return reinterpret_cast<void*>(HwRender.get_proc_address(theSymbol));
}

bool LoadGLFunctions()
{
	int aVersion = gladLoadGLES2(reinterpret_cast<GLADloadfunc>(GlProcTrampoline));
	if (aVersion == 0)
	{
		LogError("[pvz] Failed to resolve OpenGL entry points through the frontend.\n");
		return false;
	}
	GlFunctionsLoaded = true;
	return true;
}

void BindFrontendFramebuffer()
{
	if (!HwRenderEnabled)
		return;

	if (HwRender.get_current_framebuffer != nullptr)
	{
		GLuint aFramebuffer = static_cast<GLuint>(HwRender.get_current_framebuffer());
		glBindFramebuffer(GL_FRAMEBUFFER, aFramebuffer);
	}
	glViewport(0, 0, static_cast<GLsizei>(sGameWidth), static_cast<GLsizei>(sGameHeight));
}

void ReapplyRendererState()
{
	if (!GlFunctionsLoaded)
		return;
	Sexy::GfxReapplyState();
}

static uintptr_t OnGetCurrentFramebuffer(void)
{
	return 0;
}

static retro_proc_address_t OnGetProcAddress(const char* theSymbol)
{
	return reinterpret_cast<retro_proc_address_t>(GlProcTrampoline(theSymbol));
}

static void OnContextDestroy(void)
{
	// The frontend is about to drop the GL context; every GL object the game
	// owns becomes invalid.  Reset the cached GL state so the next
	// context_reset() rebuilds it from scratch.
	GlFunctionsLoaded = false;
}

static void OnContextReset(void)
{
	if (!GameInitialized)
	{
		// First context: bring the whole game up now that GL is usable.
		if (!LoadGLFunctions())
		{
			ShutdownRequested = true;
			return;
		}
		if (gSexyAppBase == nullptr)
		{
			ShutdownRequested = true;
			return;
		}

		gSexyAppBase->Init();
		if (!gSexyAppBase->mInitialized)
		{
			LogError("[pvz] Game initialisation failed.\n");
			ShutdownRequested = true;
			return;
		}

		// The game owns the frame now; the frontend paces it.
		sGameWidth = static_cast<unsigned>(gSexyAppBase->mWidth);
		sGameHeight = static_cast<unsigned>(gSexyAppBase->mHeight);
		if (sGameWidth != 0 && sGameHeight != 0)
		{
			FrameWidth = sGameWidth;
			FrameHeight = sGameHeight;
		}

		// Never minimise/mute ourselves: the frontend decides when we run.
		gSexyAppBase->mActive = true;
		gSexyAppBase->mMinimized = false;
		gSexyAppBase->mPaused = false;
		gSexyAppBase->mYieldMainThread = false;
		gSexyAppBase->mSoftVSyncWait = false;
		gSexyAppBase->mVSyncUpdates = false;

		gSexyAppBase->Start();
		GameInitialized = true;
		LogInfo("[pvz] Game started (%ux%u).\n", FrameWidth, FrameHeight);
		return;
	}

	// Subsequent resets (video driver re-init, fullscreen toggle, ...).
	if (!LoadGLFunctions())
	{
		ShutdownRequested = true;
		return;
	}
	if (gSexyAppBase != nullptr && gSexyAppBase->mGLInterface != nullptr)
		gSexyAppBase->InitGLInterface();
}

// ===========================================================================
// Frame stepping
// ===========================================================================

// One game frame's worth of work, from the frontend's point of view.
// (Not in an anonymous namespace: CompletePendingFrame below is part of the
// backend's interface and needs external linkage.)
static void RunFrameBody()
{
	if (!GameInitialized || gSexyAppBase == nullptr)
		return;

	SexyAppBase* anApp = gSexyAppBase;

	if (anApp->mShutdown)
	{
		if (!ShutdownRequested)
		{
			LogInfo("[pvz] the game asked to shut down; telling the frontend.\n");
			ShutdownRequested = true;
			bool aOk = true;
			if (EnvironCb != nullptr)
				EnvironCb(RETRO_ENVIRONMENT_SHUTDOWN, &aOk);
		}
		return;
	}

	// The game must believe it is focused or it will pause itself.
	anApp->mActive = true;
	anApp->mMinimized = false;
	anApp->mPaused = false;

	anApp->mExitToTop = false;

	// The framework only repaints widgets it considers dirty and relies on the
	// framebuffer still holding everything else.  A libretro frontend makes no
	// such promise about the buffer it handed us, so when the game draws nothing
	// on a frame we ask for a full repaint on the next one.  Screens that are
	// already dirty every frame (the title screen marks itself dirty on every
	// tick) are left alone.
	static bool sFullRepaintNeeded = true;
	if (sFullRepaintNeeded && anApp->mWidgetManager != nullptr)
	{
		anApp->mWidgetManager->MarkAllDirty();
		sFullRepaintNeeded = false;
	}

	const uint aDrawsBefore = anApp->mDrawCount;

	// Mirrors the Emscripten requestAnimationFrame callback: complete every
	// pending stage so that exactly one frame is drawn per retro_run().
	if (!anApp->UpdateAppStep(nullptr))
		return;

	CompletePendingFrame();

	if (anApp->mDrawCount == aDrawsBefore)
		sFullRepaintNeeded = true;
}

void CompletePendingFrame()
{
	if (gSexyAppBase == nullptr)
		return;

	int aGuard = 0;
	while ((gSexyAppBase->mUpdateAppState != UPDATESTATE_PROCESS_DONE || gSexyAppBase->mHasPendingDraw) && aGuard++ < 256)
	{
		if (!gSexyAppBase->UpdateAppStep(nullptr))
			break;
	}
}

// ===========================================================================
// Cooperative blocking waits
// ===========================================================================
// The framework has exactly one place that blocks waiting for the player:
// Dialog::WaitForResult() spins until a button sets the dialog result.  A
// libretro core cannot simply spin there -- the frontend only composites and
// swaps after retro_run() returns, so the dialog would never be drawn, no new
// input would arrive, and the host would be marked unresponsive.
//
// The Emscripten build solves this with Asyncify: emscripten_sleep() suspends
// this call stack and lets the browser's rAF loop keep running.  Fibers give us
// the same thing natively -- the game runs on its own fiber, and a blocking wait
// switches back to the frontend fiber, so retro_run() returns normally (frame
// presented, host responsive) and the wait resumes on the next retro_run().
#ifdef _WIN32

namespace
{

void*	sFrontendFiber = nullptr;
void*	sGameFiber = nullptr;
bool	sGameFiberActive = false;

void CALLBACK GameFiberEntry(void*)
{
	sGameFiberActive = true;
	for (;;)
	{
		// One whole game frame.  This may hand control back early if the game
		// blocks in YieldToFrontend(); when it does, we simply resume here on a
		// later retro_run() and carry on inside that wait.
		//
		// Must be RunFrameBody(), not RunFrame(): RunFrame() is the switching
		// entry point and would send this fiber straight back to itself.
		RunFrameBody();

		// Frame finished: go back to retro_run() so it can present.
		SwitchToFiber(sFrontendFiber);
	}
}

} // namespace

// Returns false when fibers are unavailable, in which case the caller falls
// back to pumping frames while blocked.
static bool EnsureGameFiber()
{
	if (sGameFiber != nullptr)
		return true;

	if (sFrontendFiber == nullptr)
	{
		sFrontendFiber = ConvertThreadToFiber(nullptr);
		if (sFrontendFiber == nullptr)
		{
			LogError("[pvz] ConvertThreadToFiber failed (%lu); dialogs will block the frontend.\n",
				GetLastError());
			return false;
		}
	}

	// Generous stack: the game's dialog path re-enters the whole update chain.
	sGameFiber = CreateFiber(16u * 1024u * 1024u, GameFiberEntry, nullptr);
	if (sGameFiber == nullptr)
	{
		LogError("[pvz] CreateFiber failed (%lu); dialogs will block the frontend.\n",
			GetLastError());
		return false;
	}

	return true;
}

void YieldToFrontend()
{
	// Only meaningful from inside the game fiber; anywhere else fall back to the
	// blocking pump so the dialog still works.
	if (sGameFiber == nullptr || sFrontendFiber == nullptr || GetCurrentFiber() != sGameFiber)
	{
		PumpBlockingWait();
		return;
	}

	// While this fiber is parked, the frontend owns the thread and may ask for a
	// save state to be loaded.  Its C++ stack still points into the level that a
	// load replaces, so the load has to wait until we are back at frame level.
	PvzLibretro::SetFiberSuspended(true);
	SwitchToFiber(sFrontendFiber);
	PvzLibretro::SetFiberSuspended(false);
}

void ShutdownGameFiber()
{
	if (sGameFiber != nullptr)
	{
		// The fiber may be parked inside a wait; nothing to unwind, just drop it.
		DeleteFiber(sGameFiber);
		sGameFiber = nullptr;
	}
	sGameFiberActive = false;

	if (sFrontendFiber != nullptr)
	{
		ConvertFiberToThread();
		sFrontendFiber = nullptr;
	}
}

#else // !_WIN32

static bool EnsureGameFiber() { return false; }

void YieldToFrontend()
{
	PumpBlockingWait();
}

void ShutdownGameFiber()
{
}

#endif // _WIN32

// One call per retro_run(): the clock moves on by exactly one game frame.
void TickGameClock()
{
	if (!sClockStarted)
	{
		// Adopt the wall clock's epoch on the first frame.  The game's own baseline
		// (mLastTimeCheck) is seeded from SDL_GetTicks() while it initialises, and a
		// clock starting at 0 would look like seconds of *negative* elapsed time -
		// the update accumulator only clamps on the high side, so the game would sit
		// still until the two met.
		sGameTime = SDL_GetTicks();
		sClockStarted = true;
		return;
	}

	// One frame, to the millisecond, with the remainder kept so the clock and the
	// audio stay exactly in step.
	sFrameFraction += kGameFrameMsNumerator;
	sGameTime += sFrameFraction / kGameFrameMsDenominator;
	sFrameFraction %= kGameFrameMsDenominator;
}

uint32_t GameTimeMs()
{
	// Before the first frame (during loading) the wall clock is the honest answer.
	return sClockStarted ? sGameTime : SDL_GetTicks();
}

void RunFrame()
{
	if (!GameInitialized || gSexyAppBase == nullptr)
		return;

	// A save state the frontend loaded is applied here, where the game is
	// guaranteed to be between frames rather than parked in a dialog.
	ApplyPendingState();

	TickGameClock();

	if (!EnsureGameFiber())
	{
		RunFrameBody();
		return;
	}

	// Never switch to the fiber we are already running on.
	if (GetCurrentFiber() == sGameFiber)
	{
		RunFrameBody();
		return;
	}

	// Switch into the game fiber.  Control comes back either because the frame
	// finished or because the game is blocked in a dialog wait; either way
	// retro_run() presents what has been drawn and returns to the frontend.
	SwitchToFiber(sGameFiber);
}

void PresentFrame()
{
	if (VideoCb != nullptr)
		VideoCb(RETRO_HW_FRAME_BUFFER_VALID, FrameWidth, FrameHeight, 0);
}

void DrawCursor()
{
	// The game supplies no cursor artwork: under SDL it asks for a system cursor
	// (SDL_CreateSystemCursor) and lets the OS draw it, and EnforceCursor() is a
	// no-op here.  A mouse user still sees the frontend's own cursor, but a
	// gamepad user would be steering an invisible pointer -- so draw the classic
	// arrow ourselves, and only while the gamepad is the device moving it (a
	// second arrow drawn under the OS cursor would just look like a ghost).
	//
	// The option exists because that reasoning depends on the frontend actually
	// drawing a cursor of its own; where it does not, "always" is the only way
	// to get a pointer at all.
	const char* aCursorMode = OptionString("pvz_draw_cursor");
	if (std::strcmp(aCursorMode, "never") == 0)
		return;
	if (std::strcmp(aCursorMode, "always") != 0 && !sCursorFromPad)
		return;

	if (!GameInitialized || gSexyAppBase == nullptr ||
		gSexyAppBase->mGLInterface == nullptr)
		return;

	const int aCursorNum = gSexyAppBase->mCursorNum;
	if (aCursorNum < 0 || aCursorNum >= NUM_CURSORS || aCursorNum == CURSOR_NONE)
		return;

	Image* aScreen = gSexyAppBase->mGLInterface->GetScreenImage();
	if (aScreen == nullptr)
		return;

	// Windows-style arrow, hotspot at (0,0), drawn as a black outline with a
	// white body so it stays visible on any background.  Both are concave, hence
	// the scanline fill (PolyFill's convex flag is left false).
	static const Point kOutline[7] = {
		{ 0, 0 }, { 0, 16 }, { 4, 12 }, { 7, 19 }, { 10, 17 }, { 7, 11 }, { 12, 11 }
	};
	static const Point kBody[7] = {
		{ 2, 3 }, { 2, 12 }, { 4, 10 }, { 6, 15 }, { 8, 14 }, { 6, 9 }, { 9, 9 }
	};

	const int aX = static_cast<int>(sCursorX);
	const int aY = static_cast<int>(sCursorY);

	Graphics g(aScreen);
	g.SetClipRect(0, 0, static_cast<int>(FrameWidth), static_cast<int>(FrameHeight));
	g.SetDrawMode(Graphics::DRAWMODE_NORMAL);

	Point aPoints[7];
	for (int i = 0; i < 7; ++i)
		aPoints[i] = Point(aX + kOutline[i].mX, aY + kOutline[i].mY);
	g.SetColor(Color(0, 0, 0, 255));
	g.PolyFill(aPoints, 7);

	for (int i = 0; i < 7; ++i)
		aPoints[i] = Point(aX + kBody[i].mX, aY + kBody[i].mY);
	g.SetColor(Color(255, 255, 255, 255));
	g.PolyFill(aPoints, 7);
}

void PumpBlockingWait()
{
	if (!GameInitialized || ShutdownRequested)
		return;

	// Re-read the frontend input.  RetroArch's dinput driver samples the mouse
	// with GetCursorPos()/GetDeviceState() rather than window messages, so the
	// state is live even though the frontend is stuck inside retro_run().
	InputPollFrontend();

	// Present at most once per frame: the frontend composites and swaps inside
	// the video callback, so this is what actually puts the dialog on screen.
	// Presenting at the full spin rate would wreck its frame timing.
	static uint32_t sLastPresent = 0;
	const uint32_t aNow = SDL_GetTicks();
	if (sLastPresent == 0 || aNow - sLastPresent >= 16)
	{
		sLastPresent = aNow;
		PresentFrame();
	}

	AudioFlushToFrontend();

	// Keep the wait from spinning a core flat out.
	SDL_Delay(1);
}

// ===========================================================================
// Audio bridge
// ===========================================================================
void AudioStartCapture()
{
	sMixerReady = true;
	if (sAudioBuffer.size() < static_cast<size_t>(kAudioFramesPerRun) * 2)
		sAudioBuffer.resize(static_cast<size_t>(kAudioFramesPerRun) * 2, 0);
}

void AudioStopCapture()
{
	sMixerReady = false;
}

void AudioFlushToFrontend()
{
	if (AudioBatchCb == nullptr)
		return;
	if (sAudioBuffer.size() < static_cast<size_t>(kAudioFramesPerRun) * 2)
		sAudioBuffer.assign(static_cast<size_t>(kAudioFramesPerRun) * 2, 0);

	// Only mix once the loading thread has finished: it decodes sounds
	// concurrently and there is no SDL audio lock when mixing by hand.
	const bool aSafeToMix = sMixerReady && GameInitialized && gSexyAppBase != nullptr &&
		gSexyAppBase->mLoadingThreadCompleted.load();

	if (aSafeToMix)
	{
		Mix_CommonMixer_t aMixer = Mix_GetGeneralMixer();
		if (aMixer != nullptr)
		{
			aMixer(nullptr, reinterpret_cast<Uint8*>(sAudioBuffer.data()),
				static_cast<int>(kAudioFramesPerRun * 2 * sizeof(int16_t)));
		}
		else
			std::fill(sAudioBuffer.begin(), sAudioBuffer.end(), static_cast<int16_t>(0));
	}
	else
		std::fill(sAudioBuffer.begin(), sAudioBuffer.end(), static_cast<int16_t>(0));

	AudioBatchCb(sAudioBuffer.data(), kAudioFramesPerRun);
}

// ===========================================================================
// Input
// ===========================================================================
namespace
{

int16_t PadState(unsigned thePort, unsigned theDevice, unsigned theIndex, unsigned theId)
{
	if (InputStateCb == nullptr)
		return 0;
	return InputStateCb(thePort, theDevice, theIndex, theId);
}

bool PadButton(unsigned theId)
{
	return PadState(0, RETRO_DEVICE_JOYPAD, 0, theId) != 0;
}

float ApplyDeadzone(int16_t theValue)
{
	// Sticks age differently, so this is a core option rather than a constant:
	// too low and the cursor drifts on its own, too high and small movements
	// are ignored.
	const float aDeadzone = OptionFloat("pvz_gamepad_deadzone");
	float aNorm = static_cast<float>(theValue) / 32767.0f;
	if (std::fabs(aNorm) < aDeadzone)
		return 0.0f;
	return aNorm;
}

void PushMouseMove(int theX, int theY)
{
	InputEvent anEvent;
	anEvent.kind = InputEvent::MOVE;
	anEvent.x = theX;
	anEvent.y = theY;
	sEventQueue.push_back(anEvent);
}

void PushButton(bool theDown, int theButton)
{
	InputEvent anEvent;
	anEvent.kind = theDown ? InputEvent::DOWN : InputEvent::UP;
	anEvent.value = theButton;
	anEvent.x = static_cast<int>(sCursorX);
	anEvent.y = static_cast<int>(sCursorY);
	sEventQueue.push_back(anEvent);
}

} // namespace

// Maps a libretro keyboard keycode (enum retro_key) onto the framework's
// KeyCode, which follows the Windows virtual-key numbering.
static KeyCode RetroKeyToKeyCode(unsigned theKey)
{
	if (theKey >= RETROK_a && theKey <= RETROK_z)
		return static_cast<KeyCode>(theKey - RETROK_a + 'A');
	if (theKey >= RETROK_0 && theKey <= RETROK_9)
		return static_cast<KeyCode>(theKey);

	switch (theKey)
	{
		case RETROK_BACKSPACE:  return KEYCODE_BACK;
		case RETROK_TAB:        return KEYCODE_TAB;
		case RETROK_CLEAR:      return KEYCODE_CLEAR;
		case RETROK_RETURN:
		case RETROK_KP_ENTER:   return KEYCODE_RETURN;
		case RETROK_ESCAPE:     return KEYCODE_ESCAPE;
		case RETROK_SPACE:      return KEYCODE_SPACE;
		case RETROK_DELETE:     return KEYCODE_DELETE;

		case RETROK_LEFT:       return KEYCODE_LEFT;
		case RETROK_UP:         return KEYCODE_UP;
		case RETROK_RIGHT:      return KEYCODE_RIGHT;
		case RETROK_DOWN:       return KEYCODE_DOWN;

		case RETROK_INSERT:     return KEYCODE_INSERT;
		case RETROK_HOME:       return KEYCODE_HOME;
		case RETROK_END:        return KEYCODE_END;
		case RETROK_PAGEUP:     return KEYCODE_PRIOR;
		case RETROK_PAGEDOWN:   return KEYCODE_NEXT;

		case RETROK_LSHIFT:
		case RETROK_RSHIFT:     return KEYCODE_SHIFT;
		case RETROK_LCTRL:
		case RETROK_RCTRL:      return KEYCODE_CONTROL;
		case RETROK_LALT:
		case RETROK_RALT:       return KEYCODE_MENU;

		case RETROK_PAUSE:      return KEYCODE_PAUSE;
		case RETROK_CAPSLOCK:   return KEYCODE_CAPITAL;
		case RETROK_NUMLOCK:    return KEYCODE_NUMLOCK;
		case RETROK_SCROLLOCK:  return KEYCODE_SCROLL;

		case RETROK_KP0:        return KEYCODE_NUMPAD0;
		case RETROK_KP1:        return KEYCODE_NUMPAD1;
		case RETROK_KP2:        return KEYCODE_NUMPAD2;
		case RETROK_KP3:        return KEYCODE_NUMPAD3;
		case RETROK_KP4:        return KEYCODE_NUMPAD4;
		case RETROK_KP5:        return KEYCODE_NUMPAD5;
		case RETROK_KP6:        return KEYCODE_NUMPAD6;
		case RETROK_KP7:        return KEYCODE_NUMPAD7;
		case RETROK_KP8:        return KEYCODE_NUMPAD8;
		case RETROK_KP9:        return KEYCODE_NUMPAD9;
		case RETROK_KP_MULTIPLY:return KEYCODE_MULTIPLY;
		case RETROK_KP_PLUS:    return KEYCODE_ADD;
		case RETROK_KP_MINUS:   return KEYCODE_SUBTRACT;
		case RETROK_KP_PERIOD:  return KEYCODE_DECIMAL;
		case RETROK_KP_DIVIDE:  return KEYCODE_DIVIDE;

		case RETROK_F1:         return KEYCODE_F1;
		case RETROK_F2:         return KEYCODE_F2;
		case RETROK_F3:         return KEYCODE_F3;
		case RETROK_F4:         return KEYCODE_F4;
		case RETROK_F5:         return KEYCODE_F5;
		case RETROK_F6:         return KEYCODE_F6;
		case RETROK_F7:         return KEYCODE_F7;
		case RETROK_F8:         return KEYCODE_F8;
		case RETROK_F9:         return KEYCODE_F9;
		case RETROK_F10:        return KEYCODE_F10;
		case RETROK_F11:        return KEYCODE_F11;
		case RETROK_F12:        return KEYCODE_F12;

		default:                return KEYCODE_UNKNOWN;
	}
}

static void PushText(uint32_t theCodepoint)
{
	if (theCodepoint == 0)
		return;

	// Encode as UTF-8: the widget layer works with UTF-8 strings.
	std::string aText;
	if (theCodepoint < 0x80)
		aText += static_cast<char>(theCodepoint);
	else if (theCodepoint < 0x800)
	{
		aText += static_cast<char>(0xC0 | (theCodepoint >> 6));
		aText += static_cast<char>(0x80 | (theCodepoint & 0x3F));
	}
	else if (theCodepoint < 0x10000)
	{
		aText += static_cast<char>(0xE0 | (theCodepoint >> 12));
		aText += static_cast<char>(0x80 | ((theCodepoint >> 6) & 0x3F));
		aText += static_cast<char>(0x80 | (theCodepoint & 0x3F));
	}
	else
	{
		aText += static_cast<char>(0xF0 | (theCodepoint >> 18));
		aText += static_cast<char>(0x80 | ((theCodepoint >> 12) & 0x3F));
		aText += static_cast<char>(0x80 | ((theCodepoint >> 6) & 0x3F));
		aText += static_cast<char>(0x80 | (theCodepoint & 0x3F));
	}

	InputEvent anEvent;
	anEvent.kind = InputEvent::TEXT;
	anEvent.text = std::move(aText);
	sEventQueue.push_back(anEvent);
}

// Keys that only ever produce a key code: text editing and dialog navigation.
static const unsigned kControlKeys[] = {
	RETROK_BACKSPACE, RETROK_TAB, RETROK_RETURN, RETROK_KP_ENTER, RETROK_ESCAPE, RETROK_DELETE,
	RETROK_LEFT, RETROK_UP, RETROK_RIGHT, RETROK_DOWN,
	RETROK_INSERT, RETROK_HOME, RETROK_END, RETROK_PAGEUP, RETROK_PAGEDOWN,
	RETROK_LSHIFT, RETROK_RSHIFT, RETROK_LCTRL, RETROK_RCTRL, RETROK_LALT, RETROK_RALT,
	RETROK_F1, RETROK_F2, RETROK_F3, RETROK_F4, RETROK_F5, RETROK_F6,
	RETROK_F7, RETROK_F8, RETROK_F9, RETROK_F10, RETROK_F11, RETROK_F12,
};

// Printable keys and the character each one types on a US layout.
struct PrintableKey
{
	unsigned	key;
	uint32_t	plain;
	uint32_t	shifted;
};

static const PrintableKey kPrintableKeys[] = {
	{ RETROK_SPACE,        ' ',  ' '  },
	{ RETROK_0,            '0',  ')'  },
	{ RETROK_1,            '1',  '!'  },
	{ RETROK_2,            '2',  '@'  },
	{ RETROK_3,            '3',  '#'  },
	{ RETROK_4,            '4',  '$'  },
	{ RETROK_5,            '5',  '%'  },
	{ RETROK_6,            '6',  '^'  },
	{ RETROK_7,            '7',  '&'  },
	{ RETROK_8,            '8',  '*'  },
	{ RETROK_9,            '9',  '('  },
	{ RETROK_MINUS,        '-',  '_'  },
	{ RETROK_EQUALS,       '=',  '+'  },
	{ RETROK_LEFTBRACKET,  '[',  '{'  },
	{ RETROK_RIGHTBRACKET, ']',  '}'  },
	{ RETROK_BACKSLASH,    '\\', '|'  },
	{ RETROK_SEMICOLON,    ';',  ':'  },
	{ RETROK_QUOTE,        '\'', '"'  },
	{ RETROK_BACKQUOTE,    '`',  '~'  },
	{ RETROK_COMMA,        ',',  '<'  },
	{ RETROK_PERIOD,       '.',  '>'  },
	{ RETROK_SLASH,        '/',  '?'  },
};

static void QueuePolledKey(unsigned theKey, uint32_t theCharacter)
{
	const bool aDown = PadState(0, RETRO_DEVICE_KEYBOARD, 0, theKey) != 0;
	if (aDown == sKeyDown[theKey])
		return;

	sKeyDown[theKey] = aDown;
	sKeySynthesized[theKey] = aDown;

	InputEvent anEvent;
	anEvent.kind = aDown ? InputEvent::KEYDOWN : InputEvent::KEYUP;
	anEvent.value = static_cast<int>(RetroKeyToKeyCode(theKey));
	sEventQueue.push_back(anEvent);

	if (aDown && theCharacter != 0)
		PushText(theCharacter);
}

// Frontends are free to consume keyboard input for their own bindings before the
// core ever sees it: RetroArch forwards nothing for a key that is bound to a
// RetroPad button or to a hotkey unless Game Focus Mode is enabled (and auto
// game focus defaults to off), so keys such as A/Q/W/E/R/S/X/Z, the arrow keys
// and Enter are swallowed.  That would silently drop characters while a profile
// name is typed, so every key is polled here as well.
//
// The raw RETRO_DEVICE_KEYBOARD state is not filtered that way, hence a press the
// callback did not deliver is synthesized from it, character included.  This is
// also what makes an on-screen/overlay keyboard usable: it only reports key
// states and never characters.  The two paths cannot double-fire, because the
// callback keeps sKeyDown[] in sync and sKeySynthesized[] marks a press that the
// poll already delivered.
void PollKeyboardKeys()
{
	if (InputStateCb == nullptr)
		return;

	const bool aShift = PadState(0, RETRO_DEVICE_KEYBOARD, 0, RETROK_LSHIFT) != 0 ||
	                    PadState(0, RETRO_DEVICE_KEYBOARD, 0, RETROK_RSHIFT) != 0;
	const bool aCapsLock = PadState(0, RETRO_DEVICE_KEYBOARD, 0, RETROK_CAPSLOCK) != 0;
	const bool aUpper = (aShift != aCapsLock);

	for (unsigned aKey : kControlKeys)
		QueuePolledKey(aKey, 0);

	for (unsigned aKey = RETROK_a; aKey <= RETROK_z; ++aKey)
		QueuePolledKey(aKey, aUpper ? (aKey - RETROK_a + 'A') : aKey);

	for (const PrintableKey& aKey : kPrintableKeys)
		QueuePolledKey(aKey.key, aShift ? aKey.shifted : aKey.plain);
}

void KeyboardEventCallback(bool theDown, unsigned theKeycode, uint32_t theCharacter, uint16_t theModifiers)
{
	(void)theModifiers;
	if (theKeycode >= RETROK_LAST)
		return;

	// Already delivered from the raw state by PollKeyboardKeys(): the same
	// physical press is arriving late, so do not deliver (or type) it twice.
	if (theDown && sKeySynthesized[theKeycode])
	{
		sKeyDown[theKeycode] = true;
		return;
	}

	sKeySynthesized[theKeycode] = false;
	sKeyDown[theKeycode] = theDown;

	InputEvent anEvent;
	anEvent.kind = theDown ? InputEvent::KEYDOWN : InputEvent::KEYUP;
	anEvent.value = static_cast<int>(RetroKeyToKeyCode(theKeycode));
	sEventQueue.push_back(anEvent);

	if (theDown && theCharacter != 0)
		PushText(theCharacter);
}

bool FrontendHasKeyboard()
{
	// The capabilities belong to the input driver, so one answer is enough.
	static int sHasKeyboard = -1;
	if (sHasKeyboard < 0)
	{
		uint64_t aCapabilities = 0;
		if (EnvironCb != nullptr && EnvironCb(RETRO_ENVIRONMENT_GET_INPUT_DEVICE_CAPABILITIES, &aCapabilities))
			sHasKeyboard = (aCapabilities & (1ull << RETRO_DEVICE_KEYBOARD)) != 0;
		else
			sHasKeyboard = true;	// no answer: assume a keyboard rather than invent a name
	}

	return sHasKeyboard != 0;
}

void InputPollFrontend()
{
	if (InputPollCb != nullptr)
		InputPollCb();

	// --- keyboard ----------------------------------------------------------
	PollKeyboardKeys();

	// --- pointer / mouse ---------------------------------------------------
	int aTargetX = static_cast<int>(sCursorX);
	int aTargetY = static_cast<int>(sCursorY);
	bool aMouseDown = false;
	bool aMousePresent = false;
	bool aPointerMoved = false;	// the pointing device is being used right now

	// The frontend lets the player pick the port device from the list the core
	// declares (see retro_set_controller_port_device), which is the explicit
	// answer to "mouse or gamepad?".  RETRO_DEVICE_JOYPAD is also merely the
	// default when nothing was chosen, so it is treated as a preference rather
	// than a lock: a resting mouse never steals the cursor, but real mouse
	// activity still takes over.
	const bool aWantsMouse = (sPortDevice == RETRO_DEVICE_MOUSE || sPortDevice == RETRO_DEVICE_POINTER);

	if (InputStateCb != nullptr)
	{
		// Every id is read exactly once per frame: frontends treat some of them
		// (the wheel in particular) as one-shot pulses that the read consumes.
		const int16_t aPointerX = PadState(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_X);
		const int16_t aPointerY = PadState(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_Y);
		const bool aPressed = PadState(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_PRESSED) != 0;
		const bool aOffscreen = PadState(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_IS_OFFSCREEN) != 0;

		const int16_t aDeltaX = PadState(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_X);
		const int16_t aDeltaY = PadState(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_Y);
		const bool aMouseLeft = PadState(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_LEFT) != 0;
		const bool aMouseRight = PadState(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_RIGHT) != 0;
		const int16_t aWheelUp = PadState(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_WHEELUP);
		const int16_t aWheelDown = PadState(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_WHEELDOWN);

		const int aWheel = (aWheelUp != 0) ? 1 : ((aWheelDown != 0) ? -1 : 0);

		// The absolute pointer is the primary source: RetroArch maps the mouse
		// onto RETRO_DEVICE_POINTER.  Note that RETRO_DEVICE_ID_POINTER_COUNT
		// must NOT be used to decide whether a pointer is available -- several
		// frontends (RetroArch's dinput driver among them) do not implement it
		// and simply return 0, which would push us onto the relative path even
		// though an absolute pointer is right there.
		if (aPointerX != 0 || aPointerY != 0 || aPressed)
			sPointerSeen = true;

		if (sPointerSeen)
		{
			// Treat it as "the mouse is being used" only when something actually
			// changes, so a resting mouse does not keep stealing the cursor back
			// from a gamepad.
			if (aPointerX != sLastPointerX || aPointerY != sLastPointerY || aPressed)
				aPointerMoved = true;
			sLastPointerX = aPointerX;
			sLastPointerY = aPointerY;

			if (aPointerMoved && !aOffscreen)
			{
				aTargetX = static_cast<int>((static_cast<int32_t>(aPointerX) + 32768) *
					static_cast<int32_t>(FrameWidth) / 65536);
				aTargetY = static_cast<int>((static_cast<int32_t>(aPointerY) + 32768) *
					static_cast<int32_t>(FrameHeight) / 65536);
				aTargetX = std::clamp(aTargetX, 0, static_cast<int>(FrameWidth) - 1);
				aTargetY = std::clamp(aTargetY, 0, static_cast<int>(FrameHeight) - 1);
				sCursorX = static_cast<float>(aTargetX);
				sCursorY = static_cast<float>(aTargetY);
			}
		}
		else if (aDeltaX != 0 || aDeltaY != 0 || aMouseLeft || aMouseRight)
		{
			// Frontends that only expose a grabbed/relative mouse.
			aPointerMoved = true;
			sCursorX = std::clamp(sCursorX + aDeltaX, 0.0f, static_cast<float>(FrameWidth) - 1.0f);
			sCursorY = std::clamp(sCursorY + aDeltaY, 0.0f, static_cast<float>(FrameHeight) - 1.0f);
			aTargetX = static_cast<int>(sCursorX);
			aTargetY = static_cast<int>(sCursorY);
		}

		if (aPointerMoved)
			sCursorFromPad = false;

		if (aWheel != 0)
		{
			InputEvent aWheelEvent;
			aWheelEvent.kind = InputEvent::WHEEL;
			aWheelEvent.value = aWheel;
			sEventQueue.push_back(aWheelEvent);
		}

		if (aMouseRight != sRightWasDown)
		{
			sRightWasDown = aMouseRight;
			PushButton(aMouseRight, -1);
		}

		// Whichever device owns the cursor also owns the primary button.
		aMouseDown = sPointerSeen ? aPressed : aMouseLeft;
	}

	// --- gamepad driving the virtual cursor --------------------------------
	const uint64_t aButtons =
		(PadButton(RETRO_DEVICE_ID_JOYPAD_UP) ? 1ull << 0 : 0ull) |
		(PadButton(RETRO_DEVICE_ID_JOYPAD_DOWN) ? 1ull << 1 : 0ull) |
		(PadButton(RETRO_DEVICE_ID_JOYPAD_LEFT) ? 1ull << 2 : 0ull) |
		(PadButton(RETRO_DEVICE_ID_JOYPAD_RIGHT) ? 1ull << 3 : 0ull) |
		(PadButton(RETRO_DEVICE_ID_JOYPAD_A) ? 1ull << 4 : 0ull) |
		(PadButton(RETRO_DEVICE_ID_JOYPAD_B) ? 1ull << 5 : 0ull) |
		(PadButton(RETRO_DEVICE_ID_JOYPAD_START) ? 1ull << 6 : 0ull) |
		(PadButton(RETRO_DEVICE_ID_JOYPAD_SELECT) ? 1ull << 7 : 0ull) |
		(PadButton(RETRO_DEVICE_ID_JOYPAD_L) ? 1ull << 8 : 0ull) |
		(PadButton(RETRO_DEVICE_ID_JOYPAD_R) ? 1ull << 9 : 0ull);

	const int16_t aStickX = PadState(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X);
	const int16_t aStickY = PadState(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y);
	const bool aPadActive = (aButtons != 0) || aStickX != 0 || aStickY != 0;

	// The pad takes over whenever it is actually being used, even if a mouse is
	// plugged in -- otherwise a connected mouse would lock the pad out entirely.
	// An explicit Mouse/Pointer selection disables the pad cursor completely.
	if (!aWantsMouse && !aPointerMoved && aPadActive)
	{
		sCursorFromPad = true;

		// The pad is the only pointer a gamepad player has, so its speed is a
		// core option: the default needs over two seconds to cross the screen.
		const float aSpeed = static_cast<float>(OptionInt("pvz_cursor_speed"));
		float aMoveX = ApplyDeadzone(aStickX);
		float aMoveY = ApplyDeadzone(aStickY);
		if ((aButtons & (1ull << 2)) != 0) aMoveX = -1.0f;
		if ((aButtons & (1ull << 3)) != 0) aMoveX = 1.0f;
		if ((aButtons & (1ull << 0)) != 0) aMoveY = -1.0f;
		if ((aButtons & (1ull << 1)) != 0) aMoveY = 1.0f;

		if (aMoveX != 0.0f || aMoveY != 0.0f)
		{
			sCursorX = std::clamp(sCursorX + aMoveX * aSpeed, 0.0f, static_cast<float>(FrameWidth) - 1.0f);
			sCursorY = std::clamp(sCursorY + aMoveY * aSpeed, 0.0f, static_cast<float>(FrameHeight) - 1.0f);
		}
		aTargetX = static_cast<int>(sCursorX);
		aTargetY = static_cast<int>(sCursorY);
		aMouseDown = (aButtons & (1ull << 4)) != 0;
		aMousePresent = true;
	}

	// --- emit mouse events -------------------------------------------------
	// Always keep the game's pointer position fresh, whichever device moved it.
	if (aMousePresent || aPointerMoved || aPadActive || sMouseIn)
	{
		PushMouseMove(aTargetX, aTargetY);
		if (aMouseDown != sPointerWasDown)
		{
			PushButton(aMouseDown, 1);
			sPointerWasDown = aMouseDown;
		}
	}

	// --- joypad shortcuts --------------------------------------------------
	const uint64_t aChanged = aButtons ^ sPrevPadButtons;
	if (aChanged != 0)
	{
		if ((aChanged & (1ull << 5)) != 0)
			PushButton((aButtons & (1ull << 5)) != 0, -1);		// B -> right click
		if ((aChanged & (1ull << 6)) != 0)
		{
			InputEvent anEvent;
			anEvent.kind = ((aButtons & (1ull << 6)) != 0) ? InputEvent::KEYDOWN : InputEvent::KEYUP;
			anEvent.value = KEYCODE_ESCAPE;						// Start -> Escape
			sEventQueue.push_back(anEvent);
		}
		if ((aChanged & (1ull << 7)) != 0)
		{
			InputEvent anEvent;
			anEvent.kind = ((aButtons & (1ull << 7)) != 0) ? InputEvent::KEYDOWN : InputEvent::KEYUP;
			anEvent.value = KEYCODE_RETURN;						// Select -> Enter
			sEventQueue.push_back(anEvent);
		}
		if ((aChanged & (1ull << 8)) != 0 && (aButtons & (1ull << 8)) != 0)
		{
			InputEvent anEvent;
			anEvent.kind = InputEvent::WHEEL;
			anEvent.value = 1;									// L -> wheel up
			sEventQueue.push_back(anEvent);
		}
		if ((aChanged & (1ull << 9)) != 0 && (aButtons & (1ull << 9)) != 0)
		{
			InputEvent anEvent;
			anEvent.kind = InputEvent::WHEEL;
			anEvent.value = -1;									// R -> wheel down
			sEventQueue.push_back(anEvent);
		}
	}
	sPrevPadButtons = aButtons;
}

bool InputDispatchOne()
{
	if (gSexyAppBase == nullptr || gSexyAppBase->mWidgetManager == nullptr)
	{
		sEventQueue.clear();
		return false;
	}

	WidgetManager* aManager = gSexyAppBase->mWidgetManager.get();

	while (!sEventQueue.empty())
	{
		const InputEvent anEvent = sEventQueue.front();
		sEventQueue.pop_front();
		gSexyAppBase->mLastUserInputTick = gSexyAppBase->mLastTimerTime;

		switch (anEvent.kind)
		{
			case InputEvent::MOVE:
			{
				if (!sMouseIn)
				{
					sMouseIn = true;
					gSexyAppBase->mMouseIn = true;
				}
				int x = anEvent.x;
				int y = anEvent.y;
				aManager->RemapMouse(x, y);
				aManager->MouseMove(x, y);
				break;
			}
			case InputEvent::DOWN:
			{
				if (!sMouseIn)
				{
					sMouseIn = true;
					gSexyAppBase->mMouseIn = true;
				}
				int x = anEvent.x;
				int y = anEvent.y;
				aManager->RemapMouse(x, y);
				aManager->MouseMove(x, y);
				aManager->MouseDown(x, y, anEvent.value);
				break;
			}
			case InputEvent::UP:
			{
				int x = anEvent.x;
				int y = anEvent.y;
				aManager->RemapMouse(x, y);
				aManager->MouseMove(x, y);
				aManager->MouseUp(x, y, anEvent.value);
				break;
			}
			case InputEvent::WHEEL:
				aManager->MouseWheel(anEvent.value);
				break;
			case InputEvent::KEYDOWN:
				aManager->KeyDown(static_cast<KeyCode>(anEvent.value));
				break;
			case InputEvent::KEYUP:
				aManager->KeyUp(static_cast<KeyCode>(anEvent.value));
				break;
			case InputEvent::TEXT:
				aManager->KeyText(std::string_view(anEvent.text));
				break;
		}
	}
	return false;
}

void RequestShutdown()
{
	ShutdownRequested = true;
}

void SetPortDevice(unsigned theDevice)
{
	// Called from retro_set_controller_port_device(): the frontend is telling us
	// which device type the player selected for port 0, chosen from the list we
	// declared through RETRO_ENVIRONMENT_SET_CONTROLLER_INFO.
	sPortDevice = theDevice;

	// An explicit Mouse/Pointer choice means the frontend already shows a cursor,
	// so the core must stop drawing its own.
	if (theDevice == RETRO_DEVICE_MOUSE || theDevice == RETRO_DEVICE_POINTER)
		sCursorFromPad = false;
}

} // namespace PvzLibretro

// Hook used by PlatformGLInit() (graphics/GLPlatform.h): the libretro frontend
// owns the GL context, so the entry points come from its get_proc_address.
void LibretroLoadGLFunctions()
{
	PvzLibretro::LoadGLFunctions();
}

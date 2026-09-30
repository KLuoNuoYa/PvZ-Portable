/*
 * Copyright (C) 2026 PvZ libretro contributors
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * This file is part of PvZ-Portable's libretro port.
 *
 * Shared state between the libretro platform backend translation units and the
 * retro_* entry points.
 */

#ifndef __PVZ_LIBRETRO_BACKEND_H__
#define __PVZ_LIBRETRO_BACKEND_H__

// libretro.h deliberately leaves RETRO_API to the core/build system: on Windows
// the retro_* entry points have to be exported from the core DLL.
#ifndef RETRO_API
	#if defined(_WIN32)
		#define RETRO_API __declspec(dllexport)
	#elif defined(__GNUC__)
		#define RETRO_API __attribute__((visibility("default")))
	#else
		#define RETRO_API
	#endif
#endif

#include "libretro.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace PvzLibretro
{

// ---------------------------------------------------------------- callbacks --
extern retro_environment_t			EnvironCb;
extern retro_video_refresh_t		VideoCb;
extern retro_audio_sample_t			AudioSampleCb;
extern retro_audio_sample_batch_t	AudioBatchCb;
extern retro_input_poll_t			InputPollCb;
extern retro_input_state_t			InputStateCb;
extern retro_log_printf_t			LogCb;

void Log(retro_log_level theLevel, const char* theFormat, ...);
void LogInfo(const char* theFormat, ...);
void LogError(const char* theFormat, ...);

// ------------------------------------------------------------ core lifecycle --
extern bool		GameLoaded;			// retro_load_game() succeeded
extern bool		ShutdownRequested;	// the game asked to quit (or init failed)

// Resource resolution (main.pak + properties/).
// Set by retro_load_game(); consumed by the window backend at Init() time.
extern std::string	ResourceDir;		// directory holding main.pak
extern std::string	PropertiesDir;		// directory holding properties/ (may differ)
extern bool			ResourceIsBios;		// true when the resource dir came from system/pvz
extern std::string	SaveDir;
extern std::string	ContentPath;

// Resolve resources and remember the outcome. Called from retro_load_game().
// Returns false (with a message placed in theError) when no usable game data
// could be located.
bool ResolveResources(const char* theContentPath, std::string& theError);

// --------------------------------------------------------- hardware rendering --
// GLES2 is requested first; when the frontend rejects it we retry with a
// desktop GL 2.1 compatibility context (the game supports both).
extern retro_hw_render_callback	HwRender;
extern bool		HwRenderEnabled;
extern bool		GlFunctionsLoaded;
extern bool		GameInitialized;	// SexyAppBase::Init() has run
extern unsigned	FrameWidth;
extern unsigned	FrameHeight;

// Ask the frontend for a GL context. Must run inside retro_load_game().
bool RequestHardwareRender();

// Resolve the GL entry points through the frontend's get_proc_address hook.
// Must run with the frontend's context current.
bool LoadGLFunctions();

// Rebind the frontend's framebuffer and set a full-frame viewport.
void BindFrontendFramebuffer();

// Re-establish the GL state the game's renderer depends on.  Frontends are
// free to leave the context in whatever state they like between frames.
void ReapplyRendererState();

// Drive exactly one game frame (mirrors the Emscripten rAF callback).
void RunFrame();

// Completes the frame currently in progress: steps the app until it reaches
// UPDATESTATE_PROCESS_DONE with nothing left to draw.  The app's update state
// machine alternates between message handling and processing, so a single
// UpdateAppStep() is only half a frame.  Both the normal frame path and the
// blocking-wait path use this, so a dialog wait still produces one complete,
// presentable frame per retro_run().
void CompletePendingFrame();

// Present the frame just rendered through the video refresh callback.
void PresentFrame();

// Draws the core's own mouse pointer over the frame, but only while the gamepad
// is the device driving it (see the implementation for why).  Call after
// RunFrame() and before PresentFrame().
void DrawCursor();

// Pump one step of a blocking in-game wait (Dialog::WaitForResult spins until a
// button sets the dialog result).  This is the fallback used when the game is
// not running on a fiber; it re-reads input, presents at most once per frame,
// forwards audio and yields the CPU briefly.
void PumpBlockingWait();

// Hand control back to the frontend from inside a blocking wait, so retro_run()
// can present the frame and return while the wait stays suspended.  Call this
// from the wait loop; when the game is not running on a fiber it degrades to
// PumpBlockingWait().
void YieldToFrontend();

// Releases the cooperative-wait fiber.  Called from retro_unload_game().
void ShutdownGameFiber();

// ---------------------------------------------------------------- game clock --
// The clock the game itself reads (SexyAppBase::UpdateFTimeAcc).  Normal play
// follows the wall clock; while the frontend is fast-forwarding it advances by
// exactly one 60 Hz game frame per retro_run() instead.  The audio bridge renders
// exactly one frame of samples per retro_run(), so a wall-clock game falls behind
// the soundtrack it is playing - which is why fast-forward used to speed up the
// BGM without speeding up the game.
void		TickGameClock();	// once per retro_run(), from RunFrame()
uint32_t	GameTimeMs();

// --------------------------------------------------------------- audio bridge --
// The game mixes through SDL_mixer. We run SDL's dummy audio driver (so the
// mixer's callback fires on a paced thread) and tap the final mix with
// Mix_SetPostMix(), forwarding it to the libretro audio batch callback.
void AudioStartCapture();
void AudioStopCapture();
void AudioFlushToFrontend();

// -------------------------------------------------------------------- input --
// Poll the frontend once per frame and cache the resulting pad/pointer state.
void InputPollFrontend();
// Feed one queued input event into the widget manager.
// Returns true while more events remain for this frame.
bool InputDispatchOne();

// Registered with the frontend through RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK.
// It delivers typed characters directly, but frontends may swallow keys that are
// bound to their own controls, so the core also polls the raw keyboard state
// (see PollKeyboardKeys in LibretroBackend.cpp).
void KeyboardEventCallback(bool theDown, unsigned theKeycode, uint32_t theCharacter, uint16_t theModifiers);

// False only when the frontend answers RETRO_ENVIRONMENT_GET_INPUT_DEVICE_CAPABILITIES
// without RETRO_DEVICE_KEYBOARD, i.e. it cannot deliver typed text at all.
// A frontend that does not answer the query is assumed to have a keyboard.
bool FrontendHasKeyboard();

// Request the game to shut down (frontend asked us to unload).
void RequestShutdown();

// Records the device type the frontend selected for port 0 (called from
// retro_set_controller_port_device).  Drives the mouse-vs-gamepad choice for
// the pointer and whether the core draws its own cursor.
void SetPortDevice(unsigned theDevice);

} // namespace PvzLibretro

#endif // __PVZ_LIBRETRO_BACKEND_H__

/*
 * Copyright (C) 2026 PvZ libretro contributors
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * This file is part of PvZ-Portable's libretro port.
 *
 * Core options: settings the game has no UI for, or that only make sense in a
 * libretro frontend.  PvZ-Portable exposes most of its own switches as build
 * options (PVZ_DEBUG, DO_FIX_BUGS, LOW_MEMORY) or command line parameters,
 * neither of which a frontend can reach, so the player-facing ones are
 * republished here as core options.
 */

#ifndef __PVZ_LIBRETRO_OPTIONS_H__
#define __PVZ_LIBRETRO_OPTIONS_H__

namespace PvzLibretro
{

// Publishes this core's option definitions to the frontend.  Must run from
// retro_set_environment(), while the frontend is still collecting them.
void RegisterOptions();

// Re-reads the values when the frontend reports that they changed, and applies
// the ones the game picks up at runtime.  Called once per frame.
void RefreshOptions();

// Current value of an option, by its key.  Unknown keys and any value that is
// not one of the "on" spellings return false, so a missing option is the same
// as the disabled default.
bool OptionBool(const char* theKey);

// Same, for numeric and free-form options.  Unknown keys yield 0 / "".
int			OptionInt(const char* theKey);
float		OptionFloat(const char* theKey);
const char*	OptionString(const char* theKey);

} // namespace PvzLibretro

#endif // __PVZ_LIBRETRO_OPTIONS_H__

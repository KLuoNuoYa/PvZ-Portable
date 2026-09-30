/*
 * Copyright (C) 2026 PvZ libretro contributors
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * This file is part of PvZ-Portable's libretro port.
 *
 * Save states.
 *
 * The game already snapshots a level in progress: "save and exit", and the save
 * written when the program closes, both run the whole board through a chunked
 * serialiser (Lawn/System/SaveGame.cpp).  That snapshot happens to be everything
 * a save state needs - plants, zombies, projectiles, particles, reanimations,
 * the RNG seed, the wave counters - so a state here is that same image, kept in
 * memory instead of on disk, with a small header for what the image leaves out.
 *
 * What is deliberately *not* part of a state: the player profile (coins, level
 * progress, achievements).  A state rolls the level back, never the save file,
 * so loading one cannot duplicate or lose progress.  What is not covered either
 * is the world outside a level (menus, the almanac, the Zen Garden), which is why
 * saving is refused there.
 */

#ifndef __PVZ_LIBRETRO_SAVESTATE_H__
#define __PVZ_LIBRETRO_SAVESTATE_H__

#include <cstddef>

namespace PvzLibretro
{

// Upper bound for one state; the frontend allocates this before calling
// SaveState().  Constant by contract, whatever is on screen.
size_t SaveStateSize();

// Snapshot the level in progress into the caller's buffer.  Fails (without
// writing anything) outside a level.
bool SaveState(void* theData, size_t theSize);

// Accept a state from the frontend.  Only the format is checked here: the
// restore itself is deferred to ApplyPendingState(), because the frontend may
// call this while the game is parked inside a modal dialog whose C++ stack still
// points at the objects being replaced.
bool LoadState(const void* theData, size_t theSize);

// Performs a load the frontend asked for.  Called once per frame from
// RunFrame(), the only place the game is known to be between dialogs.
void ApplyPendingState();

// Set while the game fiber is parked inside a blocking wait (see
// YieldToFrontend): no state may be applied until it is back at frame level.
void SetFiberSuspended(bool theSuspended);

} // namespace PvzLibretro

#endif // __PVZ_LIBRETRO_SAVESTATE_H__

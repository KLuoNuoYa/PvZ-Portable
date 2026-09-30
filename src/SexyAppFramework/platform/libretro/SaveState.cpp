/*
 * Copyright (C) 2026 PvZ libretro contributors
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * This file is part of PvZ-Portable's libretro port.
 */

#include "SaveState.h"

#include "LibretroBackend.h"

#include "ConstEnums.h"
#include "LawnApp.h"
#include "Lawn/Board.h"
#include "Lawn/System/Music.h"
#include "Lawn/System/SaveGame.h"
#include "PvzpLib/EffectSystem.h"

#include <cstring>
#include <vector>

namespace PvzLibretro
{

namespace
{

// A frontend hands the core *the size it announced* - RetroArch stores the whole
// retro_serialize_size() block and passes exactly that back - so the two must not
// be required to match: the image inside is almost always far smaller.  Real
// mid-level saves measure tens of KB (a level-one .v4 is 28 KB), so 1 MiB is a
// comfortable ceiling that still keeps each state file small.
constexpr size_t kCapacity = 1u * 1024u * 1024u;

constexpr uint32_t kMagic = 0x535A5650u;	// "PVZS"
constexpr uint32_t kVersion = 1u;

// Everything a state needs beyond the level image.  The image carries the whole
// board, but the game mode and the board result live on the app, and losing them
// would leave the restored level running under whatever mode the player is in
// now - or worse, still flagged as "just lost".
struct StateHeader
{
	uint32_t	mMagic;
	uint32_t	mVersion;
	uint32_t	mImageSize;		// bytes of .v4 image that follow
	uint32_t	mBoardResult;	// LawnApp::mBoardResult
	uint32_t	mGameMode;		// LawnApp::mGameMode
	uint32_t	mLevel;			// Board::mLevel, for diagnostics only
	uint32_t	mReserved[2];
};

std::vector<unsigned char>	sPendingImage;
StateHeader					sPendingHeader = {};
bool						sFiberSuspended = false;

// Saving and loading are one-shot events the player triggers by hand, so unlike
// the frame loop they are allowed to speak: the frontend's OSD is the only
// channel that reaches someone who is not reading a log.  Success needs no
// message - the frontend already reports that itself.
void ShowMessage(const char* theText)
{
	if (EnvironCb == nullptr)
		return;

	retro_message aMessage = {};
	aMessage.msg = theText;
	aMessage.frames = 240;
	EnvironCb(RETRO_ENVIRONMENT_SET_MESSAGE, &aMessage);
}

// Saving needs the same thing the game's own mid-level save needs: a board that
// is actually being played.  That keeps menus, the Zen Garden and the level
// intro out, exactly as Board::NeedSaveGame() does for the file save.
bool CanSave()
{
	return gLawnApp != nullptr && gLawnApp->mBoard != nullptr && gLawnApp->mBoard->NeedSaveGame();
}

// Loading needs somewhere to put a level: a live board, or the main menu (where
// the player lands after losing - the single most common reason to reload).  The
// scenes listed are the ones that can be left cleanly; anything else is refused
// rather than half-dismantled behind a running level.
bool CanRestore()
{
	if (gLawnApp == nullptr || gLawnApp->mPlayerInfo == nullptr)
		return false;

	if (gLawnApp->mBoard != nullptr)
	{
		switch (gLawnApp->mGameScene)
		{
			case GameScenes::SCENE_LEVEL_INTRO:		// seed chooser / intro cutscene
			case GameScenes::SCENE_PLAYING:
			case GameScenes::SCENE_ZOMBIES_WON:
			case GameScenes::SCENE_AWARD:
				return true;
			default:
				return false;
		}
	}

	return gLawnApp->mGameScene == GameScenes::SCENE_MENU;
}

void RestoreMusic(MusicTune thePlayingBefore)
{
	Music* aMusic = gLawnApp->mMusic.get();
	if (aMusic == nullptr || aMusic->mCurMusicTune == MusicTune::MUSIC_TUNE_NONE)
		return;

	// The image says which tune belongs to this moment but cannot carry the
	// decoder, and nothing would notice a mismatch afterwards: the game only
	// starts a tune when the wanted one differs from the one it recorded. So
	// compare - the same tune means the mixer is already right and an in-level
	// load must not interrupt the soundtrack, a different one means the stream
	// belongs to a scene the snapshot has just replaced.
	if (aMusic->mCurMusicTune == thePlayingBefore)
		return;

	const MusicTune aTune = aMusic->mCurMusicTune;
	aMusic->StopAllMusic();
	aMusic->MakeSureMusicIsPlaying(aTune);
}

} // namespace

size_t SaveStateSize()
{
	return kCapacity;
}

void SetFiberSuspended(bool theSuspended)
{
	sFiberSuspended = theSuspended;
}

bool SaveState(void* theData, size_t theSize)
{
	if (theData == nullptr || theSize < sizeof(StateHeader) || !CanSave())
	{
		ShowMessage("Save state: only available during a level.");
		return false;
	}

	// Reading the board is safe even while the frontend owns the thread: an
	// in-progress wait is parked *between* frames, never inside one.
	std::vector<unsigned char> anImage;
	if (!LawnSaveGameToBuffer(gLawnApp->mBoard, anImage))
	{
		LogError("[pvz] could not snapshot the level.\n");
		ShowMessage("Save state: could not snapshot this level.");
		return false;
	}

	const size_t aTotal = sizeof(StateHeader) + anImage.size();
	if (aTotal > theSize || aTotal > kCapacity)
	{
		LogError("[pvz] save state needs %zu bytes, the frontend offered %zu.\n", aTotal, theSize);
		ShowMessage("Save state: too large for the frontend's buffer.");
		return false;
	}

	StateHeader aHeader = {};
	aHeader.mMagic = kMagic;
	aHeader.mVersion = kVersion;
	aHeader.mImageSize = static_cast<uint32_t>(anImage.size());
	aHeader.mBoardResult = static_cast<uint32_t>(gLawnApp->mBoardResult);
	aHeader.mGameMode = static_cast<uint32_t>(gLawnApp->mGameMode);
	aHeader.mLevel = static_cast<uint32_t>(gLawnApp->mBoard->mLevel);

	unsigned char* anOut = static_cast<unsigned char*>(theData);
	// The frontend writes (and later hashes) the full block it asked for, so the
	// tail past our image must not be left holding whatever was in the buffer.
	memset(anOut, 0, theSize);
	memcpy(anOut, &aHeader, sizeof(aHeader));
	memcpy(anOut + sizeof(aHeader), anImage.data(), anImage.size());

	LogInfo("[pvz] save state: %zu bytes (level %u).\n", aTotal, aHeader.mLevel);

	return true;
}

bool LoadState(const void* theData, size_t theSize)
{
	if (theData == nullptr || theSize < sizeof(StateHeader))
		return false;

	StateHeader aHeader;
	memcpy(&aHeader, theData, sizeof(aHeader));
	if (aHeader.mMagic != kMagic || aHeader.mVersion != kVersion)
	{
		LogError("[pvz] not a save state from this core.\n");
		ShowMessage("Load state: not a save state from this core.");
		return false;
	}

	// The frontend passes the size it *announced*, not the size we filled in, so
	// only the image has to fit inside it.
	if (aHeader.mImageSize == 0 || aHeader.mImageSize > theSize - sizeof(StateHeader))
	{
		LogError("[pvz] save state image (%u bytes) does not fit the %zu byte block.\n",
			aHeader.mImageSize, theSize);
		ShowMessage("Load state: the state is truncated.");
		return false;
	}
	if (!CanRestore())
	{
		// A frontend may load a state right after load_game ("autoload state"),
		// long before the game has finished starting and reached the menu.  Park
		// it for then.  Any other screen is a real refusal: the state would sit
		// around waiting for a moment that may never come.
		if (GameInitialized && gLawnApp != nullptr)
		{
			LogError("[pvz] a save state can only be loaded during a level or from the main menu.\n");
			ShowMessage("Load state: only available during a level or from the main menu.");
			return false;
		}
	}

	const unsigned char* anImage = static_cast<const unsigned char*>(theData) + sizeof(aHeader);
	sPendingImage.assign(anImage, anImage + aHeader.mImageSize);
	sPendingHeader = aHeader;

	return true;
}

void ApplyPendingState()
{
	if (sPendingImage.empty() || sFiberSuspended)
		return;

	std::vector<unsigned char> anImage;
	anImage.swap(sPendingImage);

	if (!CanRestore())
	{
		LogError("[pvz] the save state was dropped: the game left the level and the main menu.\n");
		ShowMessage("Load state: the game is no longer in a level.");
		return;
	}

	// Which tune the mixer is on right now, so the restored one can be compared
	// against it (the decoder's position and stream are not part of a state).
	MusicTune aPlayingBefore = MusicTune::MUSIC_TUNE_NONE;
	if (gLawnApp->mMusic != nullptr)
		aPlayingBefore = gLawnApp->mMusic->mCurMusicTune;

	// A state always means "a level is being played" - saving requires it - so the
	// game has to be at that point before it lands.  Anything else on screen
	// belongs to a scene the snapshot replaces: the seed chooser is an overlay
	// that sits on top of the board during the level intro, and without this the
	// restored level simply started underneath it, with the intro's music still
	// playing.  Rebuilding the board is also what the game's own "continue" does,
	// so a state starts from exactly the same place a file load does.
	const bool aNeedsCleanLevel =
		(gLawnApp->mBoard == nullptr) || (gLawnApp->mGameScene != GameScenes::SCENE_PLAYING);

	if (aNeedsCleanLevel)
	{
		// Leaving a level erases its save file when the level was lost or won.  A
		// state load is neither, so keep that flag out of KillBoard's way and put
		// it back if the image turns out to be unreadable.
		const BoardResult aResultBefore = gLawnApp->mBoardResult;
		gLawnApp->mBoardResult = BoardResult::BOARDRESULT_NONE;

		// The menu, the intro and the award screen each own a widget that outlives
		// the board; the plant picker is the one that bites, because it is drawn
		// over a level that keeps running underneath it.
		gLawnApp->KillGameSelector();
		gLawnApp->KillSeedChooserScreen();
		gLawnApp->KillAwardScreen();
		gLawnApp->MakeNewBoard();

		gLawnApp->mBoardResult = aResultBefore;
	}

	// The image rebuilds the whole effect system - particle systems, emitters,
	// particles, trails, reanimations, attachments - and it rebuilds every list
	// through the two node allocators.  That is only coherent when it starts
	// empty, which is exactly what the game guarantees: Board::Board() and
	// Board::DisposeBoard() both open with EffectSystemFreeAll(), so a level is
	// only ever loaded into a freshly emptied one.  Restoring into the live effect
	// system of the level being replaced instead re-allocates nodes the old level
	// still holds; once the pool runs dry the lists point at nothing, and a later
	// allocation hands out a slot whose particle definition is garbage.  (Symptom:
	// an access violation in PvzpParticleEmitter::Update(), a second or so after
	// the load, only in levels with enough particles to exhaust the pool.)
	if (gLawnApp->mEffectSystem != nullptr)
		gLawnApp->mEffectSystem->EffectSystemFreeAll();

	if (!LawnLoadGameFromBuffer(gLawnApp->mBoard, anImage.data(), anImage.size()))
	{
		LogError("[pvz] the save state could not be restored (unreadable image).\n");
		ShowMessage("Load state: the state could not be restored.");
		return;
	}

	LogInfo("[pvz] save state loaded (level %u).\n", sPendingHeader.mLevel);

	// What Board::LoadGame() does after a file load.  Board::LoadGame() itself is
	// not used because it reads from a path; the steps around it are the same.
	gLawnApp->mBoard->LoadBackgroundImages();
	gLawnApp->ClearUpdateBacklog();

	gLawnApp->mBoardResult = static_cast<BoardResult>(sPendingHeader.mBoardResult);
	gLawnApp->mGameMode = static_cast<GameMode>(sPendingHeader.mGameMode);

	RestoreMusic(aPlayingBefore);
}

} // namespace PvzLibretro

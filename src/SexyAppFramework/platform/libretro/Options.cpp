/*
 * Copyright (C) 2026 PvZ libretro contributors
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * This file is part of PvZ-Portable's libretro port.
 *
 * Core options, defined once in kOptions and published through
 * RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2 (with the legacy RETRO_ENVIRONMENT_
 * SET_VARIABLES string form for frontends that predate it).
 */

#include "Options.h"

#include "LibretroBackend.h"

#include "LawnApp.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace PvzLibretro
{

namespace
{

struct OptionValue
{
	const char* value;
	const char* label;
};

struct OptionDef
{
	const char*	key;
	const char*	desc;
	const char*	info;
	// Terminated by { nullptr, nullptr }.
	OptionValue	values[RETRO_NUM_CORE_OPTION_VALUES_MAX];
	const char*	defaultValue;
};

// ---------------------------------------------------------------------------
// The option list.  Keep the keys prefixed with "pvz_" and the descriptions
// player-facing: they are the only documentation a frontend shows.
// ---------------------------------------------------------------------------
const OptionDef kOptions[] = {
	{
		"pvz_cursor_speed",
		"Gamepad Cursor Speed",
		"Pixels per frame the left stick or d-pad moves the pointer. The pad is "
		"the only pointer a gamepad player has, and the default needs over two "
		"seconds to cross the screen.",
		{
			{ "2",  "2 (slow)"    },
			{ "4",  "4"           },
			{ "6",  "6 (default)" },
			{ "8",  "8"           },
			{ "12", "12"          },
			{ "16", "16 (fast)"   },
			{ nullptr, nullptr    },
		},
		"6",
	},
	{
		"pvz_gamepad_deadzone",
		"Gamepad Deadzone",
		"Analogue stick deadzone. Raise it when the pointer drifts on its own "
		"with the stick at rest, lower it when small movements are ignored.",
		{
			{ "0.00", "off" },
			{ "0.05", "5%"  }, { "0.10", "10%" }, { "0.15", "15%" },
			{ "0.20", "20%" }, { "0.25", "25% (default)" }, { "0.30", "30%" },
			{ "0.40", "40%" }, { "0.50", "50%" }, { "0.60", "60%" },
			{ nullptr, nullptr },
		},
		"0.25",
	},
	{
		"pvz_draw_cursor",
		"Core-Drawn Cursor",
		"The core paints its own pointer while the gamepad moves it: the game "
		"ships no cursor artwork and some frontends draw none of their own. "
		"'always' draws it for mouse users too.",
		{
			{ "auto",   "auto"   },
			{ "always", "always" },
			{ "never",  "never"  },
			{ nullptr,  nullptr  },
		},
		"auto",
	},
	{
		"pvz_cheat_keys",
		"Cheat Keys",
		"Unlocks the game's hidden cheat and debug keys, which upstream only "
		"provides by building with PVZ_DEBUG and passing -cheat: slow motion, "
		"20x speed, the debug overlay and easy planting. Nothing changes until "
		"a cheat key is actually pressed.",
		{
			{ "disabled", "disabled" },
			{ "enabled",  "enabled"  },
			{ nullptr,    nullptr    },
		},
		"disabled",
	},
};

const size_t kOptionCount = sizeof(kOptions) / sizeof(kOptions[0]);

std::string	sValues[kOptionCount];

// Static storage: frontends are allowed to keep the pointers we hand out.
retro_core_option_v2_definition	sDefinitions[kOptionCount + 1] = {};
std::vector<std::string>		sLegacyStrings;
std::vector<retro_variable>		sLegacyVariables;

int FindOption(const char* theKey)
{
	for (size_t i = 0; i < kOptionCount; i++)
	{
		if (std::strcmp(kOptions[i].key, theKey) == 0)
			return static_cast<int>(i);
	}

	return -1;
}

// Applies the options the game reads at runtime.  Called whenever a value could
// have changed, and only then - the frame loop must not fight the game state.
void ApplyOptions()
{
	// Cheat keys: the game's own -cheat switch sets exactly these two flags, and
	// both are plain runtime booleans (LawnApp.cpp / SexyAppBase.h).
	if (gLawnApp != nullptr)
	{
		const bool anEnabled = OptionBool("pvz_cheat_keys");
		gLawnApp->mCheatKeys = anEnabled;
		gLawnApp->mDebugKeysEnabled = anEnabled;
	}
}

void RegisterLegacyOptions()
{
	// "key; value1|value2|value3" - the pre-core-options interface.  Reserve
	// first: the retro_variable entries point into these strings.
	sLegacyStrings.clear();
	sLegacyStrings.reserve(kOptionCount);
	sLegacyVariables.clear();
	sLegacyVariables.reserve(kOptionCount + 1);

	for (size_t i = 0; i < kOptionCount; i++)
	{
		std::string aLine = kOptions[i].key;
		aLine += ';';
		for (const OptionValue* aValue = kOptions[i].values; aValue->value != nullptr; aValue++)
		{
			aLine += ' ';
			aLine += aValue->value;
			aLine += '|';
			aLine += aValue->label;
		}

		sLegacyStrings.push_back(std::move(aLine));
	}

	for (std::string& aString : sLegacyStrings)
		sLegacyVariables.push_back({ aString.c_str(), nullptr });
	sLegacyVariables.push_back({ nullptr, nullptr });

	EnvironCb(RETRO_ENVIRONMENT_SET_VARIABLES, sLegacyVariables.data());
}

} // namespace

void RegisterOptions()
{
	if (EnvironCb == nullptr)
		return;

	for (size_t i = 0; i < kOptionCount; i++)
	{
		retro_core_option_v2_definition& aDefinition = sDefinitions[i];

		aDefinition.key				= kOptions[i].key;
		aDefinition.desc			= kOptions[i].desc;
		aDefinition.info			= kOptions[i].info;
		aDefinition.default_value	= kOptions[i].defaultValue;

		for (size_t j = 0; j < RETRO_NUM_CORE_OPTION_VALUES_MAX; j++)
		{
			aDefinition.values[j].value = kOptions[i].values[j].value;
			aDefinition.values[j].label = kOptions[i].values[j].label;
			if (kOptions[i].values[j].value == nullptr)
				break;
		}

		sValues[i] = kOptions[i].defaultValue;
	}

	retro_core_options_v2 aOptions = {};
	aOptions.categories = nullptr;
	aOptions.definitions = sDefinitions;

	if (!EnvironCb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, &aOptions))
		RegisterLegacyOptions();
}

void RefreshOptions()
{
	if (EnvironCb == nullptr)
		return;

	// The first pass always reads: a frontend that does not implement the query
	// would otherwise never report a change.
	static bool sFirstRefresh = true;

	bool anUpdated = sFirstRefresh;
	if (!sFirstRefresh)
	{
		// Frontends that do not answer leave the value alone, so a failed query
		// only costs us the early-out below.
		EnvironCb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &anUpdated);
	}
	sFirstRefresh = false;

	if (!anUpdated)
		return;

	for (size_t i = 0; i < kOptionCount; i++)
	{
		retro_variable aQuery = {};
		aQuery.key = kOptions[i].key;
		if (EnvironCb(RETRO_ENVIRONMENT_GET_VARIABLE, &aQuery) && aQuery.value != nullptr)
			sValues[i] = aQuery.value;
		else
			sValues[i] = kOptions[i].defaultValue;
	}

	ApplyOptions();
}

bool OptionBool(const char* theKey)
{
	const int anIndex = FindOption(theKey);
	if (anIndex < 0)
		return false;

	const std::string& aValue = sValues[anIndex];
	return aValue == "enabled" || aValue == "on" || aValue == "true" || aValue == "1";
}

int OptionInt(const char* theKey)
{
	const int anIndex = FindOption(theKey);
	if (anIndex < 0)
		return 0;

	return std::atoi(sValues[anIndex].c_str());
}

float OptionFloat(const char* theKey)
{
	const int anIndex = FindOption(theKey);
	if (anIndex < 0)
		return 0.0f;

	return std::strtof(sValues[anIndex].c_str(), nullptr);
}

const char* OptionString(const char* theKey)
{
	const int anIndex = FindOption(theKey);
	if (anIndex < 0)
		return "";

	return sValues[anIndex].c_str();
}

} // namespace PvzLibretro

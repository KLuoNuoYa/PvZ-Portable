/*
 * Copyright (C) 2026 Zhou Qiankang <wszqkzqk@qq.com>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * This file is part of PvZ-Portable.
 *
 * PvZ-Portable is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * PvZ-Portable is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with PvZ-Portable. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef __SAVEGAMECONTEXT_H__
#define __SAVEGAMECONTEXT_H__

#include <cstddef>
#include <string>
#include <vector>

class Board;

bool				LawnLoadGame(Board* theBoard, const std::string& theFilePath);
bool				LawnSaveGame(Board* theBoard, const std::string& theFilePath);

// The same mid-level snapshot, but kept in memory instead of on disk.  The image
// is byte-for-byte what LawnSaveGame() writes (SaveFileHeaderV4 + chunk payload),
// which is what the libretro save-state support stores.  Only the v4 format is
// produced/consumed here; the legacy format has no in-memory reader.
bool				LawnSaveGameToBuffer(Board* theBoard, std::vector<unsigned char>& theBuffer);
bool				LawnLoadGameFromBuffer(Board* theBoard, const unsigned char* theData, size_t theSize);

#endif

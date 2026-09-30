/*
 * Portions of this file are based on the PopCap Games Framework
 * Copyright (C) 2005-2009 PopCap Games, Inc.
 *
 * Copyright (C) 2026 Zhou Qiankang <wszqkzqk@qq.com>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later AND LicenseRef-PopCap
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

#ifndef __GLPLATFORM_H__
#define __GLPLATFORM_H__

#ifdef __SWITCH__
#include <switch.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#endif

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4551) // glad generated code triggers this on MSVC
#endif

#include <glad/gles2.h>

#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <SDL.h>

// Shared macro definitions — identical keywords in GLSL ES 1.00 and GLSL 1.20
#define GLSL_VERT_MACROS \
	"#define VERT_IN attribute\n" \
	"#define V2F varying\n"

#define GLSL_FRAG_MACROS \
	"#define V2F varying\n" \
	"#define FRAG_OUT gl_FragColor\n" \
	"#define TEX2D texture2D\n"

// GLSL 1.50 core profile.  Desktop *core* contexts (e.g. the one RetroArch
// creates for its "glcore" video driver) reject GLSL 1.20 and the legacy
// attribute/varying/gl_FragColor/texture2D keywords, so the same shader body is
// compiled with these macros as a fallback.
#define GLSL_VERT_MACROS_CORE \
	"#define VERT_IN in\n" \
	"#define V2F out\n"

#define GLSL_FRAG_MACROS_CORE \
	"#define V2F in\n" \
	"#define FRAG_OUT fragColor\n" \
	"#define TEX2D texture\n" \
	"out vec4 fragColor;\n"

// When true, a desktop GL compatibility context is in use and shaders
extern bool gDesktopGLFallback;

#ifdef __LIBRETRO__
// Resolve GL entry points through the libretro frontend's get_proc_address hook
// (implemented in platform/libretro/LibretroBackend.cpp).
void LibretroLoadGLFunctions();
#endif

inline void PlatformGLInit()
{
#ifdef __LIBRETRO__
	// The libretro frontend owns the GL context; ask it for the entry points.
	LibretroLoadGLFunctions();
#elif defined(__SWITCH__)
	gladLoadGLES2((GLADloadfunc)eglGetProcAddress);
#else
	gladLoadGLES2((GLADloadfunc)SDL_GL_GetProcAddress);
#endif
}

#endif // __GLPLATFORM_H__

cmake_minimum_required(VERSION 3.12...4.0)

project(pvz-portable LANGUAGES C CXX)

include(CMake/ProjectVersion.cmake)

option(BUILD_STATIC "Statically link dependencies for a standalone exe (Windows MinGW, Linux)" OFF)
if(BUILD_STATIC)
	if(WIN32 AND NOT MSVC)
		set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
		set(BUILD_SHARED_LIBS OFF)
		add_link_options(-static)
	elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
		set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
		set(BUILD_SHARED_LIBS OFF)
	elseif(MSVC)
		message(WARNING "BUILD_STATIC is not supported for MSVC. Use a vcpkg -static triplet instead.")
		set(BUILD_STATIC OFF)
	else()
		message(WARNING "BUILD_STATIC is only supported on Windows with MinGW-based toolchains and on Linux.")
		set(BUILD_STATIC OFF)
	endif()
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if (CMAKE_GENERATOR MATCHES "Ninja")
	set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
endif()

if(MSVC)
	if(DEFINED VCPKG_TARGET_TRIPLET AND VCPKG_TARGET_TRIPLET MATCHES "-static")
		set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
	elseif(DEFINED VCPKG_TARGET_TRIPLET)
		set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
	endif()
endif()

file(GLOB SOURCES CONFIGURE_DEPENDS
	src/*.cpp
	src/SexyAppFramework/*.cpp
	src/SexyAppFramework/graphics/*.cpp
	src/SexyAppFramework/imagelib/*.cpp
	src/SexyAppFramework/misc/*.cpp
	src/SexyAppFramework/paklib/*.cpp
	src/SexyAppFramework/sound/*.cpp
	src/SexyAppFramework/widget/*.cpp
	src/SexyAppFramework/fcaseopen/*.c
	src/PvzpLib/*.cpp
	src/Lawn/*.cpp
	src/Lawn/Widget/*.cpp
	src/Lawn/System/*.cpp
)

option(LOW_MEMORY "Enable low memory mode" OFF)
if(LIBRETRO)
	# The core never owns a window: the frontend supplies the GL context, the
	# audio sink and the input, so the SDL platform backends are replaced by
	# src/SexyAppFramework/platform/libretro/ (already globbed above).
	list(APPEND PLAT_INCLUDES
		${CMAKE_CURRENT_SOURCE_DIR}/src/SexyAppFramework/platform/libretro
	)

	# Module-mode finders (FindOpenMPT) for dependencies with no CMake package.
	# Kept in their own directory: CMake/ also holds an Emscripten-only
	# FindSDL2.cmake that would otherwise shadow SDL2's own config package.

	# libopenmpt's public headers use C++-style comments and other C99+ syntax,
	# and the mixer's C sources include them for the MO3 backend.  The default
	# (gnu90) rejects them.
	set(CMAKE_C_STANDARD 11)
	set(CMAKE_C_STANDARD_REQUIRED ON)

	if(LIBRETRO_OPENMPT)
		# libopenmpt is the only backend that registers MO3 and implements the
		# order/stem API the adaptive music needs.
		if(LIBRETRO_OPENMPT_ROOT)
			find_path(OPENMPT_INCLUDE_DIR libopenmpt/libopenmpt.h
				PATHS ${LIBRETRO_OPENMPT_ROOT}/include NO_DEFAULT_PATH REQUIRED)
			find_library(OPENMPT_LIBRARY NAMES openmpt libopenmpt
				PATHS ${LIBRETRO_OPENMPT_ROOT}/lib NO_DEFAULT_PATH REQUIRED)
			set(OPENMPT_LIB ${OPENMPT_LIBRARY})
		else()
			find_path(OPENMPT_INCLUDE_DIR libopenmpt/libopenmpt.h REQUIRED)
			find_library(OPENMPT_LIBRARY NAMES openmpt libopenmpt REQUIRED)
			set(OPENMPT_LIB ${OPENMPT_LIBRARY})
		endif()

		# The game's MO3 stores its samples Ogg Vorbis compressed, so libopenmpt
		# decodes them through libvorbisfile.  A static libopenmpt.a does not
		# carry those dependencies, so they are added here.  Without them the
		# module loads but every sample is silent.
		find_library(OPENMPT_VORBISFILE_LIBRARY NAMES vorbisfile libvorbisfile)
		find_library(OPENMPT_VORBIS_LIBRARY NAMES vorbis libvorbis)
		find_library(OPENMPT_OGG_LIBRARY NAMES ogg libogg)
		if(OPENMPT_VORBISFILE_LIBRARY AND OPENMPT_VORBIS_LIBRARY AND OPENMPT_OGG_LIBRARY)
			list(APPEND OPENMPT_LIB
				${OPENMPT_VORBISFILE_LIBRARY} ${OPENMPT_VORBIS_LIBRARY} ${OPENMPT_OGG_LIBRARY})
		else()
			message(WARNING
				"libretro: libvorbisfile/libvorbis/libogg not found -- MO3 Ogg-compressed "
				"samples will be silent. Enable USE_OGG_VORBIS/USE_OGG_VORBIS_STB (bundled "
				"stb_vorbis) for the mixer, or provide the system Vorbis libraries.")
		endif()

		# SDL-Mixer-X ships its own cmake/find/FindOpenMPT.cmake whose find_path/
	# find_library calls carry no hints, so preset the cache variables it reads.
	# Without this, find_package(OpenMPT) reports not-found and the mixer skips
	# its MO3-capable backend entirely.
	set(OpenMPT_INCLUDE_DIR "${OPENMPT_INCLUDE_DIR}" CACHE PATH "libopenmpt include directory" FORCE)
	set(OpenMPT_LIBRARY "${OPENMPT_LIBRARY}" CACHE FILEPATH "libopenmpt library" FORCE)

	set(USE_OPENMPT ON CACHE BOOL "SDL Mixer X openmpt" FORCE)
		message(STATUS "libretro: libopenmpt found at ${OPENMPT_LIBRARY}")
	else()
		# Without libopenmpt the MO3 background music cannot be decoded by any
		# available backend (libmodplug cannot load MO3), so this build simply has
		# no background music.  See README-libretro.md section 4.3.
		set(OPENMPT_LIB "")
		set(USE_OPENMPT OFF CACHE BOOL "SDL Mixer X openmpt" FORCE)
		message(STATUS "libretro: building without libopenmpt -- background music (MO3) will be silent")
	endif()

	# Codec backends whose libraries are not guaranteed to be present.
	set(USE_WAVPACK OFF CACHE BOOL "SDL Mixer X wavpack" FORCE)
	set(USE_FLAC OFF CACHE BOOL "SDL Mixer X flac" FORCE)
	set(USE_MP3_MPG123 OFF CACHE BOOL "SDL Mixer X mpg123" FORCE)
	set(USE_GME OFF CACHE BOOL "SDL Mixer X gme" FORCE)

	# libmodplug is dead weight for this game: its data contains only .ogg sound
	# effects and .mo3 music, and libmodplug cannot load MO3 at all
	# (ModPlug_Load returns NULL on the shipped mainmusic.mo3).  With libopenmpt
	# enabled it is also redundant, since libopenmpt covers every tracker format
	# libmodplug knows.  Dropping it removes ~21 format loaders from the core.
	set(USE_MODPLUG OFF CACHE BOOL "SDL Mixer X modplug" FORCE)
elseif(NINTENDO_SWITCH)
	list(APPEND SOURCES
		${CMAKE_CURRENT_SOURCE_DIR}/src/SexyAppFramework/platform/switch/Window.cpp
		${CMAKE_CURRENT_SOURCE_DIR}/src/SexyAppFramework/platform/switch/Input.cpp
	)
	list(APPEND PLAT_INCLUDES
		${CMAKE_CURRENT_SOURCE_DIR}/src/SexyAppFramework/platform/switch
	)
elseif(EMSCRIPTEN)
	# libopenmpt has no Emscripten port; must be pre-built with emmake.
	find_library(OPENMPT_LIBRARY NAMES openmpt libopenmpt REQUIRED)
	set(OPENMPT_LIB ${OPENMPT_LIBRARY})

	list(APPEND SOURCES
		${CMAKE_CURRENT_SOURCE_DIR}/src/SexyAppFramework/platform/emscripten/Window.cpp
		${CMAKE_CURRENT_SOURCE_DIR}/src/SexyAppFramework/platform/default/Input.cpp
	)
	list(APPEND PLAT_INCLUDES
		${CMAKE_CURRENT_SOURCE_DIR}/src/SexyAppFramework/platform/emscripten
	)
else()
	if(VCPKG_TOOLCHAIN)
		find_package(libopenmpt CONFIG REQUIRED)
		set(OPENMPT_LIB libopenmpt::libopenmpt)
	else()
		find_library(OPENMPT_LIBRARY NAMES openmpt libopenmpt REQUIRED)
		set(OPENMPT_LIB ${OPENMPT_LIBRARY})
		if(OPENMPT_LIBRARY MATCHES "\\${CMAKE_STATIC_LIBRARY_SUFFIX}$")
			find_path(OPENMPT_INCLUDE_DIR libopenmpt/libopenmpt.h REQUIRED)
			include(CheckCXXSourceCompiles)
			include(CMakePushCheckState)
			cmake_push_check_state()
			set(CMAKE_REQUIRED_INCLUDES ${OPENMPT_INCLUDE_DIR})
			set(CMAKE_REQUIRED_LIBRARIES ${OPENMPT_LIBRARY})
			check_cxx_source_compiles("#include <libopenmpt/libopenmpt.h>
int main() {
	return openmpt_module_create_from_memory2(0, 0, 0, 0, 0, 0, 0, 0, 0) != 0;
}" OPENMPT_LINKS_STANDALONE)
			cmake_pop_check_state()
			if(NOT OPENMPT_LINKS_STANDALONE)
				find_library(MPG123_LIBRARY NAMES mpg123 libmpg123)
				find_library(VORBISFILE_LIBRARY NAMES vorbisfile libvorbisfile)
				find_library(VORBIS_LIBRARY NAMES vorbis libvorbis)
				find_library(OGG_LIBRARY NAMES ogg libogg)
				if(NOT MPG123_LIBRARY OR NOT VORBISFILE_LIBRARY OR NOT VORBIS_LIBRARY OR NOT OGG_LIBRARY)
					message(FATAL_ERROR "Static libopenmpt does not link standalone and some of its dependencies were not found (see the configure log for details)")
				endif()
				list(APPEND OPENMPT_LIB ${MPG123_LIBRARY} ${VORBISFILE_LIBRARY} ${VORBIS_LIBRARY} ${OGG_LIBRARY})
			endif()
		endif()
	endif()
	list(APPEND SOURCES
		${CMAKE_CURRENT_SOURCE_DIR}/src/SexyAppFramework/platform/default/Window.cpp
		${CMAKE_CURRENT_SOURCE_DIR}/src/SexyAppFramework/platform/default/Input.cpp
	)
	list(APPEND PLAT_INCLUDES
		${CMAKE_CURRENT_SOURCE_DIR}/src/SexyAppFramework/platform/default
	)
endif()

if(CMAKE_BUILD_TYPE AND CMAKE_BUILD_TYPE MATCHES "^[Dd][Ee][Bb][Uu][Gg]$")
	set(WIN_CONSOLE_DEFAULT ON)
	set(PVZ_DEBUG_DEFAULT ON)
else()
	set(WIN_CONSOLE_DEFAULT OFF)
	set(PVZ_DEBUG_DEFAULT OFF)
endif()

option(PVZ_DEBUG "Enable PVZ_DEBUG macro to enable cheats, hidden features, etc." ${PVZ_DEBUG_DEFAULT})
option(CONSOLE "Show console on Windows" ${WIN_CONSOLE_DEFAULT})
option(DO_FIX_BUGS "Define DO_FIX_BUGS macro (Community fixes for original game bugs of 1.2.0.1073 GOTY Edition)" OFF)

# ---------------------------------------------------------------------------
# libretro core build
# ---------------------------------------------------------------------------
option(LIBRETRO "Build as a libretro core instead of a standalone game" OFF)
option(LIBRETRO_OPENMPT "Link libopenmpt so MO3 music plays (requires libopenmpt)" OFF)
set(LIBRETRO_OPENMPT_ROOT "" CACHE PATH "libopenmpt prefix containing include/ and lib/")
set(LIBRETRO_OUTPUT_NAME "pvz_libretro" CACHE STRING "File name (without extension) of the libretro core")

if(LIBRETRO)
	# The standalone entry point is replaced by the retro_* API.
	list(REMOVE_ITEM SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp)
	file(GLOB LIBRETRO_PLATFORM_SOURCES CONFIGURE_DEPENDS
		src/SexyAppFramework/platform/libretro/*.cpp
	)
	list(APPEND SOURCES ${LIBRETRO_PLATFORM_SOURCES})
	list(APPEND PLAT_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/src/SexyAppFramework/platform/libretro)
endif()

if(EMSCRIPTEN)
	list(PREPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/CMake")
	set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -sUSE_SDL=2 -sUSE_ZLIB=1 -sUSE_LIBPNG=1 -sUSE_LIBJPEG=1")
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -sUSE_SDL=2 -sUSE_ZLIB=1 -sUSE_LIBPNG=1 -sUSE_LIBJPEG=1 -fexceptions")
else()
	find_package(Threads REQUIRED)
	find_package(ZLIB REQUIRED)
	find_package(JPEG REQUIRED)
	find_package(PNG REQUIRED)
	find_package(SDL2 REQUIRED)
endif()

function(_pvz_patch_sdl2_iface_libs target)
	get_target_property(_libs ${target} INTERFACE_LINK_LIBRARIES)
	if(NOT _libs)
		return()
	endif()
	set(_patched)
	foreach(_lib IN LISTS _libs)
		if(_lib STREQUAL "SDL2::SDL2")
			list(APPEND _patched SDL2::SDL2-static)
		else()
			list(APPEND _patched "${_lib}")
		endif()
	endforeach()
	set_target_properties(${target} PROPERTIES INTERFACE_LINK_LIBRARIES "${_patched}")
endfunction()

if(BUILD_STATIC AND TARGET SDL2::SDL2-static)
	if(TARGET SDL2::SDL2main)
		_pvz_patch_sdl2_iface_libs(SDL2::SDL2main)
	endif()
	set(SDL2_LINK_TARGET SDL2::SDL2-static)
else()
	set(SDL2_LINK_TARGET SDL2::SDL2)
endif()

set(SDL_MIXER_X_STATIC ON CACHE BOOL "Build static library of SDL Mixer X")
set(SDL_MIXER_X_SHARED OFF CACHE BOOL "Build shared library of SDL Mixer X")
set(USE_MIDI OFF CACHE BOOL "SDL Mixer X midi")
set(USE_MODPLUG OFF CACHE BOOL "SDL Mixer X modplug")
set(USE_XMP OFF CACHE BOOL "SDL Mixer X xmp")
set(USE_OPUS OFF CACHE BOOL "SDL Mixer X opus")
set(USE_DRFLAC OFF CACHE BOOL "SDL Mixer X flac")
set(USE_PXTONE OFF CACHE BOOL "SDL Mixer X pxtone")
add_subdirectory(src/SexyAppFramework/sound/SDL-Mixer-X)

# Patch SDL-Mixer-X's transitive SDL2 dep without touching vendored CMake files
if(BUILD_STATIC AND TARGET SDL2::SDL2-static)
	_pvz_patch_sdl2_iface_libs(SDL2_mixer_ext_Static)
endif()

# Android requires a shared library loaded by SDLActivity; other platforms use an executable.
# libretro cores are always shared objects loaded by the frontend.
if(LIBRETRO)
	add_library(pvz_libretro MODULE ${SOURCES})
	set_target_properties(pvz_libretro PROPERTIES
		PREFIX ""
		OUTPUT_NAME "${LIBRETRO_OUTPUT_NAME}"
		C_VISIBILITY_PRESET hidden
		CXX_VISIBILITY_PRESET hidden
	)
elseif(ANDROID)
	add_library(pvz-portable SHARED ${SOURCES})
elseif(IOS)
	add_executable(pvz-portable MACOSX_BUNDLE ${SOURCES})
else()
	add_executable(pvz-portable ${SOURCES})
endif()

if(LIBRETRO)
	set(PVZ_TARGET pvz_libretro)
else()
	set(PVZ_TARGET pvz-portable)
endif()

target_include_directories(${PVZ_TARGET} PRIVATE
	${PLAT_INCLUDES}
	${PROJECT_BINARY_DIR}
	${PROJECT_SOURCE_DIR}/src
	${PROJECT_SOURCE_DIR}/src/SexyAppFramework
	${PROJECT_SOURCE_DIR}/src/SexyAppFramework/sound/SDL-Mixer-X/include
	${SDL2_INCLUDE_DIRS}
)
target_compile_definitions(${PVZ_TARGET} PRIVATE
	$<$<BOOL:${PVZ_DEBUG}>:PVZ_DEBUG>
	$<$<BOOL:${DO_FIX_BUGS}>:DO_FIX_BUGS>
	$<$<BOOL:${LIBRETRO}>:__LIBRETRO__>
)
target_compile_features(${PVZ_TARGET} PRIVATE cxx_std_20)
set_target_properties(${PVZ_TARGET} PROPERTIES CXX_EXTENSIONS OFF)

if(LIBRETRO AND NOT MSVC)
	# Let the linker discard code the core never reaches.  This matters most for
	# libopenmpt, which is by far the largest contributor to the core's size; its
	# archive is built with the same flags so gc-sections can act on it.
	target_compile_options(${PVZ_TARGET} PRIVATE -ffunction-sections -fdata-sections)
	target_link_options(${PVZ_TARGET} PRIVATE -Wl,--gc-sections)
	if(NOT CMAKE_BUILD_TYPE MATCHES "^[Dd][Ee][Bb][Uu][Gg]$")
		# Drop the COFF symbol table.  The retro_* entry points live in .edata,
		# so they survive stripping.
		target_link_options(${PVZ_TARGET} PRIVATE -s)
	endif()
endif()

if(LIBRETRO AND OPENMPT_INCLUDE_DIR)
	target_include_directories(${PVZ_TARGET} PRIVATE ${OPENMPT_INCLUDE_DIR})
endif()

if (LOW_MEMORY)
	target_compile_definitions(${PVZ_TARGET} PRIVATE LOW_MEMORY)
endif()

if(LIBRETRO)
	# No SDL2main: the core exports retro_* instead of defining main().
	target_link_libraries(${PVZ_TARGET} PRIVATE
		SDL2_mixer_ext_Static
		${OPENMPT_LIB}
		PNG::PNG
		JPEG::JPEG
		ZLIB::ZLIB
		${SDL2_LINK_TARGET}
		Threads::Threads
	)
	if(WIN32)
		target_compile_definitions(${PVZ_TARGET} PRIVATE WINDOWS)
		target_link_libraries(${PVZ_TARGET} PRIVATE ws2_32 user32 gdi32 winmm imm32 shlwapi)
		if(MSVC)
			target_compile_options(${PVZ_TARGET} PRIVATE /utf-8)
		endif()
	endif()
elseif(EMSCRIPTEN)
	set_property(TARGET pvz-portable APPEND PROPERTY LINK_DEPENDS
		"${CMAKE_CURRENT_SOURCE_DIR}/wasm/shell.html"
	)
	target_link_libraries(pvz-portable PRIVATE
		SDL2_mixer_ext_Static
		${OPENMPT_LIB}
	)
	target_link_options(pvz-portable PRIVATE
		"SHELL:-sUSE_SDL=2"
		"SHELL:-sUSE_ZLIB=1"
		"SHELL:-sUSE_LIBPNG=1"
		"SHELL:-sUSE_LIBJPEG=1"
		"SHELL:-fexceptions"
		"SHELL:-sASYNCIFY"
		"SHELL:-sALLOW_MEMORY_GROWTH=1"
		"SHELL:-sMAXIMUM_MEMORY=1073741824"
		"SHELL:-sGROWABLE_ARRAYBUFFERS=0"
		"SHELL:-sFORCE_FILESYSTEM=1"
		"SHELL:-sEXPORTED_RUNTIME_METHODS=[\"FS\",\"callMain\"]"
		"SHELL:-sINVOKE_RUN=0"
		"SHELL:-lidbfs.js"
		"SHELL:--shell-file ${CMAKE_CURRENT_SOURCE_DIR}/wasm/shell.html"
	)
	set_target_properties(pvz-portable PROPERTIES SUFFIX ".html")
else()
	target_link_libraries(pvz-portable PRIVATE
		SDL2_mixer_ext_Static 
		${OPENMPT_LIB} 
		PNG::PNG
		JPEG::JPEG
		ZLIB::ZLIB
		SDL2::SDL2main
		${SDL2_LINK_TARGET}
		Threads::Threads
	)
endif()

if(LIBRETRO)
	# Windows/libretro specifics were already handled in the LIBRETRO branch
	# above (the core is a DLL, so no resource script and no -mwindows).
elseif (WIN32)
	if(MSVC)
		target_compile_options(pvz-portable PRIVATE /utf-8)
	endif()

	target_sources(pvz-portable PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/SexyAppFramework/LawnProject.rc)

	set_source_files_properties(
		${CMAKE_CURRENT_SOURCE_DIR}/src/SexyAppFramework/LawnProject.rc
		PROPERTIES
			COMPILE_DEFINITIONS "APP_ICON_PATH=\"${CMAKE_CURRENT_SOURCE_DIR}/icon.ico\""
			OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/icon.ico"
	)

	target_link_libraries(pvz-portable PRIVATE ws2_32 user32 gdi32 winmm imm32 shlwapi)
	target_compile_definitions(pvz-portable PRIVATE WINDOWS)

	if(NOT CONSOLE)
		if(MSVC)
			set_target_properties(pvz-portable PROPERTIES WIN32_EXECUTABLE TRUE)
		else()
			target_link_options(pvz-portable PRIVATE -mwindows)
		endif()
	endif()
elseif (NINTENDO_SWITCH)
	target_link_libraries(pvz-portable PRIVATE GLESv2 EGL glapi drm_nouveau)

	nx_generate_nacp(pvz-portable.nacp
		NAME "PvZ Portable"
		AUTHOR "wszqkzqk, etc., a community-driven port"
		VERSION "${PVZP_VERSION_PLAIN}"
	)

	nx_create_nro(pvz-portable
		NACP pvz-portable.nacp
		ICON ${CMAKE_CURRENT_SOURCE_DIR}/icon-switch.jpg
	)
elseif (ANDROID)
	target_link_libraries(pvz-portable PRIVATE GLESv2 EGL android log)
	set_target_properties(pvz-portable PROPERTIES OUTPUT_NAME "main")
elseif (IOS)
	target_link_libraries(pvz-portable PRIVATE
		"-framework Foundation"
		"-framework UIKit"
		"-framework OpenGLES"
		"-framework QuartzCore"
		"-framework CoreGraphics"
		"-framework CoreMotion"
		"-framework GameController"
		"-framework AVFoundation"
		"-framework AudioToolbox"
		"-framework Metal"
	)

	configure_file(ios/Info.plist.in "${CMAKE_CURRENT_BINARY_DIR}/Info.plist" @ONLY)
	set_target_properties(pvz-portable PROPERTIES
		MACOSX_BUNDLE TRUE
		MACOSX_BUNDLE_GUI_IDENTIFIER "io.github.wszqkzqk.pvz-portable"
		MACOSX_BUNDLE_BUNDLE_NAME "PvZ Portable"
		MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/Info.plist"
		XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
		XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "16.4"
		XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "io.github.wszqkzqk.pvz-portable"
		XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "-"
		XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_APPICON_NAME "AppIcon"
	)

	# Add launch storyboard to the bundle
	if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/ios/LaunchScreen.storyboard")
		target_sources(pvz-portable PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/ios/LaunchScreen.storyboard")
		set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/ios/LaunchScreen.storyboard"
			PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
	endif()

	# Add asset catalog (app icon)
	if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/ios/Assets.xcassets")
		target_sources(pvz-portable PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/ios/Assets.xcassets")
		set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/ios/Assets.xcassets"
			PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
	endif()
endif()

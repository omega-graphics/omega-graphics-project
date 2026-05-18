set(OMEGAVA_VERSION "0.1")

omega_graphics_project(OmegaVA VERSION ${OMEGAVA_VERSION} LANGUAGES C CXX)

set(OMEGAVA_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR})

if(IOS OR ANDROID)
	set(OMEGAVA_MOBILE TRUE)
else()
	set(OMEGAVA_MOBILE FALSE)
endif()

# Mirror wtk/CMakeLists.txt's TARGET_* convention so the per-platform
# backend selection reads the same on both sides of the extraction.
if(WIN32)
	set(TARGET_WIN32 TRUE)
elseif(APPLE AND NOT IOS)
	set(TARGET_MACOS TRUE)
elseif(APPLE AND IOS)
	set(TARGET_IOS TRUE)
elseif(UNIX AND NOT APPLE AND NOT ANDROID)
	set(TARGET_LINUX TRUE)
elseif(ANDROID)
	set(TARGET_ANDROID TRUE)
endif()

# ── Linux FFmpeg + alsa-lib options (moved from wtk/CMakeLists.txt) ──
if(TARGET_LINUX)
	find_package(PkgConfig REQUIRED)

	option(OMEGAVA_USE_VENDORED_FFMPEG "Build FFmpeg from the AUTOMDEPS clone instead of pkg-config" ON)
	option(OMEGAVA_USE_VENDORED_ALSA   "Build alsa-lib from the AUTOMDEPS clone instead of pkg-config" ON)

	if(NOT OMEGAVA_USE_VENDORED_FFMPEG)
		pkg_check_modules(FFMPEG REQUIRED
			libavcodec
			libavformat
			libavutil
			libavdevice
			libswscale
			libswresample)
	endif()
	if(NOT OMEGAVA_USE_VENDORED_ALSA)
		# Even with the system fallback ALSA is optional — without it,
		# the FFmpeg Media backend builds but capture/playback are
		# no-ops at runtime.
		pkg_check_modules(ALSA alsa)
		if(NOT ALSA_FOUND)
			message(WARNING "OmegaVA: OMEGAVA_USE_VENDORED_ALSA=OFF and ALSA "
				"is not installed via pkg-config. Audio capture/playback will "
				"be no-ops at runtime.")
		endif()
	endif()
endif()

set(OMEGAVA_THIRD_PARTY_SRC_DIR ${OMEGAVA_SOURCE_DIR}/deps)
set(OMEGAVA_THIRD_PARTY_OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/deps)

add_custom_target(OmegaVAThirdParty)

# Vendored alsa-lib (Linux only). Source comes from va/AUTOMDEPS pinned at
# v1.2.14. autotools build via add_third_party CUSTOM_PROJECT.
if(TARGET_LINUX AND OMEGAVA_USE_VENDORED_ALSA)
	set(ALSA_SOURCE_DIR  "${OMEGAVA_THIRD_PARTY_SRC_DIR}/alsa-lib/code")
	set(ALSA_INSTALL_DIR "${OMEGAVA_THIRD_PARTY_OUTPUT_DIR}/alsa-lib")
	if(NOT EXISTS "${ALSA_SOURCE_DIR}")
		message(FATAL_ERROR "OmegaVA: OMEGAVA_USE_VENDORED_ALSA=ON but "
			"${ALSA_SOURCE_DIR} does not exist. Run AUTOMDEPS to clone "
			"the alsa-lib source first, or set OMEGAVA_USE_VENDORED_ALSA=OFF.")
	endif()
	# Build prerequisites on the host: libtool, autoconf, automake.
	# On Debian/Ubuntu: `apt install libtool autoconf automake`. The
	# alternative is OMEGAVA_USE_VENDORED_ALSA=OFF + system libasound2-dev.
	add_third_party(
		CUSTOM_PROJECT
		NAME alsa-lib
		SOURCE_DIR "${ALSA_SOURCE_DIR}"
		BINARY_DIR "${ALSA_SOURCE_DIR}"
		INSTALL_DIR "${ALSA_INSTALL_DIR}"
		CONF bash -c "cd ${ALSA_SOURCE_DIR} && GITCOMPILE_NO_MAKE=1 ./gitcompile --prefix=${ALSA_INSTALL_DIR} --disable-python --disable-static --enable-shared --with-pic"
		BUILD make
		INSTALL make install
		EXPORT_SHARED_LIBS "asound:lib/libasound${CMAKE_SHARED_LIBRARY_SUFFIX}"
		EXPORT_INCLUDE_DIRS "include")
	add_dependencies(OmegaVAThirdParty alsa-lib)
endif()

if(TARGET_LINUX AND OMEGAVA_USE_VENDORED_FFMPEG)
	set(FFMPEG_SOURCE_DIR  "${OMEGAVA_THIRD_PARTY_SRC_DIR}/ffmpeg/code")
	set(FFMPEG_INSTALL_DIR "${OMEGAVA_THIRD_PARTY_OUTPUT_DIR}/ffmpeg")
	if(NOT EXISTS "${FFMPEG_SOURCE_DIR}")
		message(FATAL_ERROR "OmegaVA: OMEGAVA_USE_VENDORED_FFMPEG=ON but "
			"${FFMPEG_SOURCE_DIR} does not exist. Run AUTOMDEPS to clone "
			"the FFmpeg source first, or set OMEGAVA_USE_VENDORED_FFMPEG=OFF.")
	endif()
	set(_FFMPEG_EXTRA_FLAGS "")
	set(_FFMPEG_DEPS "")
	if(OMEGAVA_USE_VENDORED_ALSA)
		set(_FFMPEG_EXTRA_FLAGS "--enable-alsa --extra-cflags=-I${ALSA_INSTALL_DIR}/include --extra-ldflags=-L${ALSA_INSTALL_DIR}/lib")
		set(_FFMPEG_DEPS alsa-lib)
	endif()
	add_third_party(
		CUSTOM_PROJECT
		NAME ffmpeg
		SOURCE_DIR "${FFMPEG_SOURCE_DIR}"
		BINARY_DIR "${FFMPEG_SOURCE_DIR}"
		INSTALL_DIR "${FFMPEG_INSTALL_DIR}"
		CONF bash -c "cd ${FFMPEG_SOURCE_DIR} && ./configure --prefix=${FFMPEG_INSTALL_DIR} --disable-programs --disable-doc --disable-static --enable-shared --enable-pic ${_FFMPEG_EXTRA_FLAGS}"
		BUILD make
		INSTALL make install
		EXPORT_SHARED_LIBS
			"avcodec:lib/libavcodec${CMAKE_SHARED_LIBRARY_SUFFIX}"
			"avformat:lib/libavformat${CMAKE_SHARED_LIBRARY_SUFFIX}"
			"avutil:lib/libavutil${CMAKE_SHARED_LIBRARY_SUFFIX}"
			"avdevice:lib/libavdevice${CMAKE_SHARED_LIBRARY_SUFFIX}"
			"swscale:lib/libswscale${CMAKE_SHARED_LIBRARY_SUFFIX}"
			"swresample:lib/libswresample${CMAKE_SHARED_LIBRARY_SUFFIX}"
		EXPORT_INCLUDE_DIRS "include"
		DEPS ${_FFMPEG_DEPS})
	add_dependencies(OmegaVAThirdParty ffmpeg)
endif()

# ── Gather sources ──
file(GLOB MEDIA_SRCS "${OMEGAVA_SOURCE_DIR}/src/*.cpp")

if(TARGET_WIN32)
	file(GLOB WIN_SRCS "${OMEGAVA_SOURCE_DIR}/src/wmf/*.cpp")
	list(APPEND MEDIA_SRCS ${WIN_SRCS})
elseif(TARGET_MACOS OR TARGET_IOS)
	file(GLOB MACOS_SRCS "${OMEGAVA_SOURCE_DIR}/src/avf/*.mm")
	list(APPEND MEDIA_SRCS ${MACOS_SRCS})
elseif(TARGET_LINUX)
	file(GLOB FFMPEG_SRCS "${OMEGAVA_SOURCE_DIR}/src/ffmpeg/*.cpp")
	list(APPEND MEDIA_SRCS ${FFMPEG_SRCS})
elseif(TARGET_ANDROID)
	file(GLOB ANDROID_SRCS "${OMEGAVA_SOURCE_DIR}/src/android/*.cpp")
	list(APPEND MEDIA_SRCS ${ANDROID_SRCS})
endif()

source_group(OmegaVA ${MEDIA_SRCS})

add_library(OmegaVA STATIC ${MEDIA_SRCS})
set_target_properties(OmegaVA PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(OmegaVA
	PUBLIC ${OMEGAVA_SOURCE_DIR}/include
	       $<TARGET_PROPERTY:OmegaCommon,INCLUDE_DIRECTORIES>
	       $<TARGET_PROPERTY:OmegaGTE,INCLUDE_DIRECTORIES>)

target_link_libraries(OmegaVA PUBLIC OmegaCommon OmegaGTE)

if(TARGET_LINUX)
	if(OMEGAVA_USE_VENDORED_FFMPEG)
		# IMPORTED SHARED targets created by add_third_party above. The
		# target's INTERFACE_INCLUDE_DIRECTORIES already points at
		# ${FFMPEG_INSTALL_DIR}/include so we don't need a separate
		# target_include_directories call.
		target_link_libraries(OmegaVA PUBLIC
			avcodec
			avformat
			avutil
			avdevice
			swscale
			swresample)
	else()
		target_include_directories(OmegaVA PRIVATE ${FFMPEG_INCLUDE_DIRS})
		target_compile_options(OmegaVA PRIVATE ${FFMPEG_CFLAGS_OTHER})
		target_link_directories(OmegaVA PUBLIC ${FFMPEG_LIBRARY_DIRS})
		target_link_libraries(OmegaVA PUBLIC ${FFMPEG_LIBRARIES})
	endif()

	if(OMEGAVA_USE_VENDORED_ALSA)
		target_link_libraries(OmegaVA PUBLIC asound)
		target_compile_definitions(OmegaVA PRIVATE OMEGA_AUDIO_ALSA=1)
	elseif(ALSA_FOUND)
		target_include_directories(OmegaVA PRIVATE ${ALSA_INCLUDE_DIRS})
		target_compile_options(OmegaVA PRIVATE ${ALSA_CFLAGS_OTHER})
		target_link_directories(OmegaVA PUBLIC ${ALSA_LIBRARY_DIRS})
		target_link_libraries(OmegaVA PUBLIC ${ALSA_LIBRARIES})
		target_compile_definitions(OmegaVA PRIVATE OMEGA_AUDIO_ALSA=1)
	endif()
endif()

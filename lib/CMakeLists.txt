SET (STREAMRIPPER_LIB_SRC
	callback.c callback.h
	cbuf3.c cbuf3.h
	charset.c charset.h
	debug.c	debug.h
	errors.c errors.h
	external.c external.h
	filelib.c filelib.h
	findsep.c findsep.h
	findsep2.c
	http.c http.h
	mchar.c mchar.h
	parse.c parse.h
	prefs.c prefs.h
	relaylib.c relaylib.h
	ripogg.c ripogg.h
	ripstream.c ripstream.h
	ripstream_mp3.c ripstream_mp3.h
	ripstream_ogg.c
	rip_manager.c rip_manager.h
	socklib.c socklib.h
	threadlib.c threadlib.h
	track_info.c track_info.h
	utf8.c utf8.h

	charmaps.h
	confw32.h
	list.h
	sr_compat.h
	srtypes.h
	uce_dirent.h
	)

list(APPEND STREAMRIPPER_LIB_SRC argv.c http_stream.c audio_frames.c)
add_library(streamripper1 STATIC ${STREAMRIPPER_LIB_SRC})
target_include_directories(streamripper1 PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" "${PROJECT_BINARY_DIR}")
target_include_directories(streamripper1 SYSTEM PUBLIC ${MAD_INCLUDE_DIR} ${OGG_INCLUDE_DIR} ${VORBIS_INCLUDE_DIR})
target_link_libraries(streamripper1 PUBLIC sr_quality PkgConfig::GLIB2 PkgConfig::CURL Threads::Threads
  ${MAD_LIBRARIES} ${VORBIS_LIBRARIES} ${OGG_LIBRARIES})
if(UNIX)
  target_link_libraries(streamripper1 PUBLIC m)
  target_compile_definitions(streamripper1 PUBLIC __UNIX__=1)
endif()

add_executable(pcsx2-eerunner)

if (PACKAGE_MODE)
	install(TARGETS pcsx2-eerunner DESTINATION ${CMAKE_INSTALL_BINDIR})
else()
	install(TARGETS pcsx2-eerunner DESTINATION ${CMAKE_SOURCE_DIR}/bin)
endif()

target_sources(pcsx2-eerunner PRIVATE
	Main.cpp
)

if(UNIX)
	target_link_libraries(pcsx2-eerunner PRIVATE ${CMAKE_DL_LIBS})
endif()

if(ANDROID)
	# The core leaves android/log/shaderc to the final link (emucore does the
	# same in the Android entry CMakeLists); a bare NDK executable has to say so
	# itself. shaderc is direct-linked on Android where desktop dlopens it.
	target_link_libraries(pcsx2-eerunner PRIVATE android log shaderc)
endif()

# Relative to this directory, not CMAKE_SOURCE_DIR: the Android entry CMakeLists
# is the top-level project there, so CMAKE_SOURCE_DIR is not the source root.
get_filename_component(EERUNNER_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

target_include_directories(pcsx2-eerunner PRIVATE
	"${CMAKE_BINARY_DIR}/common/include"
	"${EERUNNER_SOURCE_ROOT}/pcsx2"
	"${EERUNNER_SOURCE_ROOT}/tests/ctest/core/recompilers"
)

target_link_libraries(pcsx2-eerunner PRIVATE
	PCSX2_FLAGS
	PCSX2
)

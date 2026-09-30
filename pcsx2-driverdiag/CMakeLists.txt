# armsx2-driverdiag: the GS dump's driver report for a device with no game running, for adb and
# Linux handhelds. Opt-in only: the parent adds this directory EXCLUDE_FROM_ALL, so no default
# build, app or package ever contains it. Build it with --target armsx2-driverdiag.

add_executable(armsx2-driverdiag)

target_sources(armsx2-driverdiag PRIVATE
	Main.cpp
)

target_include_directories(armsx2-driverdiag PRIVATE
	"${CMAKE_BINARY_DIR}/common/include"
	"${CMAKE_SOURCE_DIR}/pcsx2"
)

# Only the driver-report objects, the Vulkan loader and the profile resolver are pulled out of the
# static core; nothing here starts an emulator.
target_link_libraries(armsx2-driverdiag PRIVATE
	PCSX2_FLAGS
	PCSX2
	${CMAKE_DL_LIBS}
)

if(ANDROID)
	target_link_libraries(armsx2-driverdiag PRIVATE adrenotools android log)
endif()

# Runs on every build (see the armsx2_git_hash target) rather than at configure
# time, so the banner reports the commit you actually built.

execute_process(
	COMMAND git rev-parse --short HEAD
	WORKING_DIRECTORY "${GIT_WORKING_DIR}"
	OUTPUT_VARIABLE hash
	OUTPUT_STRIP_TRAILING_WHITESPACE
	ERROR_QUIET
)

if(NOT hash)
	set(hash "unknown")
endif()

set(contents "#pragma once\n#define ARMSX2_GIT_HASH \"${hash}\"\n")

# Only touch the file when the hash moved, otherwise every build would
# recompile the banner and relink for nothing.
if(EXISTS "${OUTPUT_FILE}")
	file(READ "${OUTPUT_FILE}" existing)
	if(existing STREQUAL contents)
		return()
	endif()
endif()

file(WRITE "${OUTPUT_FILE}" "${contents}")

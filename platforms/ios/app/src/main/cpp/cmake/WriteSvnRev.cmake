# Build time twin of get_git_version_info + write_svnrev_h in Pcsx2Utils.cmake.
# Those run at configure time, so the PCSX2 banner reported whatever revision
# cmake last saw. This produces byte identical output on purpose: the only thing
# that changes is when it runs, so configure and build never disagree over the
# file and fight each other into a rebuild loop.

set(tag "")
set(hash "")
set(date "")
set(rev "")

if(EXISTS "${GIT_WORKING_DIR}/.git")
	execute_process(WORKING_DIRECTORY "${GIT_WORKING_DIR}" COMMAND git describe --tags
		OUTPUT_VARIABLE rev OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)

	execute_process(WORKING_DIRECTORY "${GIT_WORKING_DIR}" COMMAND git tag --points-at HEAD --sort=version:refname
		OUTPUT_VARIABLE tag_list RESULT_VARIABLE tag_result
		OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)

	# Last tag wins, same as upstream, for a commit tagged more than once.
	if(tag_list AND tag_result EQUAL 0)
		string(REPLACE "\n" ";" tag_list "${tag_list}")
		if(tag_list)
			list(GET tag_list -1 tag)
		endif()
	endif()

	execute_process(WORKING_DIRECTORY "${GIT_WORKING_DIR}" COMMAND git rev-parse HEAD
		OUTPUT_VARIABLE hash OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)

	execute_process(WORKING_DIRECTORY "${GIT_WORKING_DIR}" COMMAND git log -1 --format=%cd --date=local
		OUTPUT_VARIABLE date OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
endif()

if(NOT rev)
	execute_process(WORKING_DIRECTORY "${GIT_WORKING_DIR}" COMMAND git rev-parse --short HEAD
		OUTPUT_VARIABLE rev OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
	if(NOT rev)
		set(rev "Unknown")
	endif()
endif()

if("${tag}" MATCHES "^v([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
	set(contents
		"#define GIT_TAG \"${tag}\"\n"
		"#define GIT_TAGGED_COMMIT 1\n"
		"#define GIT_TAG_HI  ${CMAKE_MATCH_1}\n"
		"#define GIT_TAG_MID ${CMAKE_MATCH_2}\n"
		"#define GIT_TAG_LO  ${CMAKE_MATCH_3}\n"
		"#define GIT_REV \"${tag}\"\n")
elseif("${rev}" MATCHES "^v([0-9]+)\\.([0-9]+)\\.([0-9]+)")
	set(contents
		"#define GIT_TAG \"${tag}\"\n"
		"#define GIT_TAGGED_COMMIT 0\n"
		"#define GIT_TAG_HI  ${CMAKE_MATCH_1}\n"
		"#define GIT_TAG_MID ${CMAKE_MATCH_2}\n"
		"#define GIT_TAG_LO  ${CMAKE_MATCH_3}\n"
		"#define GIT_REV \"${rev}\"\n")
else()
	set(contents
		"#define GIT_TAG \"${tag}\"\n"
		"#define GIT_TAGGED_COMMIT 0\n"
		"#define GIT_TAG_HI 0\n"
		"#define GIT_TAG_MID 0\n"
		"#define GIT_TAG_LO 0\n"
		"#define GIT_REV \"${rev}\"\n")
endif()

string(APPEND contents_str ${contents}
	"#define GIT_HASH \"${hash}\"\n"
	"#define GIT_DATE \"${date}\"\n")

# Only touch the file when something moved, otherwise every build relinks the
# core through LTO for nothing.
if(EXISTS "${OUTPUT_FILE}")
	file(READ "${OUTPUT_FILE}" existing)
	if(existing STREQUAL contents_str)
		return()
	endif()
endif()

file(WRITE "${OUTPUT_FILE}" "${contents_str}")

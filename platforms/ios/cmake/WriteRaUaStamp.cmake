# Host.cpp includes the header through __has_include, so its object has to depend on whether the header exists.
if(EXISTS "${HEADER}")
	set(contents "present\n")
else()
	set(contents "absent\n")
endif()

if(EXISTS "${OUTPUT_FILE}")
	file(READ "${OUTPUT_FILE}" existing)
	if(existing STREQUAL contents)
		return()
	endif()
endif()

file(WRITE "${OUTPUT_FILE}" "${contents}")

# Fails if LIB uses ELF TLS. Run after linking a library whose minimum Android API is below 29,
# where the dynamic linker ignores TLS descriptors and a thread_local access calls address 0.
#
#   cmake -DREADELF=<llvm-readelf> -DLIB=<library> -DMIN_API=<api> -P CheckNoElfTls.cmake

execute_process(
	COMMAND "${READELF}" -r -l -W "${LIB}"
	OUTPUT_VARIABLE out
	RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
	message(FATAL_ERROR "CheckNoElfTls: ${READELF} failed on ${LIB} (${rc})")
endif()

string(REGEX MATCHALL "R_AARCH64_TLS[A-Z_0-9]*" relocs "${out}")
list(LENGTH relocs n)
if(n GREATER 0 OR out MATCHES "\n[ \t]*TLS[ \t]")
	message(FATAL_ERROR "CheckNoElfTls: ${LIB} has ELF TLS (${n} TLS relocations or a PT_TLS segment), "
		"but its minimum Android API is ${MIN_API}. Android below API 29 ignores these and the first "
		"thread_local access calls address 0. Link the LTO core with emulated TLS.")
endif()

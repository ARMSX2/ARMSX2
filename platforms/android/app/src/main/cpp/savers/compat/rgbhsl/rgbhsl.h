/* Case alias: Helios includes <rgbhsl/rgbhsl.h>, Flux includes <Rgbhsl/Rgbhsl.h>. Windows did
 * not care and neither does macOS, but a case-sensitive build host would fail on one of them.
 *
 * The target is spelled as an explicit relative path, NOT as <Rgbhsl/Rgbhsl.h>. On a
 * case-insensitive filesystem the search path would match "compat/Rgbhsl" to this very
 * directory and the file would include itself -- the guard then silently swallows it and the
 * real declarations never arrive. */
#ifndef SAVERS_RGBHSL_LOWER_ALIAS_H
#define SAVERS_RGBHSL_LOWER_ALIAS_H
#include "../../rslibs/Rgbhsl/Rgbhsl.h"
#endif

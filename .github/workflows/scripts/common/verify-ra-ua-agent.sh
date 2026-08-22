#!/usr/bin/env bash
# Fail the build if the RetroAchievements client identity did not reach the binary.
#
# A reordered step, a stale build directory or a wrong path each produce a working IPA that
# reports itself as stock PCSX2. Nothing on the client says so, and the first sign is a
# player losing a hardcore unlock. Matching the exact version rather than the name also
# catches a build that compiled an older header.

set -euo pipefail

BINARY="${1:-}"

if [[ -z "$BINARY" ]]; then
	echo "::error::usage: verify-ra-ua-agent.sh <path to the built binary>"
	exit 1
fi

if [[ ! -f "$BINARY" ]]; then
	echo "::error::$BINARY does not exist, so the build did not produce what this check reads."
	exit 1
fi

VERSION="$(printf '%s' "${IOS_RA_UA_VERSION:-}" | tr -d '[:space:]')"

if [[ -z "$VERSION" ]]; then
	echo "no secret was supplied, so this build identifies as stock PCSX2. Nothing to verify."
	exit 0
fi

# The trailing space comes from Host.cpp's format string and is load bearing: a stale 1.2.3
# is a prefix of a current 1.2.345, so an unanchored match passes the build this exists to
# catch. grep drains the stream instead of using -q, which exits on the first match and
# SIGPIPEs strings, leaving pipefail to read a hit as a failed pipeline. Output is discarded
# so the version never reaches the log.
if strings -a "$BINARY" | grep -F "ARMSX2-iOS/v$VERSION " >/dev/null; then
	echo "$BINARY carries the RetroAchievements client identity"
	exit 0
fi

echo "::error::$BINARY does not carry the expected client identity, so this build would be softcore only."
exit 1

#!/usr/bin/env bash
# Fail the build if the RetroAchievements client identity did not reach the binary.
#
# A reordered step, stale build directory or wrong path still produces a working IPA that
# identifies as stock PCSX2. Matching the exact version also catches an older header.

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

# Host.cpp puts a space after the version, which anchors the match so 1.2.3 can't pass for
# 1.2.345. grep reads the whole stream instead of -q, which would SIGPIPE strings under
# pipefail. Output is discarded to keep the version out of the log.
if strings -a "$BINARY" | grep -F "ARMSX2-iOS/v$VERSION " >/dev/null; then
	echo "$BINARY carries the RetroAchievements client identity"
	exit 0
fi

echo "::error::$BINARY does not carry the expected client identity, so this build would be softcore only."
exit 1

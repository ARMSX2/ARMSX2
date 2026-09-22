#!/usr/bin/env bash
# Write the RetroAchievements client identity that pcsx2/Host.cpp expects.
#
# Host.cpp includes "ra_ua_secret.h" behind __has_include and falls back to a stock PCSX2
# agent when the macro is absent. RetroAchievements grants hardcore on the leading
# Name/Version token of that agent, so a build without this header is softcore only. The
# header is gitignored, which is why a runner has to write it.
#
# The version lives in a repository secret so forks don't inherit the ARMSX2 identity; it is
# still a plain string in the binary.

set -euo pipefail

HEADER="${GITHUB_WORKSPACE:-$PWD}/pcsx2/ra_ua_secret.h"

# Whitespace pasted into the secret field would end up inside an HTTP header.
VERSION="$(printf '%s' "${IOS_RA_UA_VERSION:-}" | tr -d '[:space:]')"

if [[ -z "$VERSION" ]]; then
	echo "::warning::IOS_RA_UA_VERSION is unset, so this build identifies as stock PCSX2 and RetroAchievements will allow softcore only."
	exit 0
fi

# The client treats a refused agent like an unknown one, so reject a version
# RetroAchievements can't order.
if [[ ! "$VERSION" =~ ^[0-9]+([.-][0-9]+)*$ ]]; then
	echo "::error::IOS_RA_UA_VERSION is not a numeric dotted version. RetroAchievements cannot order it and will treat the client as unknown."
	exit 1
fi

if [[ ! -d "${HEADER%/*}" ]]; then
	echo "::error::${HEADER%/*} does not exist. Run this from the repository checkout."
	exit 1
fi

# Host.cpp's include is quoted, so one file beside it serves every platform building pcsx2/.
printf '#pragma once\n#define ARMSX2_IOS_RA_UA_VERSION "%s"\n' "$VERSION" > "$HEADER"

echo "wrote $HEADER"

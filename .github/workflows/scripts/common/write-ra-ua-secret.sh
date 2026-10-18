#!/usr/bin/env bash
# Write the RetroAchievements client identity that pcsx2/Host.cpp expects.
#
# Host.cpp includes "ra_ua_secret.h" behind __has_include and falls back to a stock PCSX2
# agent when the macro is absent. RetroAchievements grants hardcore on the leading
# Name/Version token of that agent, so a build without this header is softcore only. The
# header is gitignored, which is why a runner has to write it.
#
# The versions live in repository secrets so forks don't inherit the ARMSX2 identity; they are
# still plain strings in the binary.
#
#   RA_UA_VERSION      the ARMSX2 client (Android and desktop)
#   IOS_RA_UA_VERSION  the ARMSX2-iOS client
#   RA_UA_REQUIRED     set by release builds: a missing RA_UA_VERSION then fails the build
#                      instead of warning, so a release can never go out softcore only.

set -euo pipefail

HEADER="${GITHUB_WORKSPACE:-$PWD}/pcsx2/ra_ua_secret.h"

# Whitespace pasted into a secret field would end up inside an HTTP header.
VERSION="$(printf '%s' "${RA_UA_VERSION:-}" | tr -d '[:space:]')"
IOS_VERSION="$(printf '%s' "${IOS_RA_UA_VERSION:-}" | tr -d '[:space:]')"

if [[ -z "$VERSION" && -n "${RA_UA_REQUIRED:-}" ]]; then
	echo "::error::RA_UA_VERSION is unset. This is a release build, and without it RetroAchievements would allow softcore only."
	exit 1
fi

if [[ -z "$VERSION" && -z "$IOS_VERSION" ]]; then
	echo "::warning::Neither RA_UA_VERSION nor IOS_RA_UA_VERSION is set, so this build identifies as stock PCSX2 and RetroAchievements will allow softcore only."
	exit 0
fi

# The client treats a refused agent like an unknown one, so reject a version
# RetroAchievements can't order.
for pair in "RA_UA_VERSION=$VERSION" "IOS_RA_UA_VERSION=$IOS_VERSION"; do
	name="${pair%%=*}"
	value="${pair#*=}"
	if [[ -n "$value" && ! "$value" =~ ^[0-9]+([.-][0-9]+)*$ ]]; then
		echo "::error::$name is not a numeric dotted version. RetroAchievements cannot order it and will treat the client as unknown."
		exit 1
	fi
done

if [[ ! -d "${HEADER%/*}" ]]; then
	echo "::error::${HEADER%/*} does not exist. Run this from the repository checkout."
	exit 1
fi

# Host.cpp's include is quoted, so one file beside it serves every platform building pcsx2/.
{
	printf '#pragma once\n'
	[[ -z "$VERSION" ]] || printf '#define ARMSX2_RA_UA_VERSION "%s"\n' "$VERSION"
	[[ -z "$IOS_VERSION" ]] || printf '#define ARMSX2_IOS_RA_UA_VERSION "%s"\n' "$IOS_VERSION"
} > "$HEADER"

echo "wrote $HEADER"

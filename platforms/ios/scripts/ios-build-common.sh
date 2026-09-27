# Sourced by build-ios-ipa.sh and build-ios-ipa-fast.sh after they set ROOT_DIR,
# BUILD_DIR and PROJECT.

LIBRASHADER_RUST_TARGET="aarch64-apple-ios"

require_tool() {
	if ! command -v "$1" >/dev/null 2>&1; then
		echo "error: required tool '$1' was not found." >&2
		exit 1
	fi
}

ensure_librashader_toolchain() {
	if [[ -z "${CARGO_HOME:-}" && -x "$BUILD_DIR/cargo-home/bin/cargo" ]]; then
		export RUSTUP_HOME="$BUILD_DIR/rustup-home" CARGO_HOME="$BUILD_DIR/cargo-home"
		export PATH="$CARGO_HOME/bin:$PATH"
	fi

	if ! command -v cargo >/dev/null 2>&1 \
		|| ! command -v rustc >/dev/null 2>&1; then
		# Only a machine without Rust gets the build-local homes. Pointing an
		# existing rustup at them left it with no toolchain.
		export RUSTUP_HOME="${RUSTUP_HOME:-$BUILD_DIR/rustup-home}"
		export CARGO_HOME="${CARGO_HOME:-$BUILD_DIR/cargo-home}"
		require_tool curl
		local rustup_platform rustup_url installer expected_hash actual_hash
		case "$(uname -m)" in
			arm64) rustup_platform="aarch64-apple-darwin" ;;
			x86_64) rustup_platform="x86_64-apple-darwin" ;;
			*) echo "error: unsupported Rust host architecture: $(uname -m)" >&2; exit 1 ;;
		esac
		rustup_url="https://static.rust-lang.org/rustup/dist/$rustup_platform/rustup-init"
		installer="$BUILD_DIR/rustup-init"
		echo "Installing the project-local Rust toolchain required by librashader..."
		curl --proto '=https' --tlsv1.2 --fail --location --silent --show-error \
			"$rustup_url" --output "$installer"
		expected_hash="$(curl --proto '=https' --tlsv1.2 --fail --location --silent --show-error \
			"$rustup_url.sha256" | awk '{print $1}')"
		actual_hash="$(shasum -a 256 "$installer" | awk '{print $1}')"
		if [[ -z "$expected_hash" || "$actual_hash" != "$expected_hash" ]]; then
			echo "error: rustup-init checksum verification failed." >&2
			exit 1
		fi
		chmod u+x "$installer"
		"$installer" -y --no-modify-path --profile minimal --default-toolchain stable
		export PATH="$CARGO_HOME/bin:$PATH"
	fi

	if command -v rustup >/dev/null 2>&1; then
		rustup target add "$LIBRASHADER_RUST_TARGET"
	fi

	local target_libdir
	target_libdir="$(rustc --print target-libdir --target "$LIBRASHADER_RUST_TARGET" 2>/dev/null || true)"
	if [[ -z "$target_libdir" || ! -d "$target_libdir" ]]; then
		echo "error: Rust target $LIBRASHADER_RUST_TARGET is required for the iOS shader chain." >&2
		echo "Install it with: rustup target add $LIBRASHADER_RUST_TARGET" >&2
		exit 1
	fi
}

refresh_generated_git_metadata() {
	local short_hash full_hash git_date pbxproj svnrev_file
	short_hash="$(git -C "$ROOT_DIR" rev-parse --short HEAD 2>/dev/null || true)"
	full_hash="$(git -C "$ROOT_DIR" rev-parse HEAD 2>/dev/null || true)"
	git_date="$(git -C "$ROOT_DIR" log -1 --format=%cd --date=local 2>/dev/null || true)"
	[[ -n "$short_hash" ]] || return 0

	pbxproj="$PROJECT/project.pbxproj"
	if [[ -f "$pbxproj" ]]; then
		SHORT_HASH="$short_hash" perl -0pi -e \
			's/ARMSX2_GIT_HASH=\\"[0-9A-Fa-f]+\\"/ARMSX2_GIT_HASH=\\"$ENV{SHORT_HASH}\\"/g' \
			"$pbxproj"
	fi

	svnrev_file="$BUILD_DIR/common/include/svnrev.h"
	if [[ -f "$svnrev_file" ]]; then
		SHORT_HASH="$short_hash" FULL_HASH="$full_hash" GIT_DATE_TEXT="$git_date" perl -0pi -e '
			s/(#define GIT_REV "[^"]*-g)[0-9A-Fa-f]+(")/$1$ENV{SHORT_HASH}$2/g;
			s/(#define GIT_HASH ")[^"]*(")/$1$ENV{FULL_HASH}$2/g;
			s/(#define GIT_DATE ")[^"]*(")/$1$ENV{GIT_DATE_TEXT}$2/g;
		' "$svnrev_file"
	fi
}

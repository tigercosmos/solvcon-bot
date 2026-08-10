#!/usr/bin/env bash
# Install the pinned codexmon release binary.
#
# codexmon (https://github.com/tigercosmos/codexmon) is the
# health-monitoring wrapper the bot spawns to run every AI reviewer
# (claude / codex / cursor). This script downloads the pinned release
# for the current OS/arch, verifies it against the release's
# SHA256SUMS, and installs it as an executable.
#
# Usage:
#   ./scripts/install_codexmon.sh [DEST_DIR]
#
#   DEST_DIR                 install directory (default:
#                            $CODEXMON_INSTALL_DIR, else ~/.local/bin)
#   CODEXMON_VERSION=X.Y.Z   override the pinned version
#
# The bot looks codexmon up on PATH; if DEST_DIR is not on PATH, set
# CODEXMON_BIN=DEST_DIR/codexmon in the bot's environment instead.

set -euo pipefail

VERSION="${CODEXMON_VERSION:-0.8.0}"
DEST="${1:-${CODEXMON_INSTALL_DIR:-$HOME/.local/bin}}"

case "$(uname -s)" in
    Darwin) os="darwin" ;;
    Linux)  os="linux" ;;
    *) echo "install_codexmon: unsupported OS: $(uname -s)" >&2; exit 1 ;;
esac
case "$(uname -m)" in
    arm64|aarch64) arch="arm64" ;;
    x86_64|amd64)  arch="amd64" ;;
    *) echo "install_codexmon: unsupported arch: $(uname -m)" >&2; exit 1 ;;
esac

asset="codexmon_${VERSION}_${os}_${arch}.tar.gz"
base="https://github.com/tigercosmos/codexmon/releases/download/v${VERSION}"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

echo "install_codexmon: fetching ${asset} (v${VERSION})"
curl -fsSL -o "${tmp}/${asset}" "${base}/${asset}"
curl -fsSL -o "${tmp}/SHA256SUMS" "${base}/SHA256SUMS"

# Verify the archive against the release checksum before extracting.
(
    cd "$tmp"
    if command -v sha256sum > /dev/null 2>&1; then
        grep " ${asset}\$" SHA256SUMS | sha256sum -c -
    else
        grep " ${asset}\$" SHA256SUMS | shasum -a 256 -c -
    fi
)

tar -xzf "${tmp}/${asset}" -C "$tmp" codexmon
mkdir -p "$DEST"
install -m 0755 "${tmp}/codexmon" "${DEST}/codexmon"

echo "install_codexmon: installed $("${DEST}/codexmon" version | head -1) -> ${DEST}/codexmon"
case ":$PATH:" in
    *":${DEST}:"*) ;;
    *) echo "install_codexmon: NOTE: ${DEST} is not on PATH — set CODEXMON_BIN=${DEST}/codexmon for the bot" ;;
esac

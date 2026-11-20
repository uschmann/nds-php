#!/bin/sh
# Assemble the GitHub Pages site into _site/.
# Expects nds-php.nds to be built already (make / ./build.sh).
#
#   REPO_URL=https://github.com/user/nds-php BUILD_SHA=abc1234 site/build-site.sh
set -e
cd "$(dirname "$0")/.."

[ -f nds-php.nds ] || { echo "nds-php.nds missing - build the ROM first" >&2; exit 1; }

OUT=_site
rm -rf "$OUT"
mkdir -p "$OUT"
cp site/*.html site/style.css "$OUT/"
cp -r site/img "$OUT/img"
cp nds-php.nds README.md README.en.md "$OUT/"
cp -r examples "$OUT/examples"
touch "$OUT/.nojekyll"

ROM_SIZE="$(( ($(wc -c < nds-php.nds) + 1023) / 1024 )) KB"
REPO_URL="${REPO_URL:-https://github.com}"
BUILD_SHA="${BUILD_SHA:-$(git rev-parse --short HEAD 2>/dev/null || echo dev)}"

for f in "$OUT"/*.html; do
	sed -i -e "s|@ROM_SIZE@|$ROM_SIZE|g" -e "s|@REPO_URL@|$REPO_URL|g" -e "s|@BUILD_SHA@|$BUILD_SHA|g" "$f"
done
echo "site assembled in $OUT ($ROM_SIZE ROM, build $BUILD_SHA)"

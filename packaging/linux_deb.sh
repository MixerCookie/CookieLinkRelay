#!/usr/bin/env bash
set -euo pipefail

version="${1:-${GITHUB_REF_NAME:-v1.0.0}}"
build_dir="${2:-build}"
dist_dir="${3:-dist}"
artifact_root="${COOKIELINK_RELAY_ARTIFACT_ROOT:-${build_dir}/CookieLinkRelay_artefacts/Release}"
pkg_version="${version#v}"

work_dir="$(mktemp -d)"
trap 'rm -rf "${work_dir}"' EXIT

pkg_root="${work_dir}/cookielink-relay"
output_deb="${dist_dir}/CookieLinkRelay-${version}-linux-x64.deb"

if [[ ! -e "${artifact_root}/CookieLinkRelay" ]]; then
    echo "Missing relay binary: ${artifact_root}/CookieLinkRelay" >&2
    exit 1
fi

mkdir -p "${dist_dir}" "${pkg_root}/DEBIAN" "${pkg_root}/usr/bin"

cat > "${pkg_root}/DEBIAN/control" <<CONTROL
Package: cookielink-relay
Version: ${pkg_version}
Section: sound
Priority: optional
Architecture: amd64
Maintainer: Cookie Studio <noreply@example.com>
Description: CookieLink TCP relay server
 CookieLinkRelay provides a fixed relay endpoint for CookieLink clients when direct peer networking is unstable.
CONTROL

cp "${artifact_root}/CookieLinkRelay" "${pkg_root}/usr/bin/cookielink-relay"
chmod 0755 "${pkg_root}/usr/bin/cookielink-relay"

mkdir -p "${pkg_root}/usr/share/doc/cookielink-relay"
cp -a README.md LICENSE LICENSE_EXCEPTION "${pkg_root}/usr/share/doc/cookielink-relay/"

dpkg-deb --build --root-owner-group "${pkg_root}" "${output_deb}"

echo "${output_deb}"

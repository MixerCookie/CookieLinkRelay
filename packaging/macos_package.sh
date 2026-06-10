#!/usr/bin/env bash
set -euo pipefail

version="${1:-${GITHUB_REF_NAME:-v1.7.2}}"
build_dir="${2:-build}"
dist_dir="${3:-dist}"
artifact_root="${COOKIELINK_RELAY_ARTIFACT_ROOT:-${build_dir}/CookieLinkRelay_artefacts/Release}"
pkg_version="${version#v}"

work_dir="$(mktemp -d)"
trap 'rm -rf "${work_dir}"' EXIT

pkg_root="${work_dir}/pkgroot"
component_pkg="${work_dir}/CookieLinkRelay-component.pkg"
output_pkg="${dist_dir}/CookieLinkRelay-${version}-macos-universal.pkg"

if [[ ! -e "${artifact_root}/CookieLinkRelay" ]]; then
    echo "Missing relay binary: ${artifact_root}/CookieLinkRelay" >&2
    exit 1
fi

mkdir -p "${dist_dir}" "${pkg_root}/usr/local/bin"
cp "${artifact_root}/CookieLinkRelay" "${pkg_root}/usr/local/bin/CookieLinkRelay"
chmod 0755 "${pkg_root}/usr/local/bin/CookieLinkRelay"

pkgbuild \
    --root "${pkg_root}" \
    --identifier "com.cookiestudio.cookielinkrelay.pkg" \
    --version "${pkg_version}" \
    --install-location "/" \
    "${component_pkg}"

productbuild \
    --package "${component_pkg}" \
    "${output_pkg}"

echo "${output_pkg}"
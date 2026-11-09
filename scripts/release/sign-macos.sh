#!/usr/bin/env bash
# Sign the macOS release binary with the Developer ID certificate and have
# Apple notarize it, so Gatekeeper accepts it however it was downloaded
# (a browser download no longer needs `xattr -d com.apple.quarantine`).
#
# Usage: scripts/release/sign-macos.sh <binary>
#
# Environment (repository secrets, passed in by release.yml):
#   MACOS_CERT_P12       base64 of the Developer ID Application certificate
#                        and its private key, exported as .p12
#   MACOS_CERT_PASSWORD  the .p12 export password
#   ASC_KEY_ID           App Store Connect API key id
#   ASC_ISSUER_ID        App Store Connect issuer id
#   ASC_KEY_P8           the API key itself (contents of AuthKey_<id>.p8)
#
# A bare executable can't carry a stapled ticket: Gatekeeper looks the
# notarization up online the first time the binary runs.
set -euo pipefail

bin=$1
work=$(mktemp -d)
keychain="$work/signing.keychain-db"
keychain_password=$(uuidgen)
cleanup() {
    security delete-keychain "$keychain" 2> /dev/null || true
    rm -rf "$work"
}
trap cleanup EXIT

# A throwaway keychain holding only the Developer ID identity.
printf '%s' "$MACOS_CERT_P12" | base64 --decode > "$work/cert.p12"
security create-keychain -p "$keychain_password" "$keychain"
security set-keychain-settings -lut 3600 "$keychain"
security unlock-keychain -p "$keychain_password" "$keychain"
security import "$work/cert.p12" -k "$keychain" -P "$MACOS_CERT_PASSWORD" -T /usr/bin/codesign
security set-key-partition-list -S apple-tool:,apple: -s -k "$keychain_password" "$keychain" > /dev/null
# codesign looks identities up through the search list: add ours to it.
existing=$(security list-keychains -d user | tr -d '"')
# shellcheck disable=SC2086 # one keychain path per word
security list-keychains -d user -s "$keychain" $existing
identity=$(security find-identity -v -p codesigning "$keychain" |
    awk -F'"' '/Developer ID Application/ { print $2; exit }')
if [ -z "$identity" ]; then
    echo "::error::MACOS_CERT_P12 holds no Developer ID Application identity"
    exit 1
fi

# Hardened runtime and a secure timestamp: both required for notarization.
codesign --force --options runtime --timestamp --keychain "$keychain" --sign "$identity" "$bin"
codesign --verify --strict --verbose=2 "$bin"
codesign --display --verbose=2 "$bin" 2>&1 | grep -E '^(Authority|TeamIdentifier|Timestamp)|flags='

printf '%s\n' "$ASC_KEY_P8" > "$work/key.p8"
ditto -c -k --keepParent "$bin" "$work/pptctl.zip"
xcrun notarytool submit "$work/pptctl.zip" \
    --key "$work/key.p8" --key-id "$ASC_KEY_ID" --issuer "$ASC_ISSUER_ID" \
    --wait --timeout 30m --output-format json > "$work/submission.json"
status=$(jq -r .status "$work/submission.json")
id=$(jq -r .id "$work/submission.json")
echo "notarization $id: $status"
if [ "$status" != Accepted ]; then
    xcrun notarytool log "$id" \
        --key "$work/key.p8" --key-id "$ASC_KEY_ID" --issuer "$ASC_ISSUER_ID" || true
    echo "::error::notarization $id ended as $status"
    exit 1
fi

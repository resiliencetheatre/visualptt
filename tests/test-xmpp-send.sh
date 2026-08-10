#!/bin/sh
set -eu

program=${1:-./visualptt-xmpp-send}
case "$program" in /*) ;; *) program="$(pwd)/${program#./}" ;; esac
work=$(mktemp -d /tmp/visualptt-xmpp-test-XXXXXX)
trap 'rm -rf "$work"' EXIT HUP INT TERM

expect_status() {
    expected=$1
    shift
    set +e
    "$@" >"$work/stdout" 2>"$work/stderr"
    actual=$?
    set -e
    if [ "$actual" -ne "$expected" ]; then
        echo "FAIL: expected status $expected, got $actual: $*" >&2
        cat "$work/stderr" >&2
        exit 1
    fi
}

"$program" --help >"$work/help"
grep -q -- '--keep-converted' "$work/help"
expect_status 2 "$program"
expect_status 4 "$program" --to alice@example.com "$work/missing.mkv"

: >"$work/recording.mkv"
expect_status 4 "$program" --to alice@example.com \
    --annotation "$work/missing.txt" "$work/recording.mkv"

printf '%s\n' '[xmpp]' 'jid=broken' >"$work/bad.ini"
expect_status 3 "$program" --config "$work/bad.ini" \
    --to alice@example.com "$work/recording.mkv"
grep -q 'malformed configuration' "$work/stderr"

printf '%s\n' '[xmpp]' 'jid=test@example.com' 'password=not-printed' \
    'server=127.0.0.1' 'port=9' >"$work/good.ini"
expect_status 5 "$program" --config "$work/good.ini" --ffmpeg /no/such/ffmpeg \
    --to alice@example.com "$work/recording.mkv"
if grep -R -q 'not-printed' "$work/stdout" "$work/stderr"; then
    echo 'FAIL: password appeared in output' >&2
    exit 1
fi

before=$(find /tmp -maxdepth 1 -type d -name 'visualptt-xmpp-*' | wc -l)
expect_status 6 "$program" --config "$work/good.ini" --ffmpeg "$(pwd)/tests/fake-ffmpeg.sh" \
    --timeout 1 --to alice@example.com "$work/recording.mkv"
after=$(find /tmp -maxdepth 1 -type d -name 'visualptt-xmpp-*' | wc -l)
if [ "$before" -ne "$after" ]; then
    echo 'FAIL: temporary conversion directory was not cleaned up' >&2
    exit 1
fi

echo 'Basic XMPP sender tests passed'

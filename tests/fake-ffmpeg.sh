#!/bin/sh
set -eu
for argument do output=$argument; done
printf 'fake mp4 data' >"$output"

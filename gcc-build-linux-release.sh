#!/usr/bin/env bash
exec "$(dirname "$0")/scripts/build.sh" linux-gcc-release "$@"

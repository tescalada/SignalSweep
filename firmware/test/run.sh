#!/usr/bin/env bash
# Host-side C++ tests for the attack-detector window logic and the Nearby LED
# mode. Pure functions, no ESP headers, so plain g++ compiles them. Runs
# locally (WSL) and in CI.
set -euo pipefail
cd "$(dirname "$0")"
CXX="${CXX:-g++}"
outdir="$(mktemp -d)"
for t in attack_detectors_test nearby_test; do
    "$CXX" -std=c++17 -Wall -Wextra -O1 "$t.cpp" -o "$outdir/$t"
    "$outdir/$t"
done

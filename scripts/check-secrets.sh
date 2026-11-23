#!/bin/bash
# Crumple Zone: make sure no value from .env ever reaches git.
#
# Copyright (C) 2026 chinesepowered
# SPDX-License-Identifier: AGPL-3.0-only
#
# Reads every value in .env and searches the working tree (tracked and
# untracked, excluding .env itself) and the full history of every ref for it.
# Prints only variable names and counts, never the values. Exit 1 on a hit.
set -u
cd "$(dirname "$0")/.."
[ -f .env ] || { echo "no .env, nothing to check"; exit 0; }
fail=0
while IFS='=' read -r name value; do
    [[ -z "$name" || "$name" =~ ^# || -z "$value" ]] && continue
    # Skip non-secret configuration values.
    case "$name" in OPENAI_BASE_URL|OPENAI_MODEL|OPENAI_FALLBACK_MODEL|ELEVENLABS_VOICE_ID|ELEVENLABS_MODEL_ID) continue ;; esac
    tree=$(git ls-files -co --exclude-standard | grep -v '^\.env$' | xargs -r grep -F -l -- "$value" 2>/dev/null | wc -l)
    hist=$(git log --all -p --no-color 2>/dev/null | grep -F -c -- "$value")
    msgs=$(git log --all --format=%B 2>/dev/null | grep -F -c -- "$value")
    if [ "$tree" -ne 0 ] || [ "$hist" -ne 0 ] || [ "$msgs" -ne 0 ]; then
        echo "LEAK: $name found (files: $tree, history lines: $hist, commit messages: $msgs)"
        fail=1
    else
        echo "ok: $name not in working tree or history"
    fi
done < .env
if git ls-files --error-unmatch .env >/dev/null 2>&1 || git log --all --format= --name-only | grep -qx '.env'; then
    echo "LEAK: .env is or was tracked"
    fail=1
fi
exit $fail

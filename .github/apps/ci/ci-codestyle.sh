#!/bin/bash

. "$(dirname "${BASH_SOURCE[0]}")/codestyle-scope.sh"

set -u

echo "Starting Codestyling Script:"
echo

mapfile -t files < <(git ls-files | grep -vE "${codestyle_exclude_re}")
count=${#files[@]}

if [ "${count}" -lt "${codestyle_minimum_files}" ]; then
    echo "Only ${count} files in scope, expected at least ${codestyle_minimum_files}."
    echo "The scan is not reading the tree; failing instead of passing quietly."
    exit 1
fi

echo "  ${count} files in scope"
echo

failed=0

# Every rule runs, and every rule reports, so one push answers all of them
# instead of one per round trip.
#
# The `|| true` is not decoration. This file is SOURCED by the workflow, into a
# shell GitHub starts with `bash -e -o pipefail`, and grep exits 1 when it finds
# nothing -- which is the good case. Without it the first clean rule killed the
# step, printing the file count and then exit 1 with no rule output at all.
check() {
    local name="$1" pattern="$2" advice="$3" hits

    hits=$(grep -n -I -P -- "${pattern}" "${files[@]}" 2>/dev/null || true)

    if [ -n "${hits}" ]; then
        echo "  ${name}:"
        echo "${hits}"
        echo "  ${advice}"
        echo
        failed=1
    else
        echo "  ${name}: clean"
    fi
}

check "tabs" $'\t' "Replace tabs with 4 spaces in the lines above"
check "trailing whitespace" '[[:blank:]]+$' "Remove whitespace at the end of the lines above"

echo

if [ "${failed}" -ne 0 ]; then
    echo "Codestyle failed."
    exit 1
fi

echo "Awesome! No issues..."

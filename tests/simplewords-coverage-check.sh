#!/bin/sh
set -eu

if [ "$#" -ne 1 ] || [ ! -r "$1" ]; then
    echo "usage: simplewords-coverage-check.sh gcov-function-report" >&2
    exit 2
fi

awk '
BEGIN {
    minimum["write_document"] = 70
    minimum["save_document_to_path"] = 60
    minimum["autosave_file_common"] = 60
    minimum["kill_buffer_index"] = 15
    minimum["read_document_lines"] = 80
    minimum["disk_revision_changed"] = 70
    minimum["replace_range_raw"] = 60
}
/^Function '\''/ {
    name = $0
    sub(/^Function '\''/, "", name)
    sub(/'\''$/, "", name)
    next
}
/^Lines executed:/ && (name in minimum) {
    value = $0
    sub(/^Lines executed:/, "", value)
    sub(/%.*/, "", value)
    seen[name] = 1
    if ((value + 0) < minimum[name]) {
        printf "coverage gate: %s is %.2f%%; need at least %d%%\n", \
               name, value + 0, minimum[name] > "/dev/stderr"
        failed = 1
    }
    name = ""
}
END {
    for (required in minimum) {
        if (!seen[required]) {
            printf "coverage gate: missing function %s\n", required \
                   > "/dev/stderr"
            failed = 1
        }
    }
    exit failed
}
' "$1"

echo "SimpleWords destructive-path coverage thresholds passed"

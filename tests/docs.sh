#!/bin/sh
# Check required source-file and function comments, including test helpers.
set -eu
cd "$(dirname "$0")/.."

awk '
function fail(message) {
    printf "%s:%d: %s\n", FILENAME, FNR, message > "/dev/stderr"
    failures++
}
FNR == 1 {
    previous = ""
    before_previous = ""
    if (FILENAME ~ /\.(c|h)$/ && $0 !~ /^[[:space:]]*\/\*/)
        fail("missing source-purpose comment")
}
FNR == 2 && FILENAME ~ /\.sh$/ && $0 !~ /^#[[:space:]]+[^[:space:]]/ {
    fail("missing shell script purpose comment after shebang")
}
{
    if (FILENAME ~ /\.sh$/ &&
        $0 ~ /^[[:space:]]*[a-zA-Z_][a-zA-Z_0-9]*\(\)[[:space:]]*\{/) {
        if (previous !~ /^[[:space:]]*#[[:space:]]+[^[:space:]]/)
            fail("shell function lacks preceding documentation comment")
    }
    if (FILENAME ~ /\.h$/ && $0 ~ /\);[[:space:]]*$/ &&
        $0 !~ /^[[:space:]]*_Static_assert\(/) {
        if (previous !~ /\*\/[[:space:]]*$/)
            fail("header declaration lacks preceding documentation comment")
    }
    if (FILENAME ~ /\.c$/ && $0 ~ /^[[:space:]]*\{[[:space:]]*$/ &&
        previous ~ /\)[[:space:]]*$/ &&
        previous !~ /^[[:space:]]*(if|while|for|switch)[[:space:]]*\(/) {
        if (before_previous !~ /\*\/[[:space:]]*$/)
            fail("C function lacks preceding documentation comment")
    }
    before_previous = previous
    previous = $0
}
END {
    if (failures) exit 1
    print "PASS documentation comments"
}
' src/*.[ch] tests/*.[ch] tests/*.sh

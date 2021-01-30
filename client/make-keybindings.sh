#!/bin/sh
sed -nr -e '1,/^\|-\|/d;/^$/q;s/<\/?kbd>//g;s/^\| //;s/ \|$//;s/ +\| +/|/;p' "$1" | column -ts\| > "$2"
truncate -s +1 "$2"

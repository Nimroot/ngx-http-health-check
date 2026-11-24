#!/bin/sh
# Run by socat for each connection: reads the request header, then sends
# /tmp/raw/resp as it is and closes the connection.
cr=$(printf '\r')
while IFS= read -r line; do
    [ -z "$line" ] || [ "$line" = "$cr" ] && break
done
cat /tmp/raw/resp

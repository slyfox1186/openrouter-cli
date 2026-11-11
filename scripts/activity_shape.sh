#!/usr/bin/env bash
# Usage: bash scripts/activity_shape.sh
#
# Prints the shape of OpenRouter's GET /activity reply for the Management key that
# openrouter-cli stores: field names, JSON types, the date's digit pattern and the row count.
# Use it when `account activity` reports an unexpected response, to see how the live reply
# differs from what src/account.cpp parses. The key is passed to curl on stdin, so it never
# appears in the process list or on screen, and no model name, cost or other value is printed.
set -euo pipefail

cred="$HOME/.local/state/openrouter-cli/credentials/management-key.cred"
user_agent='Mozilla/5.0 (X11; Linux x86_64; rv:159.0) Gecko/20100101 Firefox/159.0'
url='https://openrouter.ai/api/v1/activity'

for tool in systemd-creds curl jq; do
    if ! command -v "$tool" >/dev/null; then
        echo "error: $tool is not installed" >&2
        exit 1
    fi
done
if [[ ! -f "$cred" ]]; then
    echo "error: no stored Management key at $cred" >&2
    exit 1
fi

body="$(mktemp)"
trap 'rm -f -- "$body"' EXIT

key="$(systemd-creds --user --no-ask-password --newline=no \
    --name=openrouter-cli-management-key decrypt "$cred" -)"
status="$(printf 'Authorization: Bearer %s\n' "$key" |
    curl -sS -A "$user_agent" -H @- -o "$body" -w '%{http_code}' -- "$url")"
unset key

echo "HTTP $status, $(wc -c <"$body") bytes"
if ! jq -e . "$body" >/dev/null 2>&1; then
    echo "the reply is not JSON"
    exit 0
fi
# openrouter-cli reads counts as whole numbers only, so "integer" (written without a point or
# exponent, as in 5) is told apart from "number" (as in 5.0 or 0.015).
jq '
  def kind: if type == "number" and (tojson | test("^-?[0-9]+$")) then "integer" else type end;
  if type != "object" then {top: type}
  else {
    top: keys,
    data: (.data | type),
    rows: (if (.data | type) == "array" then (.data | length) else null end),
    fields: ([.data | arrays | .[] | if type == "object" then with_entries(.value |= kind) else type end] | unique),
    dates: ([.data | arrays | .[] | objects | .date | tostring | gsub("[0-9]"; "9")] | unique)
  } end
' "$body"

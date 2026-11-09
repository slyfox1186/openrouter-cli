#!/usr/bin/env bash
# Test stand-in for systemd-creds. It accepts exactly the argv openrouter-cli passes:
#   --user --no-ask-password --name=NAME encrypt - -
#   --user --no-ask-password --newline=no --name=NAME decrypt - -
# "Encryption" is FAKECRED:NAME: followed by base64 of stdin, so a credential decrypted under
# another name fails. Each variable below changes the behaviour when set to 1:
#   FAKE_CREDS_FAIL     exit 3
#   FAKE_CREDS_HANG     never finish
#   FAKE_CREDS_HUGE     write 200000 bytes
#   FAKE_CREDS_CORRUPT  decrypt returns something other than what was encrypted
set -euo pipefail

# stdin is drained before anything else so the caller never writes into a closed pipe, which
# would raise SIGPIPE in a test process that does not ignore it.
stdin_base64=$(base64 -w0)

[[ $# -ge 1 && $1 == --user ]] || exit 2
shift
[[ $# -ge 1 && $1 == --no-ask-password ]] || exit 2
shift
newline_no=0
if [[ $# -ge 1 && $1 == --newline=no ]]; then
    newline_no=1
    shift
fi
[[ $# -ge 1 && $1 == --name=* ]] || exit 2
name=${1#--name=}
shift
[[ $# -eq 3 && $2 == - && $3 == - ]] || exit 2
operation=$1
case $operation in
    encrypt) [[ $newline_no -eq 0 ]] || exit 2 ;;
    decrypt) [[ $newline_no -eq 1 ]] || exit 2 ;;
    *) exit 2 ;;
esac

if [[ ${FAKE_CREDS_FAIL:-} == 1 ]]; then
    exit 3
fi
if [[ ${FAKE_CREDS_HANG:-} == 1 ]]; then
    # exec, so the caller's SIGKILL reaches the process that holds its stdout pipe.
    exec sleep 600
fi
if [[ ${FAKE_CREDS_HUGE:-} == 1 ]]; then
    printf '%*s' 200000 ''
    exit 0
fi

prefix="FAKECRED:${name}:"
if [[ $operation == encrypt ]]; then
    printf '%s%s' "$prefix" "$stdin_base64"
    exit 0
fi

ciphertext=$(printf '%s' "$stdin_base64" | base64 -d)
[[ $ciphertext == "$prefix"* ]] || exit 1
printf '%s' "${ciphertext#"$prefix"}" | base64 -d
if [[ ${FAKE_CREDS_CORRUPT:-} == 1 ]]; then
    printf 'corrupt'
fi

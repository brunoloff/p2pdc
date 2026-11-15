#!/usr/bin/env bash
set -euo pipefail
set +x
umask 077
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
if [[ $# -lt 1 || $# -gt 3 || -z "$1" || ( ${2:-phone-responder} != phone-responder && ${2:-phone-responder} != phone-initiator ) || ( ${3:-direct} != direct && ${3:-direct} != relay && ${3:-direct} != phone-auto && ${3:-direct} != linux-auto && ${3:-direct} != phone-fallback && ${3:-direct} != blocked-direct ) ]]; then
    echo 'Usage: run_android_internet_echo.sh DEVICE_SERIAL [phone-responder|phone-initiator] [direct|relay|phone-auto|linux-auto|phone-fallback|blocked-direct]' >&2
    exit 2
fi
serial=$1
mobile_role=responder
local_role=initiator
if [[ ${2:-} == phone-initiator ]]; then mobile_role=initiator; local_role=responder; fi
turn_env=()
remote_turn=''
runner_env=()
for variable in P2PDC_TEST_BROWSER_PROXY P2PDC_TEST_BROWSER_ENGINE P2PDC_TEST_ICE_TRACE PLAYWRIGHT_BROWSERS_PATH; do
    if [[ -n ${!variable:-} ]]; then runner_env+=("$variable=${!variable}"); fi
done
native_command=("$root/build-native/p2pdc_echo")
if [[ -n ${P2PDC_TEST_NATIVE_RUNNER:-} ]]; then
    if [[ ! -x "$P2PDC_TEST_NATIVE_RUNNER" ]]; then echo 'Native test runner is not executable.' >&2; exit 2; fi
    native_command=("$P2PDC_TEST_NATIVE_RUNNER" "$root/build-native/p2pdc_echo")
fi
if [[ ( ${3:-direct} == phone-fallback || ${3:-direct} == blocked-direct ) && -z ${P2PDC_TEST_NATIVE_RUNNER:-} ]]; then
    echo 'Phone fallback mode needs the isolated network runner.' >&2; exit 2
fi
if [[ ${3:-direct} != direct && ${3:-direct} != blocked-direct ]]; then
    local_policy=required mobile_policy=required
    if [[ ${3:-} == phone-auto ]]; then mobile_policy=automatic; fi
    if [[ ${3:-} == linux-auto ]]; then local_policy=automatic; fi
    if [[ ${3:-} == phone-fallback ]]; then local_policy=automatic; mobile_policy=automatic; fi
    turn_host=${P2PDC_TEST_TURN_HOST:-} turn_user=${P2PDC_TEST_TURN_USER:-}
    turn_password=${P2PDC_TEST_TURN_PASSWORD:-} turn_port=${P2PDC_TEST_TURN_PORT:-3478}
    turn_transport=${P2PDC_TEST_TURN_TRANSPORT:-udp}
    if [[ ! "$turn_host" =~ ^[A-Za-z0-9.-]+$ || ! "$turn_user" =~ ^[A-Za-z0-9._:-]+$ || \
          ! "$turn_password" =~ ^[A-Za-z0-9._:+/=-]+$ || ! "$turn_port" =~ ^[0-9]{1,5}$ ]] || \
       (( 10#$turn_port < 1 || 10#$turn_port > 65535 )) || [[ "$turn_transport" != udp && "$turn_transport" != tcp ]]; then
        echo 'Relay test requires valid P2PDC_TEST_TURN_HOST, USER, PASSWORD, PORT and TRANSPORT.' >&2; exit 2
    fi
    turn_env=("P2PDC_ECHO_TURN_HOST=$turn_host" "P2PDC_ECHO_TURN_USER=$turn_user"
              "P2PDC_ECHO_TURN_PASSWORD=$turn_password" "P2PDC_ECHO_TURN_PORT=$turn_port"
              "P2PDC_ECHO_TURN_TRANSPORT=$turn_transport" "P2PDC_ECHO_RELAY_POLICY=$local_policy")
    if [[ ${3:-} == phone-fallback ]]; then turn_env=(); fi
    remote_turn="P2PDC_ECHO_TURN_HOST='$turn_host' P2PDC_ECHO_TURN_USER='$turn_user' P2PDC_ECHO_TURN_PASSWORD='$turn_password' P2PDC_ECHO_TURN_PORT='$turn_port' P2PDC_ECHO_TURN_TRANSPORT='$turn_transport' P2PDC_ECHO_RELAY_POLICY='$mobile_policy'"
fi
adb_command=${P2PDC_ADB:-adb}
for command in "$adb_command" openssl setsid timeout; do command -v "$command" >/dev/null; done
for file in "$root/build-native/p2pdc_echo" "$root/.deps/android-device-test/p2pdc_echo" \
            "$root/.deps/android-device-test/libc++_shared.so"; do
    if [[ ! -f "$file" ]]; then echo "Missing test file: $file" >&2; exit 1; fi
done
work_dir=$(mktemp -d /tmp/p2pdc-internet.XXXXXX)
remote_dir='' mobile_pid='' local_pid=''
cleanup() {
    exit_code=$?
    trap - EXIT
    for pid in "$local_pid" "$mobile_pid"; do
        if [[ -n "$pid" ]]; then kill -- "-$pid" 2>/dev/null || true; fi
    done
    if [[ -n "$remote_dir" ]]; then
        if ! timeout 10 "$adb_command" -s "$serial" shell "if [ -f '$remote_dir/pid' ]; then p=\$(cat '$remote_dir/pid'); case \"\$p\" in ''|*[!0-9]*) exit 1;; esac; if [ -r /proc/\"\$p\"/cmdline ] && tr '\\0' ' ' 2>/dev/null < /proc/\"\$p\"/cmdline | grep -Fq '$remote_dir/p2pdc_echo'; then kill \"\$p\"; fi; fi; rm -rf '$remote_dir'"; then
            echo "Could not clean phone test directory: $remote_dir" >&2; exit_code=1
        fi
    fi
    if (( exit_code != 0 )); then
        for log_name in local mobile; do
            if [[ -f "$work_dir/$log_name.log" ]]; then tail -25 "$work_dir/$log_name.log" >&2; fi
        done
    fi
    rm -rf -- "$work_dir"
    exit "$exit_code"
}
trap cleanup EXIT
if [[ -n ${P2PDC_TEST_BROWSER_PROXY:-} ]]; then
    mkdir -p "$work_dir/browser-config" "$work_dir/browser-cache"
    runner_env+=("XDG_CONFIG_HOME=$work_dir/browser-config" "XDG_CACHE_HOME=$work_dir/browser-cache")
fi
remote_dir=$("$adb_command" -s "$serial" shell 'mktemp -d /data/local/tmp/p2pdc-test.XXXXXX' | tr -d '\r')
if [[ ! "$remote_dir" =~ ^/data/local/tmp/p2pdc-test\.[a-zA-Z0-9]+$ ]]; then
    remote_dir=; echo 'Device did not create a safe temporary test directory.' >&2; exit 1
fi
for file in p2pdc_echo libc++_shared.so; do
    "$adb_command" -s "$serial" push "$root/.deps/android-device-test/$file" "$remote_dir/" >/dev/null
done
"$adb_command" -s "$serial" shell "chmod 700 '$remote_dir/p2pdc_echo'"
seed_local=$(openssl rand -hex 32); seed_mobile=$(openssl rand -hex 32)
secret_local=$(openssl rand -hex 32); secret_mobile=$(openssl rand -hex 32)
capability=$(openssl rand -hex 32)
key_local=$(P2PDC_ECHO_SEED="$seed_local" "$root/build-native/p2pdc_echo" --public-key)
key_mobile=$(P2PDC_ECHO_SEED="$seed_mobile" "$root/build-native/p2pdc_echo" --public-key)
remote_command="echo \$\$ > '$remote_dir/pid'; exec env -i PATH=/system/bin:/system/xbin LD_LIBRARY_PATH='$remote_dir' P2PDC_ECHO_ROLE='$mobile_role' P2PDC_ECHO_SEED='$seed_mobile' P2PDC_ECHO_SECRET='$secret_mobile' P2PDC_ECHO_PEER_KEY='$key_local' P2PDC_ECHO_CAPABILITY='$capability' P2PDC_ECHO_NETWORK=0 P2PDC_ECHO_BOOTSTRAP_HOST=bootstrap.jami.net P2PDC_ECHO_BOOTSTRAP_PORT=4222 P2PDC_ECHO_STUN_HOST=stun.l.google.com P2PDC_ECHO_STUN_PORT=19302 $remote_turn timeout 70 '$remote_dir/p2pdc_echo'"
setsid "$adb_command" -s "$serial" shell "$remote_command" >"$work_dir/mobile.log" 2>&1 &
mobile_pid=$!
env -i PATH="$PATH" P2PDC_ECHO_ROLE="$local_role" P2PDC_ECHO_SEED="$seed_local" \
    P2PDC_ECHO_SECRET="$secret_local" P2PDC_ECHO_PEER_KEY="$key_mobile" P2PDC_ECHO_CAPABILITY="$capability" \
    P2PDC_ECHO_NETWORK=0 P2PDC_ECHO_BOOTSTRAP_HOST=bootstrap.jami.net P2PDC_ECHO_BOOTSTRAP_PORT=4222 \
    P2PDC_ECHO_STUN_HOST=stun.l.google.com P2PDC_ECHO_STUN_PORT=19302 "${turn_env[@]}" "${runner_env[@]}" \
    setsid timeout 70 "${native_command[@]}" >"$work_dir/local.log" 2>&1 &
local_pid=$!
wait "$local_pid"; local_pid=
wait "$mobile_pid"; mobile_pid=
if [[ "$local_role" == initiator ]]; then initiator=local; responder=mobile; else initiator=mobile; responder=local; fi
grep -q '^echo received$' "$work_dir/$initiator.log"
grep -q '^echo acknowledged; waiting for peer$' "$work_dir/$responder.log"
if [[ ${3:-direct} != direct && ${3:-direct} != blocked-direct ]]; then
    for log_name in local mobile; do
        exchange_route=$(awk '/^echo received$|^echo acknowledged; waiting for peer$/ { exit } /^route:/ { route=$0 } END { print route }' "$work_dir/$log_name.log")
        if { [[ "$log_name" == local && "$local_policy" == required ]] || \
             [[ "$log_name" == mobile && ( "$mobile_policy" == required || ${3:-} == phone-fallback ) ]]; } && \
           [[ "$exchange_route" != 'route: TurnRelayed (TURN used) [local=Relayed,'* ]]; then
            echo "Local relay not observed by $log_name at exchange" >&2; exit 1
        fi
    done
fi
if [[ -n ${P2PDC_TEST_BROWSER_PROXY:-} ]]; then
    echo 'Browser and Android exchanged an internet echo'
else
    echo 'Linux and Android exchanged an internet echo'
fi
for log_name in local mobile; do
    while IFS= read -r line; do printf '%s %s\n' "$log_name" "$line"; done < <(grep '^route:' "$work_dir/$log_name.log" || true)
done
if [[ ${P2PDC_TEST_ICE_TRACE:-0} == 1 ]]; then
    grep '^p2pdc-ice:' "$work_dir/local.log" || true
fi

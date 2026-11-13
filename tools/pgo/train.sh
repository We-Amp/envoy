#!/usr/bin/env bash
#
# Collects a PGO training profile from an instrumented Envoy (built with --config=pgo-instrument).
#
# Runs a fixed traffic mix against the instrumented Envoy, one leg at a time, and writes one
# .profraw file per leg. A second, plain Envoy acts as the origin, so nothing else is needed
# besides nighthawk_client, curl, openssl and python3. Linux and bash 4.4 or newer only.
# See tools/pgo/README.md.

set -euo pipefail

if ((BASH_VERSINFO[0] < 4 || (BASH_VERSINFO[0] == 4 && BASH_VERSINFO[1] < 4))); then
  echo "ERROR: bash 4.4 or newer is required" >&2
  exit 1
fi

ALL_LEGS="h1-s,h1-fh,h1-m,h1-l,h2-s,h2-fh,h3-s,h3-fh,xds-s"

usage() {
  cat <<EOF
Usage: $0 --envoy PATH --nighthawk PATH --out DIR [options]

  --envoy PATH         instrumented envoy-static (built with --config=pgo-instrument)
  --nighthawk PATH     nighthawk_client binary
  --out DIR            output directory; must not hold profiles from an earlier run
  --origin-envoy PATH  Envoy binary for the origin (default: the --envoy binary). A normal,
                       non-instrumented build is preferred; its profile is never merged.
  --duration SECONDS   length of each leg (default: 60)
  --legs LIST          comma-separated subset of legs (default: all)
                       ${ALL_LEGS}

Environment (optional):
  CONCURRENCY   Envoy worker threads and nighthawk workers (default: 4)
  RPS           requests per second per nighthawk worker, open loop (default: 500)
  CONNECTIONS   connections per nighthawk worker (default: 4)
  CHURN_HZ      xDS route + cluster updates per second in the xds leg (default: 2)
  ENVOY_CPUS, ORIGIN_CPUS, CLIENT_CPUS
                CPU lists for taskset, e.g. "0-3". Unset means no pinning.
  HTTP_PORT, H3_PORT, ORIGIN_PORT
                loopback listener ports (defaults: 19080, 19443 (UDP), 19081). They must be
                free. Admin interfaces always use a port picked by the kernel.
EOF
}

die() {
  echo "ERROR: $*" >&2
  exit 1
}

# A browser-like request header set, sent by the client on every leg.
REQUEST_HEADERS=(
  "user-agent: Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36"
  "accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,*/*;q=0.8"
  "accept-language: en-US,en;q=0.9,de;q=0.8,fr;q=0.7"
  "accept-encoding: gzip, deflate, br"
  "cache-control: max-age=0"
  "referer: https://www.example.com/catalog/items?page=2&sort=price"
  "cookie: session=6f1c0d2e9b8a4c3f8e7d6c5b4a392817; prefs=lang%3Den%26theme%3Ddark; _ga=GA1.2.1234567890.1700000000"
  "sec-ch-ua: \"Chromium\";v=\"124\", \"Not-A.Brand\";v=\"99\""
  "sec-ch-ua-mobile: ?0"
  "sec-ch-ua-platform: \"Linux\""
  "sec-fetch-dest: document"
  "sec-fetch-mode: navigate"
  "sec-fetch-site: same-origin"
  "sec-fetch-user: ?1"
  "upgrade-insecure-requests: 1"
  "dnt: 1"
  "priority: u=0, i"
  "x-forwarded-for: 198.51.100.23"
  "x-forwarded-proto: https"
  "x-request-id: 0b6c1f0e-4d1a-4f61-9a43-2d6f7b9c8e15"
)

# The helper scripts sit next to this one, or in the runfiles tree under `bazel run`.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
for dir in "${SCRIPT_DIR}" "${RUNFILES_DIR:-${BASH_SOURCE[0]}.runfiles}/_main/tools/pgo" \
  "${PWD}/tools/pgo"; do
  if [[ -f "${dir}/gen_config.py" ]]; then
    SCRIPT_DIR="$(cd "${dir}" && pwd)"
    break
  fi
done
# Under `bazel run`, resolve relative paths against the directory bazel was started from.
if [[ -n "${BUILD_WORKING_DIRECTORY:-}" ]]; then
  cd "${BUILD_WORKING_DIRECTORY}"
fi

ENVOY=""
NIGHTHAWK=""
ORIGIN_ENVOY=""
OUT=""
DURATION=60
LEGS="${ALL_LEGS}"

# need_value FLAG ARGS...: fails unless FLAG is followed by a non-empty value.
need_value() {
  if [[ $# -lt 2 || -z "$2" ]]; then
    echo "ERROR: $1 needs a value" >&2
    usage >&2
    exit 2
  fi
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --envoy) need_value "$@"; ENVOY="$2"; shift 2 ;;
    --nighthawk) need_value "$@"; NIGHTHAWK="$2"; shift 2 ;;
    --origin-envoy) need_value "$@"; ORIGIN_ENVOY="$2"; shift 2 ;;
    --out) need_value "$@"; OUT="$2"; shift 2 ;;
    --duration) need_value "$@"; DURATION="$2"; shift 2 ;;
    --legs) need_value "$@"; LEGS="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "ERROR: unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

CONCURRENCY="${CONCURRENCY:-4}"
RPS="${RPS:-500}"
CONNECTIONS="${CONNECTIONS:-4}"
CHURN_HZ="${CHURN_HZ:-2}"
HTTP_PORT="${HTTP_PORT:-19080}"
H3_PORT="${H3_PORT:-19443}"
ORIGIN_PORT="${ORIGIN_PORT:-19081}"

[[ -n "${ENVOY}" && -x "${ENVOY}" ]] || { usage >&2; die "--envoy must name an executable"; }
[[ -n "${NIGHTHAWK}" && -x "${NIGHTHAWK}" ]] || die "--nighthawk must name an executable"
[[ -n "${OUT}" ]] || die "--out is required"
ORIGIN_ENVOY="${ORIGIN_ENVOY:-${ENVOY}}"
[[ -x "${ORIGIN_ENVOY}" ]] || die "--origin-envoy must name an executable"
for tool in curl openssl python3; do
  command -v "${tool}" >/dev/null || die "${tool} not found"
done

for var in DURATION CONCURRENCY RPS CONNECTIONS; do
  [[ "${!var}" =~ ^[1-9][0-9]*$ ]] || die "${var} must be a positive integer, got '${!var}'"
done
for var in HTTP_PORT H3_PORT ORIGIN_PORT; do
  [[ "${!var}" =~ ^[1-9][0-9]*$ && "${!var}" -le 65535 ]] ||
    die "${var} must be a port number, got '${!var}'"
done
[[ "${CHURN_HZ}" =~ ^[0-9]*\.?[0-9]+$ && ! "${CHURN_HZ}" =~ ^[0.]+$ ]] ||
  die "CHURN_HZ must be a positive number, got '${CHURN_HZ}'"

IFS=',' read -r -a SELECTED <<<"${LEGS}"
[[ ${#SELECTED[@]} -gt 0 ]] || die "--legs is empty"
for leg in "${SELECTED[@]}"; do
  [[ ",${ALL_LEGS}," == *",${leg},"* ]] || die "unknown leg '${leg}' (known: ${ALL_LEGS})"
done

# Fails if something already uses the loopback port. TCP is probed with a connect; UDP needs
# `ss` and is otherwise left to Envoy's own bind failure, which the liveness check catches.
check_port_free() {
  local proto="$1" port="$2" in_use=""
  if [[ "${proto}" == tcp ]]; then
    if python3 -c 'import socket, sys; s = socket.socket(); s.settimeout(1); sys.exit(s.connect_ex(("127.0.0.1", int(sys.argv[1]))) != 0)' "${port}"; then
      in_use=1
    fi
  elif command -v ss >/dev/null && [[ -n "$(ss -H -lun "sport = :${port}")" ]]; then
    in_use=1
  fi
  [[ -z "${in_use}" ]] ||
    die "${proto} port ${port} is already in use; stop that process or pick another port"
}
check_port_free tcp "${HTTP_PORT}"
check_port_free tcp "${ORIGIN_PORT}"
check_port_free udp "${H3_PORT}"

[[ -f "${SCRIPT_DIR}/gen_config.py" ]] ||
  die "gen_config.py not found (looked next to train.sh and in the runfiles tree)"
mkdir -p "${OUT}"
OUT="$(cd "${OUT}" && pwd)"
PROFILE_DIR="${OUT}/profiles"
# Each leg writes its profile to a staging directory of its own. It is moved to PROFILE_DIR only
# when the leg passes its checks, so PROFILE_DIR never holds the profile of a failed leg.
STAGING_DIR="${OUT}/staging"
LOG_DIR="${OUT}/logs"
TLS_DIR="${OUT}/tls"
if compgen -G "${PROFILE_DIR}/*.profraw" >/dev/null; then
  die "${PROFILE_DIR} already holds .profraw files; use a fresh --out directory"
fi
rm -rf "${STAGING_DIR}"
mkdir -p "${PROFILE_DIR}" "${STAGING_DIR}" "${LOG_DIR}" "${OUT}/origin-profile"

# Only processes this script started are ever signalled.
STARTED_PIDS=()

is_running() {
  jobs -pr | grep -qx "$1"
}

# Stops PID only while it is still one of this shell's own running background jobs: SIGTERM,
# then SIGKILL if it has not exited after 30 seconds.
stop_pid() {
  local pid="$1" i
  if is_running "${pid}"; then
    kill "${pid}" 2>/dev/null || true
    for ((i = 0; i < 60; i++)); do
      is_running "${pid}" || break
      sleep 0.5
    done
    if is_running "${pid}"; then
      echo "WARNING: process ${pid} did not exit after SIGTERM; sending SIGKILL" >&2
      kill -KILL "${pid}" 2>/dev/null || true
    fi
  fi
  wait "${pid}" 2>/dev/null || true
}

cleanup() {
  local pid
  for pid in "${STARTED_PIDS[@]}"; do
    stop_pid "${pid}"
  done
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

forget_pid() {
  local pid="$1" kept=() p
  for p in "${STARTED_PIDS[@]}"; do
    [[ "${p}" == "${pid}" ]] || kept+=("${p}")
  done
  STARTED_PIDS=("${kept[@]}")
}

# Command prefixes for CPU pinning. taskset execs the command, so $! stays the process itself.
pin_prefix() {
  if [[ -n "$1" ]]; then
    echo "taskset -c $1"
  fi
}
read -r -a ENVOY_PIN <<<"$(pin_prefix "${ENVOY_CPUS:-}")"
read -r -a ORIGIN_PIN <<<"$(pin_prefix "${ORIGIN_CPUS:-}")"
read -r -a CLIENT_PIN <<<"$(pin_prefix "${CLIENT_CPUS:-}")"

# Every Envoy binds its admin interface to a kernel-picked port and writes the address to a file
# of its own (--admin-address-path). The admin address is only ever read from that file, so
# readiness checks, stats and /quitquitquit can never reach a process this script did not start.
ADMIN_ADDRESS=""

# wait_ready PID ADDRESS_FILE: waits until PID wrote its admin address and reports ready, then
# makes sure PID is still alive (a listener that failed to bind makes Envoy exit).
wait_ready() {
  local pid="$1" address_file="$2" i
  ADMIN_ADDRESS=""
  for ((i = 0; i < 120; i++)); do
    is_running "${pid}" || return 1
    if [[ -z "${ADMIN_ADDRESS}" && -s "${address_file}" ]]; then
      ADMIN_ADDRESS="$(<"${address_file}")"
    fi
    if [[ -n "${ADMIN_ADDRESS}" ]] &&
      curl -sf --max-time 5 "http://${ADMIN_ADDRESS}/ready" >/dev/null; then
      sleep 1
      is_running "${pid}" || return 1
      return 0
    fi
    sleep 0.5
  done
  return 1
}

stat_value() {
  local address="$1" name="$2"
  curl -sf --max-time 5 --get "http://${address}/stats" --data-urlencode "filter=^${name}\$" |
    awk -F': ' -v n="${name}" '$1 == n { print $2 }'
}

# Graceful stop through the admin API of our own process, so the instrumented binary writes its
# profile at exit.
quit_envoy() {
  local address="$1" pid="$2" i
  if is_running "${pid}"; then
    curl -sf --max-time 5 -X POST "http://${address}/quitquitquit" >/dev/null || true
  fi
  for ((i = 0; i < 120; i++)); do
    is_running "${pid}" || break
    sleep 0.5
  done
  stop_pid "${pid}"
  forget_pid "${pid}"
}

echo "== generating a throwaway self-signed certificate for the HTTP/3 legs"
mkdir -p "${TLS_DIR}"
chmod 700 "${TLS_DIR}"
openssl ecparam -name prime256v1 -genkey -noout -out "${TLS_DIR}/server.key" ||
  die "openssl could not generate the HTTP/3 key"
openssl req -new -x509 -key "${TLS_DIR}/server.key" -out "${TLS_DIR}/server.crt" -days 2 \
  -subj "/CN=localhost" -addext "subjectAltName=DNS:localhost,IP:127.0.0.1" ||
  die "openssl could not generate the HTTP/3 certificate (OpenSSL 1.1.1 or newer is needed)"

echo "== writing Envoy configurations to ${OUT}"
python3 "${SCRIPT_DIR}/gen_config.py" --out "${OUT}" --tls-dir "${TLS_DIR}" \
  --http-port "${HTTP_PORT}" --h3-port "${H3_PORT}" --origin-port "${ORIGIN_PORT}"
H3_TRANSPORT_SOCKET="$(<"${OUT}/nighthawk-h3-transport-socket.json")"

echo "== starting the origin"
ORIGIN_ADDRESS_FILE="${LOG_DIR}/origin.admin"
rm -f "${ORIGIN_ADDRESS_FILE}"
LLVM_PROFILE_FILE="${OUT}/origin-profile/origin-%p.profraw" \
  "${ORIGIN_PIN[@]}" "${ORIGIN_ENVOY}" -c "${OUT}/origin.json" \
  --admin-address-path "${ORIGIN_ADDRESS_FILE}" \
  --concurrency "${CONCURRENCY}" --disable-hot-restart -l error \
  >"${LOG_DIR}/origin.log" 2>&1 &
ORIGIN_PID=$!
STARTED_PIDS+=("${ORIGIN_PID}")
wait_ready "${ORIGIN_PID}" "${ORIGIN_ADDRESS_FILE}" || {
  tail -20 "${LOG_DIR}/origin.log" >&2
  die "origin did not become ready"
}
ORIGIN_ADMIN="${ADMIN_ADDRESS}"
for path in /s /m /l; do
  curl -sf --max-time 5 -o /dev/null "http://127.0.0.1:${ORIGIN_PORT}${path}" ||
    die "origin does not serve ${path}"
done

SUMMARY="${OUT}/legs.txt"
: >"${SUMMARY}"
EXPECTED=$((RPS * CONCURRENCY * DURATION))

# fail_leg LEG MESSAGE: stops the run. The leg's profile stays in its staging directory.
fail_leg() {
  die "$2; its profile was not added to ${PROFILE_DIR} (kept in ${STAGING_DIR}/$1)"
}

run_leg() {
  local leg="$1" config url churn_pid="" envoy_pid address_file admin header staging
  local -a client_args=()
  case "${leg}" in
    h1-s | h1-fh | h1-m | h1-l)
      config=proxy.json
      url="http://127.0.0.1:${HTTP_PORT}/${leg#h1-}"
      check_port_free tcp "${HTTP_PORT}"
      ;;
    h2-s | h2-fh)
      config=proxy.json
      url="http://127.0.0.1:${HTTP_PORT}/${leg#h2-}"
      client_args=(--protocol http2)
      check_port_free tcp "${HTTP_PORT}"
      ;;
    h3-s | h3-fh)
      config=proxy-h3.json
      url="https://127.0.0.1:${H3_PORT}/${leg#h3-}"
      client_args=(--protocol http3 --transport-socket "${H3_TRANSPORT_SOCKET}")
      check_port_free udp "${H3_PORT}"
      ;;
    xds-s)
      config=proxy-xds.json
      url="http://127.0.0.1:${HTTP_PORT}/s"
      check_port_free tcp "${HTTP_PORT}"
      python3 "${SCRIPT_DIR}/xds_churn.py" "${OUT}/xds" 0
      ;;
    *) die "unknown leg ${leg} (known: ${ALL_LEGS})" ;;
  esac
  for header in "${REQUEST_HEADERS[@]}"; do
    client_args+=(--request-header "${header}")
  done

  echo "== leg ${leg}: ${DURATION}s, ${CONCURRENCY} workers x ${RPS} rps, ${url}"
  staging="${STAGING_DIR}/${leg}"
  mkdir -p "${staging}"
  address_file="${LOG_DIR}/envoy-${leg}.admin"
  rm -f "${address_file}"
  LLVM_PROFILE_FILE="${staging}/envoy-%p-%m.profraw" \
    "${ENVOY_PIN[@]}" "${ENVOY}" -c "${OUT}/${config}" \
    --admin-address-path "${address_file}" \
    --concurrency "${CONCURRENCY}" --disable-hot-restart -l error \
    >"${LOG_DIR}/envoy-${leg}.log" 2>&1 &
  envoy_pid=$!
  STARTED_PIDS+=("${envoy_pid}")
  wait_ready "${envoy_pid}" "${address_file}" || {
    tail -20 "${LOG_DIR}/envoy-${leg}.log" >&2
    fail_leg "${leg}" "instrumented Envoy did not become ready for leg ${leg}"
  }
  admin="${ADMIN_ADDRESS}"

  if [[ "${leg}" == xds-s ]]; then
    python3 "${SCRIPT_DIR}/xds_churn.py" "${OUT}/xds" "${CHURN_HZ}" &
    churn_pid=$!
    STARTED_PIDS+=("${churn_pid}")
  fi

  "${CLIENT_PIN[@]}" "${NIGHTHAWK}" --rps "${RPS}" --connections "${CONNECTIONS}" \
    --concurrency "${CONCURRENCY}" --open-loop --duration "${DURATION}" \
    "${client_args[@]}" "${url}" >"${LOG_DIR}/nighthawk-${leg}.log" 2>&1 &
  # Waiting on a background job keeps the script responsive to INT/TERM during the leg.
  local client_pid=$! client_status=0
  STARTED_PIDS+=("${client_pid}")
  wait "${client_pid}" || client_status=$?
  forget_pid "${client_pid}"
  [[ "${client_status}" -eq 0 ]] ||
    fail_leg "${leg}" "nighthawk failed in leg ${leg}; see ${LOG_DIR}/nighthawk-${leg}.log"

  local extra=""
  if [[ -n "${churn_pid}" ]]; then
    stop_pid "${churn_pid}"
    forget_pid "${churn_pid}"
    extra=" rds_reloads=$(stat_value "${admin}" http.ingress_http.rds.local.update_success)"
    extra+=" cds_reloads=$(stat_value "${admin}" cluster_manager.cds.update_success)"
  fi
  local ok completed
  ok="$(stat_value "${admin}" http.ingress_http.downstream_rq_2xx)"
  completed="$(stat_value "${admin}" http.ingress_http.downstream_rq_completed)"
  ok="${ok:-0}"
  completed="${completed:-0}"
  quit_envoy "${admin}" "${envoy_pid}"
  echo "${leg} rq_2xx=${ok} rq_completed=${completed} expected=${EXPECTED}${extra}" |
    tee -a "${SUMMARY}"
  [[ $((completed - ok)) -eq 0 ]] ||
    fail_leg "${leg}" "leg ${leg} had $((completed - ok)) non-2xx responses"
  [[ $((ok * 10)) -ge $((EXPECTED * 9)) ]] ||
    fail_leg "${leg}" "leg ${leg} served ${ok} of ${EXPECTED} expected requests (below 90%)"

  compgen -G "${staging}/*.profraw" >/dev/null ||
    fail_leg "${leg}" "leg ${leg} wrote no .profraw file; is --envoy instrumented?"
  mv "${staging}"/*.profraw "${PROFILE_DIR}/"
  rmdir "${staging}"
}

for leg in "${SELECTED[@]}"; do
  run_leg "${leg}"
done

quit_envoy "${ORIGIN_ADMIN}" "${ORIGIN_PID}"
rmdir "${STAGING_DIR}" 2>/dev/null || true

PROFILES=("${PROFILE_DIR}"/*.profraw)
[[ -e "${PROFILES[0]}" ]] || die "no .profraw files were written; is --envoy instrumented?"
echo "== done: ${#PROFILES[@]} profile files in ${PROFILE_DIR}"
echo "Merge them with: tools/pgo/merge.sh ${PROFILE_DIR} /absolute/path/envoy.profdata"

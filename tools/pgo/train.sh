#!/usr/bin/env bash
#
# Collects a PGO training profile from an instrumented Envoy (built with --config=pgo-instrument).
#
# Runs a fixed traffic mix against the instrumented Envoy, one leg at a time, and writes one
# .profraw file per leg. A second, plain Envoy acts as the origin, so nothing else is needed
# besides nighthawk_client, curl, openssl and python3. See tools/pgo/README.md.

set -euo pipefail

usage() {
  cat <<EOF
Usage: $0 --envoy PATH --nighthawk PATH [options]

  --envoy PATH         instrumented envoy-static (built with --config=pgo-instrument)
  --nighthawk PATH     nighthawk_client binary
  --origin-envoy PATH  Envoy binary for the origin (default: the --envoy binary). A normal,
                       non-instrumented build is preferred; its profile is never merged.
  --out DIR            output directory (default: ./pgo-train); must not hold old profiles
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

ALL_LEGS="h1-s,h1-fh,h1-m,h1-l,h2-s,h2-fh,h3-s,h3-fh,xds-s"

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

ENVOY=""
NIGHTHAWK=""
ORIGIN_ENVOY=""
OUT="pgo-train"
DURATION=60
LEGS="${ALL_LEGS}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --envoy) ENVOY="$2"; shift 2 ;;
    --nighthawk) NIGHTHAWK="$2"; shift 2 ;;
    --origin-envoy) ORIGIN_ENVOY="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    --duration) DURATION="$2"; shift 2 ;;
    --legs) LEGS="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

CONCURRENCY="${CONCURRENCY:-4}"
RPS="${RPS:-500}"
CONNECTIONS="${CONNECTIONS:-4}"
CHURN_HZ="${CHURN_HZ:-2}"
HTTP_PORT="${HTTP_PORT:-19080}"
H3_PORT="${H3_PORT:-19443}"
ORIGIN_PORT="${ORIGIN_PORT:-19081}"

die() {
  echo "ERROR: $*" >&2
  exit 1
}

[[ -n "${ENVOY}" && -x "${ENVOY}" ]] || { usage >&2; die "--envoy must name an executable"; }
[[ -n "${NIGHTHAWK}" && -x "${NIGHTHAWK}" ]] || die "--nighthawk must name an executable"
ORIGIN_ENVOY="${ORIGIN_ENVOY:-${ENVOY}}"
[[ -x "${ORIGIN_ENVOY}" ]] || die "--origin-envoy must name an executable"
for tool in curl openssl python3; do
  command -v "${tool}" >/dev/null || die "${tool} not found"
done

# Fails if something already uses the loopback port. TCP is probed with a connect; UDP needs
# `ss` and is otherwise left to Envoy's own bind failure, which the liveness check catches.
check_port_free() {
  local proto="$1" port="$2" in_use=""
  if [[ "${proto}" == tcp ]]; then
    if (exec 3<>"/dev/tcp/127.0.0.1/${port}") 2>/dev/null; then
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

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
mkdir -p "${OUT}"
OUT="$(cd "${OUT}" && pwd)"
PROFILE_DIR="${OUT}/profiles"
LOG_DIR="${OUT}/logs"
TLS_DIR="${OUT}/tls"
if compgen -G "${PROFILE_DIR}/*.profraw" >/dev/null; then
  die "${PROFILE_DIR} already holds .profraw files; use a fresh --out directory"
fi
mkdir -p "${PROFILE_DIR}" "${LOG_DIR}" "${OUT}/origin-profile"

# Only processes this script started are ever signalled.
STARTED_PIDS=()

# Signals PID only while it is still one of this shell's own running background jobs.
stop_pid() {
  local pid="$1"
  if jobs -pr | grep -qx "${pid}"; then
    kill "${pid}" 2>/dev/null || true
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
trap 'exit 130' INT TERM

forget_pid() {
  local pid="$1" kept=() p
  for p in "${STARTED_PIDS[@]}"; do
    [[ "${p}" == "${pid}" ]] || kept+=("${p}")
  done
  STARTED_PIDS=("${kept[@]}")
}

is_running() {
  jobs -pr | grep -qx "$1"
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
    if [[ -n "${ADMIN_ADDRESS}" ]] && curl -sf "http://${ADMIN_ADDRESS}/ready" >/dev/null; then
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
  curl -sf --get "http://${address}/stats" --data-urlencode "filter=^${name}\$" |
    awk -F': ' -v n="${name}" '$1 == n { print $2 }'
}

# Graceful stop through the admin API of our own process, so the instrumented binary writes its
# profile at exit.
quit_envoy() {
  local address="$1" pid="$2" i
  if is_running "${pid}"; then
    curl -sf -X POST "http://${address}/quitquitquit" >/dev/null || true
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
openssl ecparam -name prime256v1 -genkey -noout -out "${TLS_DIR}/server.key" 2>/dev/null
openssl req -new -x509 -key "${TLS_DIR}/server.key" -out "${TLS_DIR}/server.crt" -days 2 \
  -subj "/CN=localhost" -addext "subjectAltName=DNS:localhost,IP:127.0.0.1" 2>/dev/null
H3_TRANSPORT_SOCKET="$(printf '{"name":"envoy.transport_sockets.quic","typed_config":{"@type":"type.googleapis.com/envoy.extensions.transport_sockets.quic.v3.QuicUpstreamTransport","upstream_tls_context":{"sni":"localhost","common_tls_context":{"validation_context":{"trusted_ca":{"filename":"%s"}}}}}}' "${TLS_DIR}/server.crt")"

echo "== writing Envoy configurations to ${OUT}"
python3 "${SCRIPT_DIR}/gen_config.py" --out "${OUT}" --tls-dir "${TLS_DIR}" \
  --http-port "${HTTP_PORT}" --h3-port "${H3_PORT}" --origin-port "${ORIGIN_PORT}"

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
  curl -sf -o /dev/null "http://127.0.0.1:${ORIGIN_PORT}${path}" || die "origin does not serve ${path}"
done

SUMMARY="${OUT}/legs.txt"
: >"${SUMMARY}"
EXPECTED=$((RPS * CONCURRENCY * DURATION))

run_leg() {
  local leg="$1" config url churn_pid="" envoy_pid address_file admin header
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
  address_file="${LOG_DIR}/envoy-${leg}.admin"
  rm -f "${address_file}"
  LLVM_PROFILE_FILE="${PROFILE_DIR}/envoy-%p-%m.profraw" \
    "${ENVOY_PIN[@]}" "${ENVOY}" -c "${OUT}/${config}" \
    --admin-address-path "${address_file}" \
    --concurrency "${CONCURRENCY}" --disable-hot-restart -l error \
    >"${LOG_DIR}/envoy-${leg}.log" 2>&1 &
  envoy_pid=$!
  STARTED_PIDS+=("${envoy_pid}")
  wait_ready "${envoy_pid}" "${address_file}" || {
    tail -20 "${LOG_DIR}/envoy-${leg}.log" >&2
    die "instrumented Envoy did not become ready for leg ${leg}"
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
    die "nighthawk failed in leg ${leg}; see ${LOG_DIR}/nighthawk-${leg}.log"

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
  [[ $((completed - ok)) -eq 0 ]] || die "leg ${leg} had $((completed - ok)) non-2xx responses"
  [[ $((ok * 10)) -ge $((EXPECTED * 9)) ]] ||
    die "leg ${leg} served ${ok} of ${EXPECTED} expected requests (below 90%)"
}

IFS=',' read -r -a SELECTED <<<"${LEGS}"
for leg in "${SELECTED[@]}"; do
  run_leg "${leg}"
done

quit_envoy "${ORIGIN_ADMIN}" "${ORIGIN_PID}"

PROFILES=("${PROFILE_DIR}"/*.profraw)
[[ -e "${PROFILES[0]}" ]] || die "no .profraw files were written; is --envoy instrumented?"
echo "== done: ${#PROFILES[@]} profile files in ${PROFILE_DIR}"
echo "Merge them with: tools/pgo/merge.sh ${PROFILE_DIR} /absolute/path/envoy.profdata"

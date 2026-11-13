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
  ADMIN_PORT, HTTP_PORT, H3_PORT, ORIGIN_PORT, ORIGIN_ADMIN_PORT
                loopback ports (defaults: 19901, 19080, 19443, 19081, 19902)
EOF
}

ALL_LEGS="h1-s,h1-fh,h1-m,h1-l,h2-s,h2-fh,h3-s,h3-fh,xds-s"

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
ADMIN_PORT="${ADMIN_PORT:-19901}"
HTTP_PORT="${HTTP_PORT:-19080}"
H3_PORT="${H3_PORT:-19443}"
ORIGIN_PORT="${ORIGIN_PORT:-19081}"
ORIGIN_ADMIN_PORT="${ORIGIN_ADMIN_PORT:-19902}"

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

# Command prefixes for CPU pinning. taskset execs the command, so $! stays the process itself.
pin_prefix() {
  if [[ -n "$1" ]]; then
    echo "taskset -c $1"
  fi
}
read -r -a ENVOY_PIN <<<"$(pin_prefix "${ENVOY_CPUS:-}")"
read -r -a ORIGIN_PIN <<<"$(pin_prefix "${ORIGIN_CPUS:-}")"
read -r -a CLIENT_PIN <<<"$(pin_prefix "${CLIENT_CPUS:-}")"

wait_ready() {
  local port="$1" pid="$2" i
  for ((i = 0; i < 120; i++)); do
    kill -0 "${pid}" 2>/dev/null || return 1
    curl -sf "http://127.0.0.1:${port}/ready" >/dev/null && return 0
    sleep 0.5
  done
  return 1
}

stat_value() {
  local port="$1" name="$2"
  curl -sf --get "http://127.0.0.1:${port}/stats" --data-urlencode "filter=^${name}\$" |
    awk -F': ' -v n="${name}" '$1 == n { print $2 }'
}

# Graceful stop through the admin API, so the instrumented binary writes its profile at exit.
quit_envoy() {
  local port="$1" pid="$2" i
  curl -sf -X POST "http://127.0.0.1:${port}/quitquitquit" >/dev/null || true
  for ((i = 0; i < 120; i++)); do
    jobs -pr | grep -qx "${pid}" || break
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
  --admin-port "${ADMIN_PORT}" --http-port "${HTTP_PORT}" --h3-port "${H3_PORT}" \
  --origin-port "${ORIGIN_PORT}" --origin-admin-port "${ORIGIN_ADMIN_PORT}"

echo "== starting the origin"
LLVM_PROFILE_FILE="${OUT}/origin-profile/origin-%p.profraw" \
  "${ORIGIN_PIN[@]}" "${ORIGIN_ENVOY}" -c "${OUT}/origin.json" \
  --concurrency "${CONCURRENCY}" --disable-hot-restart -l error \
  >"${LOG_DIR}/origin.log" 2>&1 &
ORIGIN_PID=$!
STARTED_PIDS+=("${ORIGIN_PID}")
wait_ready "${ORIGIN_ADMIN_PORT}" "${ORIGIN_PID}" || {
  tail -20 "${LOG_DIR}/origin.log" >&2
  die "origin did not become ready"
}
for path in /s /m /l; do
  curl -sf -o /dev/null "http://127.0.0.1:${ORIGIN_PORT}${path}" || die "origin does not serve ${path}"
done

SUMMARY="${OUT}/legs.txt"
: >"${SUMMARY}"

run_leg() {
  local leg="$1" config url churn_pid="" envoy_pid
  local -a client_args=()
  case "${leg}" in
    h1-s | h1-fh | h1-m | h1-l)
      config=proxy.json
      url="http://127.0.0.1:${HTTP_PORT}/${leg#h1-}"
      ;;
    h2-s | h2-fh)
      config=proxy.json
      url="http://127.0.0.1:${HTTP_PORT}/${leg#h2-}"
      client_args=(-p http2)
      ;;
    h3-s | h3-fh)
      config=proxy-h3.json
      url="https://127.0.0.1:${H3_PORT}/${leg#h3-}"
      client_args=(-p http3 --transport-socket "${H3_TRANSPORT_SOCKET}")
      ;;
    xds-s)
      config=proxy-xds.json
      url="http://127.0.0.1:${HTTP_PORT}/s"
      python3 "${SCRIPT_DIR}/xds_churn.py" "${OUT}/xds" 0
      ;;
    *) die "unknown leg ${leg} (known: ${ALL_LEGS})" ;;
  esac

  echo "== leg ${leg}: ${DURATION}s, ${CONCURRENCY} workers x ${RPS} rps, ${url}"
  LLVM_PROFILE_FILE="${PROFILE_DIR}/envoy-%p-%m.profraw" \
    "${ENVOY_PIN[@]}" "${ENVOY}" -c "${OUT}/${config}" \
    --concurrency "${CONCURRENCY}" --disable-hot-restart -l error \
    >"${LOG_DIR}/envoy-${leg}.log" 2>&1 &
  envoy_pid=$!
  STARTED_PIDS+=("${envoy_pid}")
  wait_ready "${ADMIN_PORT}" "${envoy_pid}" || {
    tail -20 "${LOG_DIR}/envoy-${leg}.log" >&2
    die "instrumented Envoy did not become ready for leg ${leg}"
  }

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
    extra=" rds_updates=$(stat_value "${ADMIN_PORT}" http.ingress_http.rds.local.update_success)"
    extra+=" cds_updates=$(stat_value "${ADMIN_PORT}" cluster_manager.cds.update_success)"
  fi
  local ok completed
  ok="$(stat_value "${ADMIN_PORT}" http.ingress_http.downstream_rq_2xx)"
  completed="$(stat_value "${ADMIN_PORT}" http.ingress_http.downstream_rq_completed)"
  quit_envoy "${ADMIN_PORT}" "${envoy_pid}"
  echo "${leg} rq_2xx=${ok:-0} rq_completed=${completed:-0}${extra}" | tee -a "${SUMMARY}"
  [[ "${ok:-0}" -gt 0 ]] || die "leg ${leg} served no successful requests"
}

IFS=',' read -r -a SELECTED <<<"${LEGS}"
for leg in "${SELECTED[@]}"; do
  run_leg "${leg}"
done

quit_envoy "${ORIGIN_ADMIN_PORT}" "${ORIGIN_PID}"

PROFILES=("${PROFILE_DIR}"/*.profraw)
[[ -e "${PROFILES[0]}" ]] || die "no .profraw files were written; is --envoy instrumented?"
echo "== done: ${#PROFILES[@]} profile files in ${PROFILE_DIR}"
echo "Merge them with: tools/pgo/merge.sh ${PROFILE_DIR} /absolute/path/envoy.profdata"

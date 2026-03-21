#!/usr/bin/env bash
#
# run_experiments.sh - Comprehensive experiment runner for mutex benchmark research
#
# Usage:
#   ./run_experiments.sh <mode> [options]
#
# Modes:
#   dram        - Standard DRAM (no extra flags)
#   cached_sc   - Cached store-conditional (-Dcached_sc -mclflushopt), SW locks only
#   hcxl        - Hardware CXL (-Dhardware_cxl -lnuma)
#   uc          - Uncacheable CXL (-Duc_cxl), SW locks only
#   all         - Run all modes sequentially
#
# Options:
#   --quick     Use quick parameters (5s max / 0.2s min, 3 iterations, fewer thread counts)
#   --bench     Which benchmark to run: max, min, or both (default: both)
#
# Examples:
#   ./run_experiments.sh dram
#   ./run_experiments.sh dram --quick --bench max
#   ./run_experiments.sh all --quick
#

set -euo pipefail

# ============================================================================
# Configuration
# ============================================================================

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DATA_DIR="${PROJECT_DIR}/data/generated"
LOG_DIR="${PROJECT_DIR}/data/logs"

# Build directories are per-mode to avoid shared library conflicts
# Set dynamically in configure_and_build()
BUILD_DIR=""
MAX_BENCH=""
MIN_BENCH=""

# The 13 paper locks (full set, used for dram and hcxl modes)
PAPER_LOCKS="bakery boulangerie lamport linear_lamport_elevator linear_bl_elevator tree_lamport_elevator tree_bl_elevator peterson knuth spin exp_spin ticket mcs"

# Software-only locks (used for uc and cached_sc modes -- no hardware atomics)
SW_LOCKS="bakery boulangerie lamport linear_lamport_elevator linear_bl_elevator tree_lamport_elevator tree_bl_elevator peterson knuth"

# Defaults: paper parameters
MAX_DURATION=20
MIN_DURATION=0.5
ITERATIONS=5
THREAD_SWEEP="1 5 9 13 17 21 25 29 33 37 41 45 49 53 57 61"

# Benchmark selection
BENCH_MODE="both"

# Tracking
TOTAL_RUNS=0
SUCCESSFUL_RUNS=0
FAILED_RUNS=0
SKIPPED_LOCKS=""
TIMEOUT_SECONDS=60

# ============================================================================
# Argument parsing
# ============================================================================

usage() {
    echo "Usage: $0 <mode> [--quick] [--bench max|min|both]"
    echo ""
    echo "Modes: dram, cached_sc, hcxl, uc, all"
    echo ""
    echo "Options:"
    echo "  --quick    Use quick parameters (5s/3iter for fast results)"
    echo "  --bench    Which benchmark: max, min, or both (default: both)"
    exit 1
}

if [[ $# -lt 1 ]]; then
    usage
fi

MODE="$1"
shift

# Validate mode
case "${MODE}" in
    dram|cached_sc|hcxl|uc|all) ;;
    *) echo "ERROR: Unknown mode '${MODE}'"; usage ;;
esac

# Parse optional arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        --quick)
            MAX_DURATION=5
            MIN_DURATION=0.2
            ITERATIONS=3
            THREAD_SWEEP="1 8 16 24 32 48 64"
            shift
            ;;
        --bench)
            if [[ $# -lt 2 ]]; then
                echo "ERROR: --bench requires an argument (max, min, or both)"
                exit 1
            fi
            BENCH_MODE="$2"
            case "${BENCH_MODE}" in
                max|min|both) ;;
                *) echo "ERROR: --bench must be max, min, or both"; exit 1 ;;
            esac
            shift 2
            ;;
        *)
            echo "ERROR: Unknown option '$1'"
            usage
            ;;
    esac
done

# ============================================================================
# Helper functions
# ============================================================================

timestamp() {
    date "+%Y-%m-%d %H:%M:%S"
}

log_info() {
    echo "[$(timestamp)] INFO: $*"
}

log_warn() {
    echo "[$(timestamp)] WARN: $*" >&2
}

log_error() {
    echo "[$(timestamp)] ERROR: $*" >&2
}

separator() {
    echo "============================================================================"
}

# Get the lock list for a given mode
get_locks_for_mode() {
    local mode="$1"
    case "${mode}" in
        dram|hcxl)
            echo "${PAPER_LOCKS}"
            ;;
        cached_sc|uc)
            echo "${SW_LOCKS}"
            ;;
    esac
}

# ============================================================================
# Build functions
# ============================================================================

configure_and_build() {
    local mode="$1"

    # Use per-mode build directories to avoid shared library conflicts
    BUILD_DIR="${PROJECT_DIR}/build_${mode}"
    MAX_BENCH="${BUILD_DIR}/apps/max_contention_bench/max_contention_bench"
    MIN_BENCH="${BUILD_DIR}/apps/min_contention_bench/min_contention_bench"
    export LD_LIBRARY_PATH="${BUILD_DIR}/lib/utils"

    separator
    log_info "Configuring build for mode: ${mode} (build dir: build_${mode})"
    separator

    # Base cpp_args always present
    local cpp_args="'-mwaitpkg','-std=c++20'"

    case "${mode}" in
        dram)
            # No extra flags
            ;;
        cached_sc)
            cpp_args="'-Dcached_sc','-mclflushopt',${cpp_args}"
            ;;
        hcxl)
            cpp_args="'-Dhardware_cxl','-lnuma',${cpp_args}"
            ;;
        uc)
            cpp_args="'-Duc_cxl',${cpp_args}"
            ;;
    esac

    log_info "cpp_args: [${cpp_args}]"

    # Setup build directory if it doesn't exist
    if [[ ! -d "${BUILD_DIR}" ]]; then
        if ! meson setup "${BUILD_DIR}"; then
            log_error "meson setup failed for mode ${mode}"
            return 1
        fi
    fi

    # Configure
    if ! meson configure "${BUILD_DIR}" --optimization 3 "-Dcpp_args=[${cpp_args}]"; then
        log_error "meson configure failed for mode ${mode}"
        return 1
    fi

    # Rebuild
    log_info "Compiling..."
    if ! meson compile -C "${BUILD_DIR}"; then
        log_error "meson compile failed for mode ${mode}"
        return 1
    fi

    log_info "Build successful for mode: ${mode}"
    return 0
}

# ============================================================================
# Benchmark runners
# ============================================================================

run_max_contention() {
    local mode="$1"
    local locks
    locks=$(get_locks_for_mode "${mode}")

    separator
    log_info "Running MAX contention benchmark (mode=${mode}, duration=${MAX_DURATION}s, iterations=${ITERATIONS})"
    separator

    for lock in ${locks}; do
        for n in ${THREAD_SWEEP}; do
            for iter in $(seq 1 "${ITERATIONS}"); do
                TOTAL_RUNS=$((TOTAL_RUNS + 1))
                local outfile="${DATA_DIR}/${lock}-${iter}-max-${mode}-threads=${n}.csv"
                log_info "  [max] lock=${lock}  threads=${n}  iter=${iter}/${ITERATIONS}"

                if timeout "${TIMEOUT_SECONDS}" \
                    "${MAX_BENCH}" "${lock}" "${n}" "${MAX_DURATION}" \
                    --thread-level --csv \
                    > "${outfile}" 2>/dev/null; then
                    SUCCESSFUL_RUNS=$((SUCCESSFUL_RUNS + 1))
                else
                    local exit_code=$?
                    FAILED_RUNS=$((FAILED_RUNS + 1))
                    if [[ ${exit_code} -eq 124 ]]; then
                        log_warn "  TIMEOUT: ${lock} threads=${n} iter=${iter} (killed after ${TIMEOUT_SECONDS}s)"
                    else
                        log_warn "  FAILED:  ${lock} threads=${n} iter=${iter} (exit code ${exit_code})"
                    fi
                    # Remove partial output
                    rm -f "${outfile}"
                    # Record skipped lock (unique)
                    if [[ ! " ${SKIPPED_LOCKS} " =~ " ${lock} " ]]; then
                        SKIPPED_LOCKS="${SKIPPED_LOCKS} ${lock}"
                    fi
                fi
            done
        done
    done
}

run_min_contention() {
    local mode="$1"
    local locks
    locks=$(get_locks_for_mode "${mode}")

    separator
    log_info "Running MIN contention benchmark (mode=${mode}, duration=${MIN_DURATION}s, iterations=${ITERATIONS})"
    separator

    for lock in ${locks}; do
        for n in ${THREAD_SWEEP}; do
            for iter in $(seq 1 "${ITERATIONS}"); do
                TOTAL_RUNS=$((TOTAL_RUNS + 1))
                local outfile="${DATA_DIR}/${lock}-${iter}-min-${mode}-threads=${n}.csv"
                log_info "  [min] lock=${lock}  threads=${n}  iter=${iter}/${ITERATIONS}"

                if timeout "${TIMEOUT_SECONDS}" \
                    "${MIN_BENCH}" "${lock}" "${n}" "${MIN_DURATION}" \
                    --csv \
                    > "${outfile}" 2>/dev/null; then
                    SUCCESSFUL_RUNS=$((SUCCESSFUL_RUNS + 1))
                else
                    local exit_code=$?
                    FAILED_RUNS=$((FAILED_RUNS + 1))
                    if [[ ${exit_code} -eq 124 ]]; then
                        log_warn "  TIMEOUT: ${lock} threads=${n} iter=${iter} (killed after ${TIMEOUT_SECONDS}s)"
                    else
                        log_warn "  FAILED:  ${lock} threads=${n} iter=${iter} (exit code ${exit_code})"
                    fi
                    rm -f "${outfile}"
                    if [[ ! " ${SKIPPED_LOCKS} " =~ " ${lock} " ]]; then
                        SKIPPED_LOCKS="${SKIPPED_LOCKS} ${lock}"
                    fi
                fi
            done
        done
    done
}

# ============================================================================
# Run benchmarks for a single mode
# ============================================================================

run_mode() {
    local mode="$1"
    local locks
    locks=$(get_locks_for_mode "${mode}")
    local lock_count
    lock_count=$(echo "${locks}" | wc -w)
    local thread_count
    thread_count=$(echo "${THREAD_SWEEP}" | wc -w)

    separator
    log_info "MODE: ${mode}"
    log_info "Locks (${lock_count}): ${locks}"
    log_info "Thread sweep (${thread_count} points): ${THREAD_SWEEP}"
    log_info "Iterations: ${ITERATIONS}"
    log_info "Bench: ${BENCH_MODE}"
    separator

    # Configure and build
    if ! configure_and_build "${mode}"; then
        log_error "Build failed for mode ${mode}, skipping."
        return 1
    fi

    # Run selected benchmarks
    if [[ "${BENCH_MODE}" == "max" || "${BENCH_MODE}" == "both" ]]; then
        run_max_contention "${mode}"
    fi

    if [[ "${BENCH_MODE}" == "min" || "${BENCH_MODE}" == "both" ]]; then
        run_min_contention "${mode}"
    fi
}

# ============================================================================
# Main
# ============================================================================

main() {
    separator
    log_info "Mutex Benchmark Experiment Runner"
    separator
    log_info "Mode:           ${MODE}"
    log_info "Bench:          ${BENCH_MODE}"
    log_info "Max duration:   ${MAX_DURATION}s"
    log_info "Min duration:   ${MIN_DURATION}s"
    log_info "Iterations:     ${ITERATIONS}"
    log_info "Thread sweep:   ${THREAD_SWEEP}"
    log_info "Timeout:        ${TIMEOUT_SECONDS}s per run"
    log_info "Project dir:    ${PROJECT_DIR}"
    log_info "Data dir:       ${DATA_DIR}"
    separator

    # Create output directories
    mkdir -p "${DATA_DIR}" "${LOG_DIR}"

    local start_time
    start_time=$(date +%s)

    if [[ "${MODE}" == "all" ]]; then
        for m in dram cached_sc hcxl uc; do
            run_mode "${m}" || true
        done
    else
        run_mode "${MODE}"
    fi

    local end_time
    end_time=$(date +%s)
    local elapsed=$((end_time - start_time))
    local elapsed_min=$((elapsed / 60))
    local elapsed_sec=$((elapsed % 60))

    # Print summary
    echo ""
    separator
    log_info "EXPERIMENT SUMMARY"
    separator
    log_info "Mode:             ${MODE}"
    log_info "Bench:            ${BENCH_MODE}"
    log_info "Total runs:       ${TOTAL_RUNS}"
    log_info "Successful:       ${SUCCESSFUL_RUNS}"
    log_info "Failed:           ${FAILED_RUNS}"
    if [[ -n "${SKIPPED_LOCKS}" ]]; then
        log_info "Locks with failures:${SKIPPED_LOCKS}"
    else
        log_info "Locks with failures: (none)"
    fi
    log_info "Elapsed time:     ${elapsed_min}m ${elapsed_sec}s"
    log_info "Output directory: ${DATA_DIR}"
    separator

    if [[ ${FAILED_RUNS} -gt 0 ]]; then
        log_warn "${FAILED_RUNS} runs failed. Check warnings above for details."
    fi
}

main

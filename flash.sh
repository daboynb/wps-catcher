#!/usr/bin/env bash
# Auto-detect ESP32 chip + port, then erase + flash firmware + flash SPIFFS + monitor.
# Usage: ./flash.sh [esp32|esp32s3|esp32c3] [/dev/ttyUSB0]
#   No args -> auto-detect everything.

set -euo pipefail

cd "$(dirname "$0")"

VALID_ENVS=(esp32 esp32s3 esp32c3)

ENV="${1:-}"
PORT="${2:-}"

# --- esptool resolver ---
resolve_esptool() {
    if command -v esptool.py >/dev/null 2>&1; then
        echo "esptool.py"; return
    fi
    if python3 -c "import esptool" >/dev/null 2>&1; then
        echo "python3 -m esptool"; return
    fi
    local pio_tool="$HOME/.platformio/packages/tool-esptoolpy/esptool.py"
    if [[ -f "$pio_tool" ]]; then
        echo "python3 $pio_tool"; return
    fi
    return 1
}

# --- port auto-detect ---
detect_port() {
    local p
    for p in /dev/ttyUSB* /dev/ttyACM*; do
        [[ -e "$p" ]] && { echo "$p"; return 0; }
    done
    return 1
}

# --- chip -> env mapping ---
chip_to_env() {
    case "$1" in
        *ESP32-S3*) echo "esp32s3" ;;
        *ESP32-C3*) echo "esp32c3" ;;
        *ESP32-S2*|*ESP32-C6*|*ESP32-H2*)
            echo "UNSUPPORTED:$1" ;;
        *ESP32*)    echo "esp32" ;;
        *)          echo "UNKNOWN:$1" ;;
    esac
}

# Prefer official PlatformIO Core venv over distro/apt package
# (apt's platformio 4.x is incompatible with modern click and crashes on `resultcallback`).
PIO=""
if [[ -x "$HOME/.platformio/penv/bin/pio" ]]; then
    PIO="$HOME/.platformio/penv/bin/pio"
elif command -v pio >/dev/null 2>&1; then
    PIO="$(command -v pio)"
else
    echo "pio not found. Install PlatformIO Core: https://platformio.org/install/cli" >&2
    exit 1
fi
echo "==> Using pio: $PIO ($("$PIO" --version 2>/dev/null))"

# Resolve port
if [[ -z "$PORT" ]]; then
    PORT="$(detect_port)" || {
        echo "No /dev/ttyUSB* or /dev/ttyACM* found. Plug board or pass port as 2nd arg." >&2
        exit 1
    }
    echo "==> Detected port: $PORT"
fi

# Resolve env
if [[ -z "$ENV" ]]; then
    ESPTOOL_CMD="$(resolve_esptool)" || {
        echo "esptool not found (need esptool.py, python3 -m esptool, or PlatformIO install)." >&2
        exit 1
    }
    echo "==> Probing chip on $PORT ..."
    CHIP_OUT="$($ESPTOOL_CMD --port "$PORT" chip_id 2>&1 || true)"
    CHIP_LINE="$(echo "$CHIP_OUT" | grep -m1 '^Chip is' || true)"
    if [[ -z "$CHIP_LINE" ]]; then
        echo "Chip detect failed. esptool output:" >&2
        echo "$CHIP_OUT" >&2
        exit 1
    fi
    ENV="$(chip_to_env "$CHIP_LINE")"
    case "$ENV" in
        UNSUPPORTED:*)
            echo "Detected ${ENV#UNSUPPORTED:} — no platformio env for this chip." >&2
            exit 1 ;;
        UNKNOWN:*)
            echo "Could not parse chip: ${ENV#UNKNOWN:}" >&2
            exit 1 ;;
    esac
    echo "==> Detected: $CHIP_LINE -> env=$ENV"
fi

if [[ ! " ${VALID_ENVS[*]} " =~ " ${ENV} " ]]; then
    echo "Invalid env: $ENV (use: ${VALID_ENVS[*]})" >&2
    exit 1
fi

PORT_ARG=(--upload-port "$PORT")

echo "==> Erasing flash"
"$PIO" run -e "$ENV" -t erase "${PORT_ARG[@]}"

echo "==> Flashing firmware"
"$PIO" run -e "$ENV" -t upload "${PORT_ARG[@]}"

echo "==> Flashing SPIFFS (web UI)"
"$PIO" run -e "$ENV" -t uploadfs "${PORT_ARG[@]}"

echo "==> Opening serial monitor (Ctrl+] to exit)"
"$PIO" device monitor -e "$ENV" --port "$PORT"

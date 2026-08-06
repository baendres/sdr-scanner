#!/usr/bin/env bash
# Measures an RTL-SDR's crystal frequency error (PPM) against a NOAA Weather Radio broadcast,
# for entry into the scanner's Settings -> Receivers -> PPM Correction field (see
# ReceiverConfig::ppmCorrection in ../src/config/Types.h and native/README.md's DMR/P25 section).
#
# Headless, no GUI/waterfall needed - wraps the `rtl_power` CLI (part of the same rtl-sdr apt
# package the Dockerfile already installs, so this runs unmodified inside the built image).
# NOAA Weather Radio stations run a continuous, always-on FM carrier on one of 7 fixed channels
# and are audible almost everywhere in the US - a much more available reference than GSM
# (decommissioned in the US) for kalibrate-rtl's usual method, and close in frequency to typical
# VHF DMR repeaters, so any tuner nonlinearity across the band is a non-issue here.
#
# Method: rtl_power reports a time-averaged power spectrum. A short wideband pass across all 7
# NOAA channels finds whichever is actually receivable at this location; a longer, fine-resolution
# pass on just that channel locates the carrier precisely. The measured peak's offset from the
# channel's nominal frequency, as a fraction of that frequency, IS the ppm value to enter -
# librtlsdr's rtlsdr_set_freq_correction() (which SoapyReceiver.cpp's set_frequency_correction()
# calls through to) takes the crystal's actual measured error directly, not its negation.
#
# The RTL-SDR must not be in use by anything else while this runs (stop the running scanner
# container first - see usage below).
#
# Usage:
#   ./tools/measure_ppm_noaa.sh [-d device_index] [-g gain_db] [-f freq_hz] [-t seconds]
#
#   -d  rtl_power device index (default 0). Run once per dongle if you have more than one -
#       `rtl_test -t` or `SoapySDRUtil --find` lists index-to-serial mappings.
#   -g  manual tuner gain in dB (default 40 - decent NOAA reception without AGC drift muddying
#       the measurement). 0 = auto/AGC.
#   -f  skip the discovery scan and use this NOAA channel frequency directly, in Hz
#       (one of 162400000 162425000 162450000 162475000 162500000 162525000 162550000).
#   -t  fine-pass integration time in seconds (default 25 - long enough to average out FM
#       modulation swings into a stable peak).
#
# Typical use (inside the built image, via docker compose - see native/README.md):
#   docker compose stop sdr_scanner_native
#   docker compose run --rm sdr_scanner_native ./tools/measure_ppm_noaa.sh -d 0
#   docker compose run --rm sdr_scanner_native ./tools/measure_ppm_noaa.sh -d 1
#   docker compose up -d

set -euo pipefail

device=0
gain=40
freq=""
fine_seconds=25

while getopts "d:g:f:t:h" opt; do
    case "$opt" in
        d) device="$OPTARG" ;;
        g) gain="$OPTARG" ;;
        f) freq="$OPTARG" ;;
        t) fine_seconds="$OPTARG" ;;
        h)
            sed -n '2,40p' "$0" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *) exit 1 ;;
    esac
done

if ! command -v rtl_power >/dev/null 2>&1; then
    echo "error: rtl_power not found (part of the rtl-sdr package - already installed in this" >&2
    echo "project's Docker image; run this script inside that container, not on the bare host)" >&2
    exit 1
fi

# rtl_power only enables automatic/AGC gain when -g is omitted entirely - passing "-g 0" sets
# gain to a literal 0dB, not auto. -g "$gain" below is empty (and so skipped) when gain=0.
gain_args=()
if [ "$gain" != "0" ]; then
    gain_args=(-g "$gain")
fi

noaa_channels=(162400000 162425000 162450000 162475000 162500000 162525000 162550000)

workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT

# Picks the strongest power bin from an rtl_power CSV file (single integration row expected -
# ends up with exactly one line of actual data since -1/-e make it exit after one interval).
# Prints "freq_hz db" for the loudest bin.
strongest_bin() {
    local csv="$1"
    awk -F', *' '
        NF < 7 { next }
        {
            hz_low = $3; hz_step = $5; nbins = NF - 6;
            for (i = 0; i < nbins; i++) {
                db = $(7 + i);
                if (db > best_db || NR == 1 && i == 0) { best_db = db; best_hz = hz_low + (i + 0.5) * hz_step; }
            }
        }
        END { printf "%.1f %.2f\n", best_hz, best_db }
    ' "$csv"
}

if [ -z "$freq" ]; then
    echo "No -f given - scanning all 7 NOAA channels to find the strongest one at this location..." >&2
    scan_csv="$workdir/scan.csv"
    # Span covers 162.395-162.555MHz (all 7 channels + margin) at 1kHz bins - coarse, just to
    # find which channel is actually receivable here. -1 exits after one integration.
    rtl_power -f 162395000:162555000:1000 -i 8 -1 "${gain_args[@]}" -d "$device" "$scan_csv" >/dev/null 2>&1

    read -r loudest_hz loudest_db < <(strongest_bin "$scan_csv")
    # Snap the loudest bin to the nearest actual NOAA channel (rtl_power's bin center won't land
    # exactly on a nominal channel frequency).
    best_channel=""
    best_delta=999999999
    for ch in "${noaa_channels[@]}"; do
        delta_abs=$(awk -v a="$loudest_hz" -v b="$ch" 'BEGIN { d = a - b; if (d < 0) d = -d; print d }')
        is_closer=$(awk -v d="$delta_abs" -v best="$best_delta" 'BEGIN { print (d < best) ? 1 : 0 }')
        if [ "$is_closer" = "1" ]; then best_delta="$delta_abs"; best_channel="$ch"; fi
    done
    freq="$best_channel"
    echo "Strongest signal near $(awk -v h="$loudest_hz" 'BEGIN{printf "%.4f", h/1e6}') MHz (~${loudest_db} dB) -> nearest NOAA channel $(awk -v f="$freq" 'BEGIN{printf "%.3f", f/1e6}') MHz" >&2
fi

gain_desc="${gain}dB"; [ "$gain" = "0" ] && gain_desc="auto"
echo "Measuring precise carrier offset on $(awk -v f="$freq" 'BEGIN{printf "%.3f", f/1e6}') MHz over ${fine_seconds}s (device $device, gain ${gain_desc})..." >&2
fine_lo=$((freq - 4000))
fine_hi=$((freq + 4000))
fine_csv="$workdir/fine.csv"
rtl_power -f "${fine_lo}:${fine_hi}:100" -i "$fine_seconds" -1 "${gain_args[@]}" -d "$device" "$fine_csv" >/dev/null 2>&1

read -r measured_hz measured_db < <(strongest_bin "$fine_csv")

awk -v measured="$measured_hz" -v nominal="$freq" -v db="$measured_db" '
BEGIN {
    offset = measured - nominal;
    ppm = (offset / nominal) * 1e6;
    printf "\nNominal channel frequency: %.3f MHz\n", nominal / 1e6;
    printf "Measured carrier peak:     %.4f MHz (%.1f dB)\n", measured / 1e6, db;
    printf "Offset:                    %+.1f Hz\n", offset;
    printf "PPM correction to enter:   %+.2f\n\n", ppm;
    print "Enter this value in Settings -> Receivers -> PPM Correction for this device, restart";
    print "sdrscan, then re-run this script on the same frequency - the offset should now be";
    print "close to 0 Hz. If it roughly doubled instead, the sign was wrong; enter the negative.";
}'

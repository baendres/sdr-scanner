#!/usr/bin/env bash
# Captures rtl_fm's raw FM-discriminator output directly from an RTL-SDR at a given frequency -
# bypassing this project's own C4FM chain (ChannelBlockDMR.cpp) and DSDcc entirely - and prints an
# ASCII amplitude histogram of the result. A real, cleanly-received C4FM/4FSK signal (DMR's outer/
# inner symbol levels: +/-1944Hz and +/-648Hz deviation - see kDmrPeakDeviationHz in
# ChannelBlockDMR.cpp) shows up as roughly 4 distinct clustered peaks in the discriminator output;
# noise, a mistuned frequency, or a badly filtered signal does not.
#
# This directly answers the open question in native/README.md's DMR investigation: whether the RF
# signal itself is clean at all, independent of any bug/limitation in this project's own front end
# or in DSDcc's sync engine - "capturing raw IQ during a confirmed transmission and inspecting/
# decoding it offline with a known-good tool independent of this codebase". rtl_fm's own
# discriminator (a different, independently-implemented quadrature demod than
# gr::analog::quadrature_demod_cf) is exactly that independent tool - no GUI needed, output is a
# small enough text summary to read directly in a terminal or paste elsewhere.
#
# The RTL-SDR must not be in use by anything else while this runs (stop the running scanner
# container first - see usage below, same as tools/measure_ppm_noaa.sh).
#
# Usage:
#   ./tools/capture_dmr_histogram.sh -f freq_hz [-d device_index] [-g gain_db] [-p ppm] [-t seconds] [-b bins] [-r lo:hi]
#
#   -f  frequency to capture, in Hz (required) - e.g. 146955000 for the DMR repeater under test.
#       Capture must span an actual transmission (key up before/during the run) to mean anything.
#   -d  rtl_power/rtl_fm device index (default 0).
#   -g  manual tuner gain in dB (default 40). 0 = auto/AGC.
#   -p  PPM correction to apply during capture (default 0) - match whatever's configured in
#       Settings for this receiver (see tools/measure_ppm_noaa.sh) for a representative capture.
#   -t  capture duration in seconds (default 10).
#   -b  number of histogram buckets (default 64).
#   -r  lo:hi amplitude range to histogram, instead of auto-scaling to the full captured
#       min/max. Use this for a second, zoomed-in pass once a first run's full-range histogram
#       shows where the interesting activity actually sits - spreading a fixed bin count across
#       the full range (which can include a wide, low-density noise floor way out toward
#       +/-32767) leaves too few bins covering a narrow real signal to resolve its internal
#       structure (e.g. DMR's 4 symbol levels), even when it's genuinely a clean 4FSK signal.
#
# Typical use (inside the built image, via docker compose - see native/README.md):
#   docker compose stop sdr_scanner_native
#   docker compose run --rm sdr_scanner_native ./tools/capture_dmr_histogram.sh -f 146955000 -d 0 -p 18.67
#   docker compose up -d
#   (key up the transmission during the capture window)

set -euo pipefail

device=0
gain=40
ppm=0
seconds=10
bins=64
freq=""
range=""

while getopts "f:d:g:p:t:b:r:h" opt; do
    case "$opt" in
        f) freq="$OPTARG" ;;
        d) device="$OPTARG" ;;
        g) gain="$OPTARG" ;;
        p) ppm="$OPTARG" ;;
        t) seconds="$OPTARG" ;;
        b) bins="$OPTARG" ;;
        r) range="$OPTARG" ;;
        h)
            sed -n '2,42p' "$0" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *) exit 1 ;;
    esac
done

if [ -z "$freq" ]; then
    echo "error: -f freq_hz is required" >&2
    exit 1
fi

if ! command -v rtl_fm >/dev/null 2>&1; then
    echo "error: rtl_fm not found (part of the rtl-sdr package - already installed in this" >&2
    echo "project's Docker image; run this script inside that container, not on the bare host)" >&2
    exit 1
fi

gain_args=()
if [ "$gain" != "0" ]; then
    gain_args=(-g "$gain")
fi

workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT
capture="$workdir/capture.raw"

echo "Capturing ${seconds}s of raw FM-discriminator output at $(awk -v f="$freq" 'BEGIN{printf "%.4f", f/1e6}') MHz (device $device, gain ${gain}dB, ppm ${ppm})..." >&2
echo "(key up the transmission now if you haven't already)" >&2
# rtl_fm has no built-in duration/exit-timer flag (unlike rtl_power's -e) - timeout bounds it.
# rtl_fm's default -M fm mode outputs a single-channel signed-16-bit discriminator stream, one
# sample per output rate tick - exactly the kind of raw demod signal DsdccDecodeBlock consumes
# (see DsdccDecodeBlock.cpp), just from a completely independent demodulator implementation.
timeout "$((seconds + 2))s" rtl_fm -f "$freq" -M fm -s 48000 -p "$ppm" "${gain_args[@]}" -d "$device" "$capture" >/dev/null 2>&1 || true

nsamples=$(($(stat -c%s "$capture" 2>/dev/null || echo 0) / 2))
if [ "$nsamples" -lt 1000 ]; then
    echo "error: captured only $nsamples samples - rtl_fm likely failed to open the device (still" >&2
    echo "in use by the scanner container?) or exited immediately. Check for rtl_fm errors above." >&2
    exit 1
fi
echo "Captured $nsamples samples." >&2

if [ -n "$range" ]; then
    min="${range%%:*}"
    max="${range##*:}"
    echo
    echo "Histogramming fixed range [$min, $max] (of captured samples, whatever their true extent)"
    echo
else
    read -r min max < <(od -A n -t d2 -w2 -v "$capture" | awk '
        NR == 1 { min = $1; max = $1 }
        { if ($1 < min) min = $1; if ($1 > max) max = $1 }
        END { print min, max }
    ')
    echo
    echo "Discriminator amplitude range: [$min, $max] (of possible [-32768, 32767])"
    echo
fi

od -A n -t d2 -w2 -v "$capture" | awk -v min="$min" -v max="$max" -v nbins="$bins" '
BEGIN {
    range = max - min;
    if (range <= 0) range = 1;
}
{
    if ($1 < min || $1 > max) next; # out-of-range only possible with -r - drop, do not smear into an edge bin
    b = int((($1 - min) / range) * nbins);
    if (b >= nbins) b = nbins - 1;
    if (b < 0) b = 0;
    count[b]++;
    inrange++;
}
END {
    maxcount = 0;
    for (i = 0; i < nbins; i++) if (count[i] > maxcount) maxcount = count[i];
    barwidth = 60;
    for (i = 0; i < nbins; i++) {
        lo = min + (range * i / nbins);
        hi = min + (range * (i + 1) / nbins);
        n = count[i] + 0;
        bar = "";
        nchars = maxcount > 0 ? int((n / maxcount) * barwidth) : 0;
        for (j = 0; j < nchars; j++) bar = bar "#";
        printf "%7.0f .. %7.0f | %7d %s\n", lo, hi, n, bar;
    }
    printf "\n%d of %d captured samples fell inside this range.\n", inrange + 0, NR;
}
'

cat << 'EOF'

Interpretation: a real, cleanly-received C4FM/4FSK signal (DMR's 4 symbol levels) shows up here as
roughly 4 distinct clustered peaks, not one central hump or a flat/uniform spread. If this looks
like a single blob or noise-like spread even during a confirmed transmission, the problem is
upstream of both DSDcc and this project's own C4FM chain - the RF signal isn't being received
cleanly at all at this frequency/tuning (deviation, filter bandwidth, antenna/signal strength,
still-wrong frequency, etc.), independent of anything DsdccDecodeBlock or ChannelBlockDMR do with
it. If 4 peaks ARE visible here, the problem is more likely specific to this project's own C4FM
front end or DSDcc's sync engine, not the raw RF.
EOF

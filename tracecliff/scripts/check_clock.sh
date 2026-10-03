#!/bin/sh
# Check the APU core clock is 1200 MHz: cpufreq first, the registers as fallback.
# Whole-MHz OPPs make cpufreq honest; stock ones report 1199999 on a 600 MHz core.
set -u

cur=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq 2>/dev/null || echo 0)
if [ "$cur" -eq 1200000 ]; then
    echo "core: 1200 MHz (cpufreq)"
    exit 0
fi
echo "cpufreq reports $cur kHz, not a witness without whole-MHz OPPs; reading the registers"

# devmem needs root: ask for it here rather than failing deep in a capture.
if [ "$(id -u)" -eq 0 ]; then SUDO=; else SUDO=sudo; $SUDO -v || exit 1; fi

acpu=$($SUDO busybox devmem 0xFD1A0060 32)   # ACPU_CTRL: SRCSEL[2:0], DIVISOR0[13:8]
apll=$($SUDO busybox devmem 0xFD1A0020 32)   # APLL_CTRL: FBDIV[14:8], DIV2 (bit 16)

srcsel=$(( acpu & 0x7 ))
divisor0=$(( (acpu >> 8) & 0x3F ))
fbdiv=$(( (apll >> 8) & 0x7F ))
div2=$(( (apll >> 16) & 0x1 ))

# 33.333 MHz reference, kept in kHz to stay in integer arithmetic.
pll_khz=$(( 33333 * fbdiv ))
[ "$div2" -eq 1 ] && pll_khz=$(( pll_khz / 2 ))
if [ "$divisor0" -gt 0 ]; then core_khz=$(( pll_khz / divisor0 )); else core_khz=0; fi

echo "ACPU_CTRL=$acpu APLL_CTRL=$apll"
echo "SRCSEL=$srcsel DIVISOR0=$divisor0 FBDIV=$fbdiv DIV2=$div2"
echo "core: $(( core_khz / 1000 )) MHz (registers)"

if [ "$srcsel" -ne 0 ] || [ "$core_khz" -lt 1150000 ]; then
    echo "ABORT: core is not on APLL at ~1200 MHz. Every offered rate scales" >&2
    echo "       with the clock, so the capture would not be comparable to" >&2
    echo "       anything else in the campaign." >&2
    echo "       sudo busybox devmem 0xFD1A0060 w 0x03000100" >&2
    echo "       and do NOT write cpufreq afterwards without whole-MHz OPPs." >&2
    exit 1
fi

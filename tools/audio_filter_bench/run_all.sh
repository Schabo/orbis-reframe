#!/usr/bin/env bash
# Audio noise filters: every candidate on one audio stream of a clip,
# as WAVs to listen to, plus a report scoring each against the raw audio.
#
#   tools/audio_filter_bench/run_all.sh <clip.insv> <stream> <start-s> <dur-s> <outdir> [deep-filter]
#
# deep-filter: the DeepFilterNet CLI (prebuilt release binary,
# github.com/Rikorose/DeepFilterNet/releases, deep-filter-*-x86_64-unknown-linux-musl);
# skipped if not given.
set -euo pipefail

clip=$1 stream=$2 start=$3 dur=$4 out=$5 df=${6:-}
bin=$(dirname "$0")/../../build/audio_filter_bench
mkdir -p "$out"
s=s$stream
report=$out/report_$s.txt

{
    echo "# $clip stream $stream, ${dur}s from ${start}s"
    echo
    echo "## wind (reduce wind, keep engine), strength 0.7"
    "$bin" extract "$clip" "$out" --stream "$stream" --start "$start" --dur "$dur" --strength 0.7
    mv "$out/wind_$s.wav" "$out/wind07_$s.wav"
    echo
    echo "## wind, strength 1.0"
    "$bin" extract "$clip" "$out" --stream "$stream" --start "$start" --dur "$dur" --strength 1.0 |
        sed -n '/^wind filter/,/^between/p'
    mv "$out/wind_$s.wav" "$out/wind10_$s.wav"

    raw=$out/raw_$s.wav
    ffmpeg -nostdin -v error -y -i "$raw" -af "afftdn=nr=20:nf=-50:tn=1" -c:a pcm_f32le "$out/afftdn_$s.wav"
    echo
    echo "## afftdn (FFmpeg, noise tracking, 20 dB)"
    "$bin" analyze "$raw" "$out/afftdn_$s.wav"

    if [[ -n $df ]]; then
        tmpd=$out/df_tmp
        for lim in 100 12; do
            rm -rf "$tmpd"
            "$df" -D -a "$lim" -o "$tmpd" "$raw" >/dev/null 2>&1
            name=$([[ $lim == 100 ]] && echo dfn || echo "dfn$lim")
            mv "$tmpd/raw_$s.wav" "$out/${name}_$s.wav"
            echo
            echo "## DeepFilterNet, attenuation limit $lim dB"
            "$bin" analyze "$raw" "$out/${name}_$s.wav"
        done
        rm -rf "$tmpd"
    fi
} | tee "$report"

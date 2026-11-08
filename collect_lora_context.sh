#!/data/data/com.termux/files/usr/bin/bash

ROOT="$(pwd)"
OUT="$ROOT/lora_context_bundle.txt"
: > "$OUT"

section() {
    printf '\n\n========== %s ==========\n' "$1" >> "$OUT"
}

add_file() {
    local f="$1"
    if [ -f "$f" ]; then
        section "FILE: $f"
        cat "$f" >> "$OUT"
    fi
}

add_matches() {
    local title="$1"
    local pattern="$2"
    local dirs="$3"
    section "$title"
    grep -R -n -E "$pattern" $dirs \
        --include='*.h' --include='*.cpp' \
        --include='*.hpp' --include='*.cc' \
        2>/dev/null | head -250 >> "$OUT"
}

section "PROJECT INFORMATION"
printf 'ROOT=%s\nDATE=%s\n' "$ROOT" "$(date)" >> "$OUT"
git status --short 2>/dev/null >> "$OUT"

section "LORA PLUGIN FILES"
find plugins/lora_support -type f \
    \( -name '*.h' -o -name '*.cpp' -o -name 'CMakeLists.txt' \) \
    -print 2>/dev/null | sort >> "$OUT"

while IFS= read -r f; do
    add_file "$f"
done < <(
    find plugins/lora_support -type f \
    \( -name '*.h' -o -name '*.cpp' -o -name 'CMakeLists.txt' \) \
    2>/dev/null | sort
)

section "FY4 REMAINING LORA FILES"
find plugins/fengyun4_support/LoRa -type f \
    \( -name '*.h' -o -name '*.cpp' \) \
    -print 2>/dev/null | sort >> "$OUT"

while IFS= read -r f; do
    add_file "$f"
done < <(
    find plugins/fengyun4_support/LoRa -type f \
    \( -name '*.h' -o -name '*.cpp' \) \
    2>/dev/null | sort
)

section "PLUGIN REGISTRATION AND CMAKE"
add_file plugins/CMakeLists.txt
add_file plugins/lora_support/main.cpp
add_file plugins/lora_support/CMakeLists.txt
add_file plugins/fengyun4_support/main.cpp
add_file plugins/fengyun4_support/CMakeLists.txt

section "DEMOD BASE INTERFACE"
add_file src-core/modules/demod/module_demod_base.h
add_file src-core/modules/demod/module_demod_base.cpp

section "DSP BLOCK AND STREAM INTERFACES"
add_file src-core/common/dsp/block.h
add_file src-core/common/dsp/buffer.h

section "RATIONAL RESAMPLER"
add_file src-core/common/dsp/resamp/rational_resampler.h
add_file src-core/common/dsp/resamp/rational_resampler.cpp
add_file src-core/common/dsp/resamp/polyphase_bank.h
add_file src-core/common/dsp/resamp/polyphase_bank.cpp

section "STREAM READ WRITE AND STOP API SEARCH"
add_matches "STREAM API REFERENCES" \
    'stopReader|startReader|readBuf|writeBuf|input_stream|output_stream|class stream|struct stream' \
    "src-core/common/dsp src-core/modules"

section "FFT API REFERENCES"
add_matches "FFTW AND FFT API" \
    'fftwf_plan_dft_1d|fftwf_execute|fftwf_malloc|FFTPlan|fft_plan|kiss_fft' \
    "src-core plugins"

section "RESAMPLER USAGE REFERENCES"
add_matches "RESAMPLER USAGE" \
    'RationalResamplerBlock|set_ratio|PolyphaseBank' \
    "src-core plugins"

section "MODULE OUTPUT STREAM REFERENCES"
add_matches "MODULE OUTPUT REFERENCES" \
    'output_stream->write|output_stream->swap|output_stream->writeBuf|output_stream->stopReader|output_stream' \
    "plugins src-core/modules"

section "LOOP AND MODULE STOP REFERENCES"
add_matches "MODULE PROCESS AND STOP REFERENCES" \
    'void .*::process\\(|void .*::stop\\(|BaseDemodModule::start|BaseDemodModule::stop' \
    "plugins"

section "LORA ALGORITHM SOURCE SEARCH"
add_matches "LORA RELATED SOURCE REFERENCES" \
    'LoRaReceiver|chirp_in_place|deinterleave|dewhiten|hamming_decode|calc_sym_num|calc_crc|low_data_rate_optimization' \
    "plugins src-core"

section "COMPILER AND BUILD SETTINGS"
for f in CMakeLists.txt cmake/CompilerOptions.cmake; do
    add_file "$f"
done

section "FILE SIZE SUMMARY"
du -h "$OUT" >> "$OUT"
wc -l "$OUT" >> "$OUT"

echo
echo "完成：$OUT"
echo "大小：$(du -h "$OUT" | cut -f1)"
echo "行数：$(wc -l < "$OUT")"

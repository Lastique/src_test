#!/usr/bin/env bash

declare -a INPUT_FILES=('44kHz_16-bit.wav' '44kHz_32-bit_float.wav' '48kHz_16-bit.wav' '48kHz_32-bit_float.wav' '96kHz_32-bit_float.wav')
declare -a OUTPUT_RATES=(44100 48000 96000)
declare -a RESAMPLERS=('speex-1' 'speex-5' 'speex-10' 'soxr-lq' 'soxr-mq' 'soxr-hq' 'soxr-vhq')

for RESAMPLER in ${RESAMPLERS[@]}; do
	mkdir -p $RESAMPLER

	for OUT_RATE in ${OUTPUT_RATES[@]}; do
		for IN_FILE in ${INPUT_FILES[@]}; do

			OUT_FILE="$RESAMPLER/$(basename $IN_FILE .wav)_to_$OUT_RATE"
			/usr/bin/env time --format "real/user/sys:\t%e/%U/%S" ./src_test "$IN_FILE" "$RESAMPLER" "$OUT_RATE" "$OUT_FILE.wav"
			[[ -f /usr/bin/sox ]] && sox "$OUT_FILE.wav" -n spectrogram -o "$OUT_FILE.png"
			echo
		done
	done
done

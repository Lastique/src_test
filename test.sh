#!/bin/bash

INPUT_FILES="44kHz_16-bit.wav 44kHz_32-bit_float.wav 48kHz_16-bit.wav 48kHz_32-bit_float.wav 96kHz_32-bit_float.wav"

OUTPUT_RATES="44100 48000 96000"
RESAMPLERS="speex-1 speex-5 speex-10 soxr-lq soxr-mq soxr-hq soxr-vhq"

for resampler in $RESAMPLERS
do
	mkdir -p $resampler

	for rate in $OUTPUT_RATES
	do
		for file in $INPUT_FILES
		do
			out_file=`basename $file .wav`_to_${rate}
			/usr/bin/time --format "real/user/sys:\t%e/%U/%S" ./src_test $file $resampler $rate $resampler/$out_file.wav
			/usr/bin/sox $out_file.wav -n spectrogram -o $out_file.png
			echo ""
		done
	done
done


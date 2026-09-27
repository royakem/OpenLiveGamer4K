#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Summarize a captured stereo S16_LE WAV; activity is not a fidelity test."""
import argparse
import array
import json
import math
import sys
import wave


def summarize(path):
    with wave.open(path, 'rb') as wav:
        if (wav.getnchannels(), wav.getsampwidth(), wav.getcomptype()) != (2, 2, 'NONE'):
            raise ValueError('expected stereo, uncompressed 16-bit PCM WAV')
        rate = wav.getframerate()
        frames = 0
        totals = [0, 0]
        squares = [0, 0]
        peaks = [0, 0]
        nonzero = [0, 0]
        clipped = [0, 0]
        while True:
            block = wav.readframes(16384)
            if not block:
                break
            if len(block) % 4:
                raise ValueError('truncated stereo PCM frame')
            values = array.array('h', block)
            if sys.byteorder != 'little':
                values.byteswap()
            frames += len(values) // 2
            for channel in range(2):
                for value in values[channel::2]:
                    totals[channel] += value
                    squares[channel] += value * value
                    peaks[channel] = max(peaks[channel], abs(value))
                    nonzero[channel] += value != 0
                    clipped[channel] += value in (-32768, 32767)
        if not frames:
            raise ValueError('empty capture')
    channels = []
    for channel in range(2):
        rms = math.sqrt(squares[channel] / frames) / 32768
        channels.append(dict(channel=channel, peak=peaks[channel] / 32768,
                             rms=rms, rms_dbfs=20 * math.log10(rms) if rms else None,
                             dc=totals[channel] / frames / 32768,
                             nonzero_samples=nonzero[channel], clipped_samples=clipped[channel]))
    return dict(rate=rate, frames=frames, duration_seconds=frames / rate,
                channels=channels,
                interpretation='Nonzero samples do not establish valid music, channel order, sample rate accuracy or A/V sync.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('wav')
    args = parser.parse_args()
    try:
        print(json.dumps(summarize(args.wav), indent=2))
    except (OSError, ValueError, wave.Error, EOFError) as exc:
        parser.exit(1, f'{exc}\n')


if __name__ == '__main__':
    main()

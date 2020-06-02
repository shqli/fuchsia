// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MEDIA_AUDIO_LIB_TEST_ANALYSIS_H_
#define SRC_MEDIA_AUDIO_LIB_TEST_ANALYSIS_H_

#include <zircon/types.h>

#include "src/media/audio/lib/test/audio_buffer.h"

namespace media::audio::test {

namespace internal {

// Perform a Fast Fourier Transform on the provided data arrays.
//
// On input, real[] and imag[] contain 'buf_size' number of double-float values
// in the time domain (such as audio samples); buf_size must be a power-of-two.
//
// On output, real[] and imag[] contain 'buf_size' number of double-float values
// in frequency domain, but generally used only through buf_size/2 (per Nyquist)
void FFT(double* real, double* imag, uint32_t buf_size);

// Calculate phase in radians for the complex pair. Correctly handles negative
// or zero values: range of return value is [-PI,PI], not just [-PI/2,PI/2].
double GetPhase(double real, double imag);

// Convert provided real-imag (cartesian) data into magn-phase (polar) format.
// This is done with 2 in-buffers 2 two out-buffers -- NOT 2 in-out-buffers.
// TODO(mpuryear): will clients (tests) want this transformed in-place?
void RectangularToPolar(const double* real, const double* imag, uint32_t buf_size, double* magn,
                        double* phase = nullptr);

void RealDFT(const double* reals, uint32_t len, double* r_freq, double* i_freq);

void InverseDFT(double* real, double* imag, uint32_t buf_size, double* real_out);

void InverseFFT(double* real, double* imag, uint32_t buf_size);

}  // namespace internal

// For the given audio buffer, analyze contents and return the magnitude (and phase) at the given
// frequency. Also return magnitude of all other content. Useful for frequency response and
// signal-to-noise. Internally uses an FFT, so slice.NumFrames() must be a power-of-two. The format
// must have channels() == 1.
//
// |freq| is the number of **complete sinusoidal periods** that should perfectly fit into the
// buffer.
template <fuchsia::media::AudioSampleFormat SampleFormat>
void MeasureAudioFreq(AudioBufferSlice<SampleFormat> slice, size_t freq, double* magn_signal,
                      double* magn_other = nullptr, double* phase_signal = nullptr);

}  // namespace media::audio::test

#endif  // SRC_MEDIA_AUDIO_LIB_TEST_ANALYSIS_H_

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fftw3.h>

#include "audiosrc.h"

static float
power_to_dB (float a)
{
	return 10.f * log10f (a);
}

void
analyze (ZeroConvoLV2::SFSource const& sf)
{
	uint32_t const window_size = 8192 * 2;
	uint32_t const data_size   = window_size / 2;

	float* fftInput  = (float*)fftwf_malloc (sizeof (float) * window_size);
	float* fftOutput = (float*)fftwf_malloc (sizeof (float) * window_size);

	float* power_at_bin = (float*)malloc (sizeof (float) * data_size);

	fftwf_plan plan = fftwf_plan_r2r_1d (window_size, fftInput, fftOutput, FFTW_R2HC, FFTW_ESTIMATE);

	memset (power_at_bin, 0, sizeof (float) * data_size);

	uint32_t const n_channels = sf.n_channels ();

	for (uint32_t c = 0; c < n_channels; ++c) {
		memset (fftInput, 0, sizeof (float) * window_size);
		sf.read (fftInput, 0, window_size, c);

		fftwf_execute (plan);

		power_at_bin[0] += fftOutput[0] * fftOutput[0];

		float power;

#define Re (fftOutput[i])
#define Im (fftOutput[window_size - i])
		for (uint32_t i = 1; i < data_size - 1; ++i) {
			power = (Re * Re) + (Im * Im);
			power_at_bin[i] += power;
		}
#undef Re
#undef Im
	}

	if (n_channels > 1) {
		for (uint32_t i = 0; i < data_size - 1; i++) {
			power_at_bin[i] /= (float)n_channels;
		}
	}

	float  pp = 0;
	double ap = 0;

	for (uint32_t i = 8; i < data_size - 1; i++) {
		pp = std::max (pp, power_at_bin[i]);
	}

	for (uint32_t i = 0; i < data_size / 2; i++) {
		ap += power_at_bin[i];
	}
	ap /= (double)data_size / 2;

	//double gain = 1.0 / (pow (ap, .25) * pow (pp, .25));
	double gain = 1.0 / (pow (ap, .3) * pow (pp, .2));
	//double gain = 1.0 / (pow (ap, .35) * pow (pp, .15));

	fprintf (stderr, "Power peak: %.2fdB average: %.2fdB | gain: %f\n", power_to_dB (pp), power_to_dB (ap), gain);
	printf ("<http://gareus.org/oss/lv2/zeroconvolv#gain> \"%f\"^^xsd:float ;\n", gain);

	fftwf_destroy_plan (plan);
	free (power_at_bin);
	fftwf_free (fftOutput);
	fftwf_free (fftInput);
}

int
main (int argc, char** argv)
{
	if (argc != 2) {
		return -1;
	}

	ZeroConvoLV2::FileSource sf (argv[1]);
	fprintf (stderr, "%-24s: ", argv[1]);
	analyze (sf);

	return 0;
}

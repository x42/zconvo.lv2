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
	/* An empircal approach to calculate a gain factor that places all
	 * IRs in the same loudness ballpark.
	 *
	 * Analyze only the first 150-200ms only. This is where the main
	 * enegery (1st, 2nd reflection) is in the IR.
	 * Long reverb tails are ignored here.
	 */
	int const      up          = ceilf (sf.sample_rate () / 48000.f);
	uint32_t const window_size = 8192 * 2 * up;
	uint32_t const data_size   = window_size / 2;
	uint32_t const n_channels  = sf.n_channels ();

	float* fftInput     = (float*)fftwf_malloc (sizeof (float) * window_size);
	float* fftOutput    = (float*)fftwf_malloc (sizeof (float) * window_size);
	float* power_at_bin = (float*)malloc (sizeof (float) * data_size);
	float  peak         = 0;

	memset (power_at_bin, 0, sizeof (float) * data_size);

	fftwf_plan plan = fftwf_plan_r2r_1d (window_size, fftInput, fftOutput, FFTW_R2HC, FFTW_ESTIMATE);

	for (uint32_t c = 0; c < n_channels; ++c) {
		memset (fftInput, 0, sizeof (float) * window_size);
		sf.read (fftInput, 0, window_size, c);

		for (uint32_t i = 0; i < window_size; ++i) {
			if (fabsf (fftInput[i]) > fabsf (peak)) {
				peak = fftInput[i];
			}
		}

		fftwf_execute (plan);

		power_at_bin[0] += fftOutput[0] * fftOutput[0];

#define Re (fftOutput[i])
#define Im (fftOutput[window_size - i])
		for (uint32_t i = 1; i < data_size - 1; ++i) {
			float power = (Re * Re) + (Im * Im);
			power_at_bin[i] += power;
		}
#undef Re
#undef Im
	}

	if (n_channels > 1) {
		for (uint32_t i = 0; i < data_size - 1; ++i) {
			power_at_bin[i] /= (float)n_channels;
		}
	}

	float  pp = 0;
	double ap = 0;

	for (uint32_t i = 8; i < data_size - 1; ++i) {
		pp = std::max (pp, power_at_bin[i]);
	}

	for (uint32_t i = 0; i < data_size / (up * 2); ++i) {
		ap += power_at_bin[i];
	}
	ap /= (double)data_size / (up + 1);

	double gain = 1.0 / (pow (ap, .3) * pow (pp, .2));

	if (peak < 0) {
		gain *= -1;
	}

	fprintf (stderr, "Peak power: %.2fdB LF-average: %.2fdB | gain: %f\n", power_to_dB (pp), power_to_dB (ap), gain);
#if 0
	printf ("<http://gareus.org/oss/lv2/zeroconvolv#gain> \"%f\"^^xsd:float ;\n", gain);
#else
	printf ("            zc:gain \"%f\"^^xsd:float ;\n", gain);
#endif

	fftwf_destroy_plan (plan);
	free (power_at_bin);
	fftwf_free (fftOutput);
	fftwf_free (fftInput);
}

int
main (int argc, char** argv)
{
	if (argc != 2) {
		fprintf (stderr, "Error: Missing parameter.\n");
		fprintf (stderr, "Usage: fftgain <ir-file>\n");
		return -1;
	}

	ZeroConvoLV2::FileSource sf (argv[1]);
	fprintf (stderr, "%-24s: ", argv[1]);
	analyze (sf);

	return 0;
}

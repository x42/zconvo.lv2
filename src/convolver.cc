/*
 * Copyright (C) 2018 Robin Gareus <robin@gareus.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "audiosrc.h"
#include "convolver.h"

using namespace ZeroConvoLV2;

DelayLine::DelayLine ()
	: _buf (0)
	, _written (false)
	, _delay (0)
	, _pos (0)
{
}

DelayLine::~DelayLine ()
{
	free (_buf);
}

void
DelayLine::clear ()
{
	if (!_written || !_buf) {
		return;
	}
	memset (_buf, 0, _delay * sizeof (float));
	_written = false;
}

void
DelayLine::reset (uint32_t delay)
{
	free (_buf);
	_buf = (float*) calloc (1 + delay, sizeof (float));
	_delay = _buf ? delay : 0;
	_pos = 0;
}

void
DelayLine::run (float* buf, uint32_t n_samples)
{
	_written = n_samples > 0;
	for (uint32_t i = 0 ; i < n_samples; ++i) {
		_buf[_pos] = buf[i];
		if (++_pos > _delay) {
			_pos = 0;
		}
		buf[i] = _buf[_pos] ;
	}
}

TimeDomainConvolver::TimeDomainConvolver ()
{
	reset ();
}

void
TimeDomainConvolver::reset ()
{
	memset (_ir, 0, 64 * sizeof (float));
	_enabled = false;
}

void
TimeDomainConvolver::configure (Readable* r, float gain, uint32_t delay)
{
	if (delay >= 64) {
		return;
	}
	uint32_t to_read = std::min ((uint32_t)64, delay);
	uint32_t max_len = r->readable_length ();
	if (delay < max_len) {
		to_read = std::min (to_read, max_len - delay);
	}
	if (to_read == 0) {
		return;
	}

	r->read (&_ir[delay], 0, to_read, 0);

	if (gain != 1.f) {
		for (uint64_t i = delay; i < 64; ++i) {
			_ir[i] *= gain;
		}
	}
}

void
TimeDomainConvolver::run (float* out, float const* in, uint32_t n_samples) const
{
	if (!_enabled) {
		return;
	}
	for (uint32_t i = 0; i < n_samples; ++i) {
		for (uint32_t j = 0; j < n_samples - i; ++j) {
			out[i + j] += in[i] * _ir[j];
		}
	}
}

Convolver::Convolver (
		std::string const& path,
		uint32_t sample_rate,
		int sched_policy,
		int sched_priority,
		IRChannelConfig irc,
		IRSettings irs)
	: _path (path)
	, _irc (irc)
	, _sched_policy (sched_policy)
	, _sched_priority (sched_priority)
	, _ir_settings (irs)
	, _n_samples (0)
	, _max_size (0)
	, _offset (0)
	, _configured (false)
	, _dry (0.f)
	, _wet (1.f)
	, _dry_target (0.f)
	, _wet_target (1.f)
	, _a (2950.f / sample_rate) // ~20Hz for 90%
{
	if (_path.substr (0, 4) == "mem:") {
		_fs = new MemSource ();
	} else {
		_fs = new FileSource (_path);
	}

	if (_fs->readable_length () > 0x1000000 /*2^24*/) {
		delete _fs;
		_fs = 0;
		throw std::runtime_error ("Convolver: IR file too long.");
	}

	for (unsigned int n = 0; n < _fs->n_channels (); ++n) {
		try {
			Readable* r = new ChanWrap (_fs, n);

			if (r->sample_rate () != sample_rate) {
				Readable* sfs = new SrcSource (r, sample_rate);
				_readables.push_back (sfs);
			} else {
				_readables.push_back (r);
			}

		} catch (std::runtime_error& err) {
			throw;
		}
	}

	if (_readables.empty ()) {
		throw std::runtime_error ("Convolver: no usable audio-channels.");
	}
}

Convolver::~Convolver ()
{
	for (std::vector<Readable*>::const_iterator i = _readables.begin (); i != _readables.end (); ++i) {
		delete *i;
	}
	_readables.clear ();
	delete _fs;
}

void
Convolver::reconfigure (uint32_t block_size, bool threaded)
{
	_convproc.stop_process ();
	_convproc.cleanup ();
	_convproc.set_options (0);

	assert (!_readables.empty ());

	uint32_t n_part;

	if (threaded) {
		_n_samples = 64;
		n_part     = Convproc::MAXPART;
	} else {
		uint32_t power_of_two;
		for (power_of_two = 1; 1U << power_of_two < block_size; ++power_of_two) ;
		_n_samples = 1 << power_of_two;
		n_part     = _n_samples;
	}

	_offset   = 0;
	_max_size = _readables[0]->readable_length ();

	int rv = _convproc.configure (
	    /*in*/  n_inputs (),
	    /*out*/ n_outputs (),
	    /*max-convolution length */ _max_size,
	    /*quantum, nominal-buffersize*/ _n_samples,
	    /*Convproc::MINPART*/ _n_samples,
	    /*Convproc::MAXPART*/ n_part,
	    /*density*/ 0
	    );

	/* map channels
	 * - Mono:
	 *    always use first only
	 * - MonoToStereo:
	 *    mono-file: use 1st for both M -> L, M -> R
	 *    else: use first two channels
	 * - Stereo
	 *    mono-file: use 1st for both L -> L, R -> R, no x-over
	 *    stereo-file: L -> L, R -> R  -- no L/R, R/L x-over
	 *    3chan-file: ignore 3rd channel, use as stereo-file.
	 *    4chan file:  L -> L, L -> R, R -> L, R -> R
	 */

	uint32_t n_imp = n_inputs () * n_outputs ();
	uint32_t n_chn = _readables.size ();

	if (_irc == Stereo && n_chn == 3) {
		/* ignore 3rd channel */
		n_chn = 2;
	}
	if (_irc == Stereo && n_chn <= 2) {
		/* ignore x-over */
		n_imp = 2;
	}

#ifndef NDEBUG
	printf ("Convolver::reconfigure Nin=%d Nout=%d Nimp=%d Nchn=%d\n", n_inputs (), n_outputs (), n_imp, n_chn);
#endif

	assert (n_imp <= 4);

	for (uint32_t i = 0; i < 4; ++i) {
		_tdc[i].reset ();
	}

	_dly[0].reset (_n_samples);
	_dly[1].reset (_n_samples);

	for (uint32_t c = 0; c < n_imp && rv == 0; ++c) {
		int ir_c = c % n_chn;
		int io_o = c % n_outputs ();
		int io_i;

		if (n_imp == 2 && _irc == Stereo) {
			/*           (imp, in, out)
			 * Stereo       (2, 2, 2)    1: L -> L, 2: R -> R
			 */
			io_i = c % n_inputs ();
		} else {
			/*           (imp, in, out)
			 * Mono         (1, 1, 1)   1: M -> M
			 * MonoToStereo (2, 1, 2)   1: M -> L, 2: M -> R
			 * Stereo       (4, 2, 2)   1: L -> L, 2: L -> R, 3: R -> L, 4: R -> R
			 */
			io_i = (c / n_outputs ()) % n_inputs ();
		}

		Readable* r = _readables[ir_c];
		assert (r->readable_length () == _max_size);
		assert (r->n_channels () == 1);

		const float    chan_gain  = _ir_settings.gain * _ir_settings.channel_gain[c];
		const uint32_t chan_delay = _ir_settings.pre_delay + _ir_settings.channel_delay[c];

#ifndef NDEBUG
		printf ("Convolver map: IR-chn %d: in %d -> out %d (gain: %.1fdB delay; %d)\n", ir_c + 1, io_i + 1, io_o + 1, 20.f * log10f (chan_gain), chan_delay);
#endif

		/* this allows for 4 channel files
		 *    LL, LR, RL, RR
		 * to be used in simple stereo lower CPU configuration:
		 *    LL, --, --, RR
		 */
		if (chan_gain == 0.f) {
			continue;
		}

		assert ((io_i * 2 + io_o) < 4);
		_tdc[io_i * 2 + io_o].configure (r, chan_gain, chan_delay);

		uint32_t pos = 0;
		while (true) {
			float ir[8192];

			uint64_t to_read = std::min ((uint32_t)8192, _max_size - pos);
			uint64_t ns      = r->read (ir, pos, to_read, 0);

			if (ns == 0) {
				assert (pos == _max_size);
				break;
			}

			if (chan_gain != 1.f) {
				for (uint64_t i = 0; i < ns; ++i) {
					ir[i] *= chan_gain;
				}
			}

			rv = _convproc.impdata_create (
			    /*i/o map */ io_i, io_o,
			    /*stride, de-interleave */ 1,
			    ir,
			    chan_delay + pos, chan_delay + pos + ns);

			if (rv != 0) {
				break;
			}

			pos += ns;

			if (pos == _max_size) {
				break;
			}
		}
	}

	if (rv == 0) {
		rv = _convproc.start_process (_sched_priority, _sched_policy);
	}

	assert (rv == 0); // bail out in debug builds

	if (rv != 0) {
		_convproc.stop_process ();
		_convproc.cleanup ();
		_configured = false;
		return;
	}

	_configured = true;

#ifndef NDEBUG
	_convproc.print (stdout);
#endif
}

bool
Convolver::ready () const
{
	return _configured && _convproc.state () == Convproc::ST_PROC;
}

void
Convolver::set_output_gain (float dry, float wet, bool interpolate)
{
	_dry_target = dry;
	_wet_target = wet;
	if (!interpolate) {
		_dry = _dry_target;
		_wet = _wet_target;
	}
}

void
Convolver::interpolate_gain ()
{
	if (_dry != _dry_target) {
		_dry += _a * (_dry_target - _dry) + 1e-10f;
		if (fabsf (_dry - _dry_target) < 1e-5f) {
			_dry = _dry_target;
		}
	}
	if (_wet != _wet_target) {
		_wet += _a * (_wet_target - _wet) + 1e-10f;
		if (fabsf (_wet - _wet_target) < 1e-5f) {
			_wet = _wet_target;
		}
	}
}

void
Convolver::output (float* dst, const float* src, uint32_t n) const
{
	if (_dry == 0.f && _wet == 1.f) {
		memcpy (dst, src, n * sizeof (float));
	} else {
		const float dry = _dry;
		const float wet = _wet;
		for (uint64_t i = 0; i < n; ++i) {
			dst[i] = dry * dst[i] + wet * src[i];
		}
	}
}

void
Convolver::run_buffered_mono (float* buf, uint32_t n_samples)
{
	assert (_convproc.state () == Convproc::ST_PROC);
	assert (_irc == Mono);

	uint32_t done   = 0;
	uint32_t remain = n_samples;

	while (remain > 0) {
		uint32_t ns = std::min (remain, _n_samples - _offset);

		float* const       in  = _convproc.inpdata (/*channel*/ 0);
		float const* const out = _convproc.outdata (/*channel*/ 0);

		memcpy (&in[_offset], &buf[done], sizeof (float) * ns);

		if (_dry == _dry_target && _dry == 0) {
			_dly[0].clear ();
		} else {
			_dly[0].run (&buf[done], ns);
		}

		interpolate_gain ();
		output (&buf[done], &out[_offset], ns);

		_offset += ns;
		done    += ns;
		remain  -= ns;

		if (_offset == _n_samples) {
			_convproc.process ();
			_offset = 0;
		}
	}
}

void
Convolver::run_buffered_stereo (float* left, float* right, uint32_t n_samples)
{
	assert (_convproc.state () == Convproc::ST_PROC);
	assert (_irc != Mono);

	uint32_t done   = 0;
	uint32_t remain = n_samples;

	while (remain > 0) {
		uint32_t ns = std::min (remain, _n_samples - _offset);

		memcpy (&_convproc.inpdata (0)[_offset], &left[done], sizeof (float) * ns);
		if (_irc >= Stereo) {
			memcpy (&_convproc.inpdata (1)[_offset], &right[done], sizeof (float) * ns);
		}

		if (_dry == _dry_target && _dry == 0) {
			_dly[0].clear ();
			_dly[1].clear ();
		} else {
			_dly[0].run (&left[done], ns);
			_dly[1].run (&right[done], ns);
		}

		interpolate_gain ();
		output (&left[done], &_convproc.outdata (0)[_offset], ns);
		output (&right[done], &_convproc.outdata (1)[_offset], ns);

		_offset += ns;
		done    += ns;
		remain  -= ns;

		if (_offset == _n_samples) {
			_convproc.process ();
			_offset = 0;
		}
	}
}

void
Convolver::run_mono (float* buf, uint32_t n_samples)
{
	assert (_convproc.state () == Convproc::ST_PROC);
	assert (_irc == Mono);

	uint32_t done   = 0;
	uint32_t remain = n_samples;

	while (remain > 0) {
		uint32_t ns = std::min (remain, _n_samples - _offset);

		float* const in  = _convproc.inpdata (/*channel*/ 0);
		float* const out = _convproc.outdata (/*channel*/ 0);

		memcpy (&in[_offset], &buf[done], sizeof (float) * ns);

		if (_offset + ns == _n_samples) {
			_convproc.process ();
			interpolate_gain ();
			output (&buf[done], &out[_offset], ns);
			_offset = 0;
		} else {
			assert (remain == ns);
			_convproc.tailonly (_offset + ns);
			_tdc[0].run (&out[_offset], &buf[done], ns);
			interpolate_gain ();
			output (&buf[done], &out[_offset], ns);
			_offset += ns;
		}
		done   += ns;
		remain -= ns;
	}
}

void
Convolver::run_stereo (float* left, float* right, uint32_t n_samples)
{
	assert (_convproc.state () == Convproc::ST_PROC);
	assert (_irc != Mono);

	uint32_t done   = 0;
	uint32_t remain = n_samples;

	float* const outL = _convproc.outdata (0);
	float* const outR = _convproc.outdata (1);

	while (remain > 0) {
		uint32_t ns = std::min (remain, _n_samples - _offset);

		memcpy (&_convproc.inpdata (0)[_offset], &left[done], sizeof (float) * ns);
		if (_irc >= Stereo) {
			memcpy (&_convproc.inpdata (1)[_offset], &right[done], sizeof (float) * ns);
		}

		if (_offset + ns == _n_samples) {
			_convproc.process ();
			interpolate_gain ();
			output (&left[done],  &outL[_offset], ns);
			output (&right[done], &outR[_offset], ns);
			_offset = 0;
		} else {
			assert (remain == ns);

			_convproc.tailonly (_offset + ns);

			_tdc[0].run (&outL[_offset], &left[done], ns);
			_tdc[1].run (&outL[_offset], &right[done], ns);
			_tdc[2].run (&outR[_offset], &left[done], ns);
			_tdc[3].run (&outR[_offset], &right[done], ns);

			interpolate_gain ();
			output (&left[done],  &outL[_offset], ns);
			output (&right[done], &outR[_offset], ns);
			_offset += ns;
		}
		done   += ns;
		remain -= ns;

	}
}

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
#include <string.h>

#include "audiosrc.h"
#include "convolver.h"

#if ZITA_CONVOLVER_MAJOR_VERSION != 3 && ZITA_CONVOLVER_MAJOR_VERSION != 4
# error "This programs requires zita-convolver 3 or 4"
#endif

using namespace ZeroConvoLV2;

Convolver::Convolver (
		std::string const& path,
		uint32_t sample_rate,
		int sched_policy,
		int sched_priority,
		IRChannelConfig irc,
		uint32_t pre_delay)
  : _path (path)
	, _irc (irc)
  , _initial_delay (pre_delay)
  , _sched_policy (sched_policy)
  , _sched_priority (sched_priority)
  , _n_samples (0)
  , _max_size (0)
  , _offset (0)
  , _configured (false)
{
#if 0 // Test Signal
	_fs = new MemSource ();
#else
	if (_path.substr (0, 4) == "mem:") {
		_fs = new VirtFileSource (_path);
	} else {
		_fs = new FileSource (_path);
	}
#endif

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
Convolver::reconfigure (uint32_t block_size)
{
	_convproc.stop_process ();
	_convproc.cleanup ();
	_convproc.set_options (0);

	assert (!_readables.empty ());

	_offset    = 0;
	_n_samples = block_size;
	_max_size  = _readables[0]->readable_length ();

	uint32_t power_of_two;
	for (power_of_two = 1; 1U << power_of_two < _n_samples; ++power_of_two) ;
	_n_samples = 1 << power_of_two;

	int n_part = std::min ((uint32_t)Convproc::MAXPART, 4 * _n_samples);

	int rv = _convproc.configure (
	    /*in*/ n_inputs (),
	    /*out*/ n_outputs (),
	    /*max-convolution length */ _max_size,
	    /*quantum, nominal-buffersize*/ _n_samples,
	    /*Convproc::MINPART*/ _n_samples,
	    /*Convproc::MAXPART*/ n_part
#if ZITA_CONVOLVER_MAJOR_VERSION == 4
	    /*density*/, 0
#endif
	    );
#if ZITA_CONVOLVER_MAJOR_VERSION == 3
	_convproc.set_density (0);
#endif

	/* map channels
	 * - Mono:
	 *    always use first only
	 * - MonoToStereo:
	 *    mono-file: use 1st for M -> L, M -> R
	 *    else: use first two channels
	 * - Stereo
	 *    mono-file: use 1st for both L -> L, R -> R, no x-over
	 *    stereo-file: L -> L, R -> R  -- no L/R, R/L x-over
	 *    3chan-file: ignore 3rd channel, use as stereo-file.
	 *    4chan file:  L -> L, L -> R, R -> R, R -> L
	 */

	uint32_t n_imp = n_inputs () * n_outputs ();
	uint32_t n_chn = _readables.size ();

#ifndef NDEBUG
	printf ("Convolver::reconfigure Nin %d Nout %d Nimp %d Nchn %d\n", n_inputs (), n_outputs (), n_imp, n_chn);
#endif

	if (_irc == Stereo && n_chn == 3) {
		/* ignore 3rd channel */
		n_chn = 2;
	}
	if (_irc == Stereo && n_chn <= 2) {
		/* ignore x-over */
		n_imp = 2;
	}

	for (uint32_t c = 0; c < n_imp && rv == 0; ++c) {
		int ir_c = c % n_chn;
		int io_o = c % n_outputs ();
		int io_i;

		if (n_imp == 2 && n_imp == 2 && _irc == Stereo) {
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

#ifndef NDEBUG
		printf ("Convolver map: IR-chn %d: in %d -> out %d\n", ir_c + 1, io_i + 1, io_o + 1);
#endif

		Readable* r = _readables[ir_c];
		assert (r->readable_length () == _max_size);
		assert (r->n_channels () == 1);

		uint32_t pos = 0;
		while (true) {
			float ir[8192];

			uint64_t to_read = std::min ((uint32_t)8192, _max_size - pos);
			uint64_t ns      = r->read (ir, pos, to_read, 0);

			if (ns == 0) {
				assert (pos == _max_size);
				break;
			}

			rv = _convproc.impdata_create (
			    /*i/o map */ io_i, io_o,
			    /*stride, de-interleave */ 1,
			    ir,
			    _initial_delay + pos, _initial_delay + pos + ns);

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
		rv = _convproc.start_process (_sched_priority,_sched_policy);
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
Convolver::run (float* buf, uint32_t n_samples)
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
		memcpy (&buf[done], &out[_offset], sizeof (float) * ns);

		_offset += ns;
		done    += ns;
		remain  -= ns;

		if (_offset == _n_samples) {
			_convproc.process (/*sync, freewheeling*/ true);
			_offset = 0;
		}
	}
}

void
Convolver::run_stereo (float* left, float* right, uint32_t n_samples)
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
		memcpy (&left[done], &_convproc.outdata (0)[_offset], sizeof (float) * ns);
		memcpy (&right[done], &_convproc.outdata (1)[_offset], sizeof (float) * ns);

		_offset += ns;
		done    += ns;
		remain  -= ns;

		if (_offset == _n_samples) {
			_convproc.process (true);
			_offset = 0;
		}
	}
}

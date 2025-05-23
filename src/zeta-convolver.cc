// ----------------------------------------------------------------------------
//
//  Copyright (C) 2006-2018 Fons Adriaensen <fons@linuxaudio.org>
//  Copyright (C) 2020-2021 Robin Gareus <robin@gareus.org>
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation; either version 3 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
// ----------------------------------------------------------------------------
// This is a customized version of Fons Adriaensen's libzita-convolver.
//
// A detailed history is available via `git log`.
//
// * Add new API method `readtail` which allows runs the FFT convolver for
//   everything except the first part (_parsize) of the IR.
//   It is intended for partial-cycles (smaller than nominal block), where the
//   remaining part can be convolved independently.
//   The current implementation does that in the time-domain.
// * Rename the header file "zita" -> "zeta" to indicate the change
//   Also a dedicated namespace/scope was added to disambiguate symbols.
// * clang-format code; variable are scoped and `const' added where appropriate
// * Add support for macOS realtime thread priority (pthread priorities play
//   no role on that OS)
// * Fix race-condition when re-initializing Convproc while it it is still
//   initializing.
// * Fix compilation with pthread-w32 with mingw (pthread_t is opaque)
// * Remove unused interfaces which are not required for the plugin version,
//   notably the API to link and update IRs, but also static globals were
//   dropped.
//
// ----------------------------------------------------------------------------

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef __APPLE__
#include <mach/mach_time.h>
#include <mach/thread_act.h>
#include <mach/thread_policy.h>
#endif

#include "zeta-convolver.h"

float Convproc::_mac_cost = 1.0f;
float Convproc::_fft_cost = 5.0f;

static pthread_mutex_t fftw_planner_lock = PTHREAD_MUTEX_INITIALIZER;

static float*
calloc_real (uint32_t k)
{
	float* p = fftwf_alloc_real (k);
	if (!p) {
		throw (Converror (Converror::MEM_ALLOC));
	}
	memset (p, 0, k * sizeof (float));
	return p;
}

static fftwf_complex*
calloc_complex (uint32_t k)
{
	fftwf_complex* p = fftwf_alloc_complex (k);
	if (!p) {
		throw (Converror (Converror::MEM_ALLOC));
	}
	memset (p, 0, k * sizeof (fftwf_complex));
	return p;
}

Convproc::Convproc (void)
	: _state (ST_IDLE)
	, _options (0)
	, _ninp (0)
	, _nout (0)
	, _quantum (0)
	, _minpart (0)
	, _maxpart (0)
	, _nlevels (0)
	, _latecnt (0)
{
	memset (_inpbuff, 0, sizeof (_inpbuff)); // MAXINP
	memset (_outbuff, 0, sizeof (_outbuff)); // MAXOUT
	memset (_convlev, 0, sizeof (_convlev)); // MAXLEV
}

Convproc::~Convproc (void)
{
	stop_process ();
	cleanup ();
}

void
Convproc::set_options (uint32_t options)
{
	_options = options;
}

int
Convproc::configure (uint32_t ninp,
                     uint32_t nout,
                     uint32_t maxsize,
                     uint32_t quantum,
                     uint32_t minpart,
                     uint32_t maxpart,
                     float    density)
{
	uint32_t offs, npar, size, pind, nmin, i;
	int      prio, step, d, r, s;
	float    cfft, cmac;

	if (_state != ST_IDLE) {
		return Converror::BAD_STATE;
	}
	if (   (ninp < 1) || (ninp > MAXINP)
	    || (nout < 1) || (nout > MAXOUT)
	    || (quantum & (quantum - 1))
	    || (quantum < MINQUANT)
	    || (quantum > MAXQUANT)
	    || (minpart & (minpart - 1))
	    || (minpart < MINPART)
	    || (minpart < quantum)
	    || (minpart > MAXDIVIS * quantum)
	    || (maxpart & (maxpart - 1))
	    || (maxpart > MAXPART)
	    || (maxpart < minpart)) {
		return Converror::BAD_PARAM;
	}

	nmin = (ninp < nout) ? ninp : nout;
	if (density <= 0.0f) {
		density = 1.0f / nmin;
	}
	if (density > 1.0f) {
		density = 1.0f;
	}
	cfft = _fft_cost * (ninp + nout);
	cmac = _mac_cost * ninp * nout * density;
	step = (cfft < 4 * cmac) ? 1 : 2;
	if (step == 2) {
		r = maxpart / minpart;
		s = (r & 0xAAAA) ? 1 : 2;
	} else {
		s = 1;
	}
	nmin = (s == 1) ? 2 : 6;
	if (minpart == quantum) {
		nmin++;
	}
	prio = 0;
	size = quantum;
	while (size < minpart) {
		prio -= 1;
		size <<= 1;
	}

	try {
		for (offs = pind = 0; offs < maxsize; pind++) {
			npar = (maxsize - offs + size - 1) / size;
			if ((size < maxpart) && (npar > nmin)) {
				r = 1 << s;
				d = npar - nmin;
				d = d - (d + r - 1) / r;
				if (cfft < d * cmac) {
					npar = nmin;
				}
			}
			_convlev[pind] = new Convlevel ();
			_convlev[pind]->configure (prio, offs, npar, size, _options);
			offs += size * npar;
			if (offs < maxsize) {
				prio -= s;
				size <<= s;
				s    = step;
				nmin = (s == 1) ? 2 : 6;
			}
		}

		_ninp    = ninp;
		_nout    = nout;
		_quantum = quantum;
		_minpart = minpart;
		_maxpart = size;
		_nlevels = pind;
		_latecnt = 0;
		_inpsize = 2 * size;

		for (i = 0; i < ninp; i++) {
			_inpbuff[i] = new float[_inpsize];
		}
		for (i = 0; i < nout; i++) {
			_outbuff[i] = new float[_minpart];
		}
	} catch (...) {
		cleanup ();
		return Converror::MEM_ALLOC;
	}

	_state = ST_STOP;
	return 0;
}

int
Convproc::impdata_create (uint32_t inp,
                          uint32_t out,
                          int32_t  step,
                          float*   data,
                          int32_t  ind0,
                          int32_t  ind1)
{
	uint32_t j;

	if (_state != ST_STOP) {
		return Converror::BAD_STATE;
	}
	if ((inp >= _ninp) || (out >= _nout)) {
		return Converror::BAD_PARAM;
	}

	try {
		for (j = 0; j < _nlevels; j++) {
			_convlev[j]->impdata_write (inp, out, step, data, ind0, ind1, true);
		}
	} catch (...) {
		cleanup ();
		return Converror::MEM_ALLOC;
	}
	return 0;
}

int
Convproc::impdata_clear (uint32_t inp, uint32_t out)
{
	uint32_t k;

	if (_state < ST_STOP) {
		return Converror::BAD_STATE;
	}
	for (k = 0; k < _nlevels; k++) {
		_convlev[k]->impdata_clear (inp, out);
	}
	return 0;
}

int
Convproc::reset (void)
{
	uint32_t k;

	if (_state == ST_IDLE) {
		return Converror::BAD_STATE;
	}
	for (k = 0; k < _ninp; k++) {
		memset (_inpbuff[k], 0, _inpsize * sizeof (float));
	}
	for (k = 0; k < _nout; k++) {
		memset (_outbuff[k], 0, _minpart * sizeof (float));
	}
	for (k = 0; k < _nlevels; k++) {
		_convlev[k]->reset (_inpsize, _minpart, _inpbuff, _outbuff);
	}
	return 0;
}

int
Convproc::start_process (int abspri, int policy, double period_ns)
{
	if (_state != ST_STOP) {
		return Converror::BAD_STATE;
	}
	return restart_process (abspri, policy, period_ns);
}

int
Convproc::restart_process (int abspri, int policy, double period_ns)
{
	uint32_t k;
	switch (_state) {
		case ST_STOP:
			/* OK, configured, but not yet started */
			break;
		case ST_WAIT:
			/* OK, already stopped */
			break;
		case ST_PROC:
			/* OK, running - restart */
			stop_process ();
			/* wait for detached threads to terminate */
			while (!check_stop ()) {
				usleep (40000);
				sched_yield ();
			}
			break;
		default:
			/* ST_IDLE - not configured */
			return Converror::BAD_STATE;
	}

	_latecnt = 0;
	_inpoffs = 0;
	_outoffs = 0;
	reset ();

	for (k = (_minpart == _quantum) ? 1 : 0; k < _nlevels; k++) {
		if (!_convlev[k]->start (abspri, policy, period_ns)) {
			stop_process (true);
			cleanup ();
			return Converror::BAD_STATE;
		}
	}

	while (!check_started ((_minpart == _quantum) ? 1 : 0)) {
		usleep (40000);
		sched_yield ();
	}

	_state = ST_PROC;
	return 0;
}

int
Convproc::process ()
{
	uint32_t k;
	int      f = 0;

	if (_state != ST_PROC) {
		return 0;
	}

	_inpoffs += _quantum;
	if (_inpoffs == _inpsize) {
		_inpoffs = 0;
	}

	_outoffs += _quantum;
	if (_outoffs == _minpart) {
		_outoffs = 0;
		for (k = 0; k < _nout; k++) {
			memset (_outbuff[k], 0, _minpart * sizeof (float));
		}
		for (k = 0; k < _nlevels; k++) {
			f |= _convlev[k]->readout ();
		}
		if (f) {
			if (++_latecnt >= 5) {
				if (~_options & OPT_LATE_CONTIN) {
					stop_process ();
				}
				f |= FL_LOAD;
			}
		} else {
			_latecnt = 0;
		}
	}
	return f;
}

int
Convproc::tailonly (uint32_t n_samples)
{
	uint32_t k;
	int      f = 0;

	if (_state != ST_PROC) {
		return 0;
	}

	uint32_t outoffs = _outoffs;
	outoffs += _quantum;
	if (outoffs == _minpart) {
		for (k = 0; k < _nout; k++) {
			memset (_outbuff[k], 0, n_samples * sizeof (float));
		}
		for (k = 0; k < _nlevels; k++) {
			f |= _convlev[k]->readtail (n_samples);
		}
	}
	return f;
}

int
Convproc::stop_process (bool force)
{
	uint32_t k;

	if (_state != ST_PROC && !force) {
		return Converror::BAD_STATE;
	}
	for (k = 0; k < _nlevels; k++) {
		_convlev[k]->stop ();
	}
	_state = ST_WAIT;
	return 0;
}

int
Convproc::cleanup (void)
{
	uint32_t k;

	while (!check_stop ()) {
		usleep (40000);
		sched_yield ();
	}
	for (k = 0; k < _ninp; k++) {
		delete[] _inpbuff[k];
		_inpbuff[k] = 0;
	}
	for (k = 0; k < _nout; k++) {
		delete[] _outbuff[k];
		_outbuff[k] = 0;
	}
	for (k = 0; k < _nlevels; k++) {
		delete _convlev[k];
		_convlev[k] = 0;
	}

	_state   = ST_IDLE;
	_options = 0;
	_ninp    = 0;
	_nout    = 0;
	_quantum = 0;
	_minpart = 0;
	_maxpart = 0;
	_nlevels = 0;
	_latecnt = 0;
	return 0;
}

bool
Convproc::check_started (uint32_t k)
{
	for (; (k < _nlevels) && (_convlev[k]->_stat == Convlevel::ST_PROC); k++) ;
	return (k == _nlevels || _nlevels == 0) ? true : false;
}

bool
Convproc::check_stop (void)
{
	uint32_t k;

	for (k = 0; (k < _nlevels) && (_convlev[k]->_stat == Convlevel::ST_IDLE); k++) ;
	if (k == _nlevels) {
		_state = ST_STOP;
		return true;
	}
	return false;
}

void
Convproc::print (FILE* F)
{
	uint32_t k;

	for (k = 0; k < _nlevels; k++) {
		_convlev[k]->print (F);
	}
}

Convlevel::Convlevel (void)
	: _stat (ST_IDLE)
	, _npar (0)
	, _parsize (0)
	, _options (0)
#ifndef PTW32_VERSION
	, _pthr (0)
#endif
	, _inp_list (0)
	, _out_list (0)
	, _plan_r2c (0)
	, _plan_c2r (0)
	, _time_data (0)
	, _prep_data (0)
	, _freq_data (0)
{
}

Convlevel::~Convlevel (void)
{
	cleanup ();
}

void
Convlevel::configure (int      prio,
                      uint32_t offs,
                      uint32_t npar,
                      uint32_t parsize,
                      uint32_t options)
{
	int fftwopt = (options & OPT_FFTW_MEASURE) ? FFTW_MEASURE : FFTW_ESTIMATE;

	_prio    = prio;
	_offs    = offs;
	_npar    = npar;
	_parsize = parsize;
	_options = options;

	_time_data = calloc_real (2 * _parsize);
	_prep_data = calloc_real (2 * _parsize);
	_freq_data = calloc_complex (_parsize + 1);
	pthread_mutex_lock (&fftw_planner_lock);
	_plan_r2c = fftwf_plan_dft_r2c_1d (2 * _parsize, _time_data, _freq_data, fftwopt);
	_plan_c2r = fftwf_plan_dft_c2r_1d (2 * _parsize, _freq_data, _time_data, fftwopt);
	pthread_mutex_unlock (&fftw_planner_lock);
	if (_plan_r2c && _plan_c2r) {
		return;
	}
	throw (Converror (Converror::MEM_ALLOC));
}

void
Convlevel::impdata_write (uint32_t inp,
                          uint32_t out,
                          int32_t  step,
                          float*   data,
                          int32_t  i0,
                          int32_t  i1,
                          bool     create)
{
	uint32_t       k;
	int32_t        j, j0, j1, n;
	float          norm;
	fftwf_complex* fftb;
	Macnode*       M;

	n  = i1 - i0;
	i0 = _offs - i0;
	i1 = i0 + _npar * _parsize;
	if ((i0 >= n) || (i1 <= 0)) {
		return;
	}

	if (create) {
		M = findmacnode (inp, out, true);
		if (M == 0 || M->_link) {
			return;
		}
		if (M->_fftb == 0) {
			M->alloc_fftb (_npar);
		}
	} else {
		M = findmacnode (inp, out, false);
		if (M == 0 || M->_link || M->_fftb == 0) {
			return;
		}
	}

	norm = 0.5f / _parsize;
	for (k = 0; k < _npar; k++) {
		i1 = i0 + _parsize;
		if ((i0 < n) && (i1 > 0)) {
			fftb = M->_fftb[k];
			if (fftb == 0 && create) {
				M->_fftb[k] = fftb = calloc_complex (_parsize + 1);
			}
			if (fftb && data) {
				memset (_prep_data, 0, 2 * _parsize * sizeof (float));
				j0 = (i0 < 0) ? 0 : i0;
				j1 = (i1 > n) ? n : i1;
				for (j = j0; j < j1; j++) {
					_prep_data[j - i0] = norm * data[j * step];
				}
				fftwf_execute_dft_r2c (_plan_r2c, _prep_data, _freq_data);
				for (j = 0; j <= (int)_parsize; j++) {
					fftb[j][0] += _freq_data[j][0];
					fftb[j][1] += _freq_data[j][1];
				}
			}
		}
		i0 = i1;
	}
}

void
Convlevel::impdata_clear (uint32_t inp, uint32_t out)
{
	uint32_t i;
	Macnode* M;

	M = findmacnode (inp, out, false);
	if (M == 0 || M->_link || M->_fftb == 0) {
		return;
	}
	for (i = 0; i < _npar; i++) {
		if (M->_fftb[i]) {
			memset (M->_fftb[i], 0, (_parsize + 1) * sizeof (fftwf_complex));
		}
	}
}

void
Convlevel::reset (uint32_t inpsize,
                  uint32_t outsize,
                  float**  inpbuff,
                  float**  outbuff)
{
	uint32_t i;
	Inpnode* X;
	Outnode* Y;

	_inpsize = inpsize;
	_outsize = outsize;
	_inpbuff = inpbuff;
	_outbuff = outbuff;
	for (X = _inp_list; X; X = X->_next) {
		for (i = 0; i < _npar; i++) {
			memset (X->_ffta[i], 0, (_parsize + 1) * sizeof (fftwf_complex));
		}
	}
	for (Y = _out_list; Y; Y = Y->_next) {
		for (i = 0; i < 3; i++) {
			memset (Y->_buff[i], 0, _parsize * sizeof (float));
		}
	}
	if (_parsize == _outsize) {
		_outoffs = 0;
		_inpoffs = 0;
	} else {
		_outoffs = _parsize / 2;
		_inpoffs = _inpsize - _outoffs;
	}
	_bits  = _parsize / _outsize;
	_wait  = 0;
	_ptind = 0;
	_opind = 0;
	_trig.init (0, 0);
	_done.init (0, 0);
}

bool
Convlevel::start (int abspri, int policy, double period_ns)
{
	int                min, max;
	pthread_attr_t     attr;
	struct sched_param parm;

#ifndef PTW32_VERSION
	_pthr = 0;
#endif
	min = sched_get_priority_min (policy);
	max = sched_get_priority_max (policy);
	abspri += _prio;
	if (abspri > max) {
		abspri = max;
	}
	if (abspri < min) {
		abspri = min;
	}
	parm.sched_priority = abspri;
	pthread_attr_init (&attr);
	pthread_attr_setdetachstate (&attr, PTHREAD_CREATE_DETACHED);
	pthread_attr_setschedpolicy (&attr, policy);
	pthread_attr_setschedparam (&attr, &parm);
	pthread_attr_setscope (&attr, PTHREAD_SCOPE_SYSTEM);
	pthread_attr_setinheritsched (&attr, PTHREAD_EXPLICIT_SCHED);
	pthread_attr_setstacksize (&attr, 0x10000);
	if (pthread_create (&_pthr, &attr, static_main, this) != 0) {
		pthread_attr_destroy (&attr);
		/* Failed to acquire realtime permissions. */
#ifndef REQUIRE_HARD_RT_CAPABILITY
		/* Try to start thread with default scheduling priority.
		 * TODO: pass this up and emit a lv2_log_warning()
		 */
		pthread_attr_init (&attr);
		pthread_attr_setscope (&attr, PTHREAD_SCOPE_SYSTEM);
		pthread_attr_setstacksize (&attr, 0x10000);
		if (pthread_create (&_pthr, &attr, static_main, this) != 0) {
			/* Failed to create process thread */
			pthread_attr_destroy (&attr);
			return false;
		}
#else
		return false;
#endif
	}
	pthread_attr_destroy (&attr);

#ifdef __APPLE__
	/* https://opensource.apple.com/source/xnu/xnu-4570.61.1/osfmk/mach/thread_policy.h.auto.html */

	/* Ask for fixed priority */
	thread_extended_policy_data_t tep;
	tep.timeshare = false;

	thread_policy_set (pthread_mach_thread_np (_pthr),
	                   THREAD_EXTENDED_POLICY,
	                   (thread_policy_t)&tep,
	                   THREAD_EXTENDED_POLICY_COUNT);

	/* relative value of the computation compared to the other threads in the task. */
	thread_precedence_policy_data_t tpp;
	tpp.importance = 60; // MAXPRI_USER - 3

	thread_policy_set (pthread_mach_thread_np (_pthr),
	                   THREAD_PRECEDENCE_POLICY,
	                   (thread_policy_t)&tpp,
	                   THREAD_PRECEDENCE_POLICY_COUNT);

	mach_timebase_info_data_t timebase;
	if (KERN_SUCCESS == mach_timebase_info (&timebase)) {
		thread_time_constraint_policy_data_t ttcp;

		ttcp.period      = period_ns      *  timebase.denom / timebase.numer;
		ttcp.computation = period_ns * .3 *  timebase.denom / timebase.numer;
		ttcp.constraint  = period_ns * .9 *  timebase.denom / timebase.numer;
		ttcp.preemptible = true;

		thread_policy_set (pthread_mach_thread_np (_pthr),
		                   THREAD_TIME_CONSTRAINT_POLICY,
		                   (thread_policy_t)&ttcp,
		                   THREAD_TIME_CONSTRAINT_POLICY_COUNT);
	}
#endif
	return true;
}

void
Convlevel::stop (void)
{
	if (_stat != ST_IDLE) {
		_stat = ST_TERM;
		_trig.post ();
	}
}

void
Convlevel::cleanup (void)
{
	Inpnode *X, *X1;
	Outnode *Y, *Y1;
	Macnode *M, *M1;

	X = _inp_list;
	while (X) {
		X1 = X->_next;
		delete X;
		X = X1;
	}
	_inp_list = 0;

	Y = _out_list;
	while (Y) {
		M = Y->_list;
		while (M) {
			M1 = M->_next;
			delete M;
			M = M1;
		}
		Y1 = Y->_next;
		delete Y;
		Y = Y1;
	}
	_out_list = 0;

	pthread_mutex_lock (&fftw_planner_lock);
	fftwf_destroy_plan (_plan_r2c);
	fftwf_destroy_plan (_plan_c2r);
	pthread_mutex_unlock (&fftw_planner_lock);

	fftwf_free (_time_data);
	fftwf_free (_prep_data);
	fftwf_free (_freq_data);
	_plan_r2c  = 0;
	_plan_c2r  = 0;
	_time_data = 0;
	_prep_data = 0;
	_freq_data = 0;
}

void*
Convlevel::static_main (void* arg)
{
	((Convlevel*)arg)->main ();
	return 0;
}

void
Convlevel::main (void)
{
	_stat = ST_PROC;
	while (true) {
		_trig.wait ();
		if (_stat == ST_TERM) {
			_stat = ST_IDLE;
#ifndef PTW32_VERSION
			_pthr = 0;
#endif
			return;
		}
		process ();
		_done.post ();
	}
}

void
Convlevel::process ()
{
	uint32_t       i, i1, j, k, n1, n2, opi1, opi2;
	Inpnode const* X;
	Macnode const* M;
	Outnode const* Y;
	fftwf_complex* ffta;
	fftwf_complex* fftb;
	float*         inpd;
	float*         outd;

	i1       = _inpoffs;
	n1       = _parsize;
	n2       = 0;
	_inpoffs = i1 + n1;
	if (_inpoffs >= _inpsize) {
		_inpoffs -= _inpsize;
		n2 = _inpoffs;
		n1 -= n2;
	}

	opi1 = (_opind + 1) % 3;
	opi2 = (_opind + 2) % 3;

	for (X = _inp_list; X; X = X->_next) {
		inpd = _inpbuff[X->_inp];
		if (n1) {
			memcpy (_time_data, inpd + i1, n1 * sizeof (float));
		}
		if (n2) {
			memcpy (_time_data + n1, inpd, n2 * sizeof (float));
		}
		memset (_time_data + _parsize, 0, _parsize * sizeof (float));
		fftwf_execute_dft_r2c (_plan_r2c, _time_data, X->_ffta[_ptind]);
	}

	for (Y = _out_list; Y; Y = Y->_next) {
		memset (_freq_data, 0, (_parsize + 1) * sizeof (fftwf_complex));
		for (M = Y->_list; M; M = M->_next) {
			X = M->_inpn;
			i = _ptind;
			for (j = 0; j < _npar; j++) {
				ffta = X->_ffta[i];
				fftb = M->_link ? M->_link->_fftb[j] : M->_fftb[j];
				if (fftb) {
					{
						for (k = 0; k <= _parsize; k++) {
							_freq_data[k][0] += ffta[k][0] * fftb[k][0] - ffta[k][1] * fftb[k][1];
							_freq_data[k][1] += ffta[k][0] * fftb[k][1] + ffta[k][1] * fftb[k][0];
						}
					}
				}
				if (i == 0) {
					i = _npar;
				}
				i--;
			}
		}

		fftwf_execute_dft_c2r (_plan_c2r, _freq_data, _time_data);
		outd = Y->_buff[opi1];
		for (k = 0; k < _parsize; k++) {
			outd[k] += _time_data[k];
		}
		outd = Y->_buff[opi2];
		memcpy (outd, _time_data + _parsize, _parsize * sizeof (float));
	}

	_ptind++;
	if (_ptind == _npar) {
		_ptind = 0;
	}
}

int
Convlevel::readout ()
{
	uint32_t       i;
	float *        p, *q;
	Outnode const* Y;

	_outoffs += _outsize;
	if (_outoffs == _parsize) {
		_outoffs = 0;
		if (_stat == ST_PROC) {
			while (_wait) {
				_done.wait ();
				_wait--;
			}
			if (++_opind == 3) {
				_opind = 0;
			}
			_trig.post ();
			_wait++;
		} else {
			process ();
			if (++_opind == 3) {
				_opind = 0;
			}
		}
	}

	for (Y = _out_list; Y; Y = Y->_next) {
		p = Y->_buff[_opind] + _outoffs;
		q = _outbuff[Y->_out];
		for (i = 0; i < _outsize; i++) {
			q[i] += p[i];
		}
	}

	return (_wait > 1) ? _bits : 0;
}

int
Convlevel::readtail (uint32_t n_samples)
{
	Outnode const* Y;

	uint32_t opind   = _opind;
	uint32_t outoffs = _outoffs + _outsize;
	if (outoffs == _parsize) {
		while (_wait) {
			_done.wait ();
			_wait--;
		}

		outoffs = 0;
		if (++opind == 3) {
			opind = 0;
		}
	}

	for (Y = _out_list; Y; Y = Y->_next) {
		float const* const p = Y->_buff[opind] + outoffs;
		float* const       q = _outbuff[Y->_out];
		for (uint32_t i = 0; i < n_samples; i++) {
			q[i] += p[i];
		}
	}
	return 0;
}

void
Convlevel::print (FILE* F)
{
	fprintf (F, "prio = %4d, offs = %6d,  parsize = %5d,  npar = %3d\n", _prio, _offs, _parsize, _npar);
}

Macnode*
Convlevel::findmacnode (uint32_t inp, uint32_t out, bool create)
{
	Inpnode* X;
	Outnode* Y;
	Macnode* M;

	for (X = _inp_list; X && (X->_inp != inp); X = X->_next) {
		;
	}
	if (!X) {
		if (!create) {
			return 0;
		}
		X         = new Inpnode (inp);
		X->_next  = _inp_list;
		_inp_list = X;
		X->alloc_ffta (_npar, _parsize);
	}

	for (Y = _out_list; Y && (Y->_out != out); Y = Y->_next) {
		;
	}
	if (!Y) {
		if (!create) {
			return 0;
		}
		Y         = new Outnode (out, _parsize);
		Y->_next  = _out_list;
		_out_list = Y;
	}

	for (M = Y->_list; M && (M->_inpn != X); M = M->_next) {
		;
	}
	if (!M) {
		if (!create) {
			return 0;
		}
		M        = new Macnode (X);
		M->_next = Y->_list;
		Y->_list = M;
	}

	return M;
}

Inpnode::Inpnode (uint16_t inp)
	: _next (0)
	, _ffta (0)
	, _npar (0)
	, _inp (inp)
{
}

Inpnode::~Inpnode (void)
{
	free_ffta ();
}

void
Inpnode::alloc_ffta (uint16_t npar, int32_t size)
{
	_npar = npar;
	_ffta = new fftwf_complex*[_npar];
	for (int i = 0; i < _npar; i++) {
		_ffta[i] = calloc_complex (size + 1);
	}
}

void
Inpnode::free_ffta (void)
{
	if (!_ffta) {
		return;
	}
	for (uint16_t i = 0; i < _npar; i++) {
		fftwf_free (_ffta[i]);
	}
	delete[] _ffta;
	_ffta = 0;
	_npar = 0;
}

Macnode::Macnode (Inpnode* inpn)
	: _next (0)
	, _inpn (inpn)
	, _link (0)
	, _fftb (0)
	, _npar (0)
{
}

Macnode::~Macnode (void)
{
	free_fftb ();
}

void
Macnode::alloc_fftb (uint16_t npar)
{
	_npar = npar;
	_fftb = new fftwf_complex*[_npar];
	for (uint16_t i = 0; i < _npar; i++) {
		_fftb[i] = 0;
	}
}

void
Macnode::free_fftb (void)
{
	if (!_fftb) {
		return;
	}
	for (uint16_t i = 0; i < _npar; i++) {
		fftwf_free (_fftb[i]);
	}
	delete[] _fftb;
	_fftb = 0;
	_npar = 0;
}

Outnode::Outnode (uint16_t out, int32_t size)
	: _next (0)
	, _list (0)
	, _out (out)
{
	_buff[0] = calloc_real (size);
	_buff[1] = calloc_real (size);
	_buff[2] = calloc_real (size);
}

Outnode::~Outnode (void)
{
	fftwf_free (_buff[0]);
	fftwf_free (_buff[1]);
	fftwf_free (_buff[2]);
}

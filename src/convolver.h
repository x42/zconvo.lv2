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

#pragma once

#include <string>
#include <vector>
#include <zita-convolver.h>

#include "readable.h"

namespace ZeroConvoLV2
{
class Convolver
{
public:
	enum IRChannelConfig {
		Mono,         ///< 1 in, 1 out; 1ch IR
		MonoToStereo, ///< 1 in, 2 out, stereo IR  M -> L, M -> R
		Stereo,       ///< 2 in, 2 out, stereo IR  L -> L, R -> R || 4 chan IR  L -> L, L -> R, R -> R, R -> L
	};

	Convolver (std::string const&,
			uint32_t sample_rate,
			int sched_policy,
			int sched_priority,
			IRChannelConfig irc = Mono,
			uint32_t pre_delay = 0);
	~Convolver ();

	void run (float*, uint32_t);
	void run_stereo (float* L, float* R, uint32_t);

	void reconfigure (uint32_t);

	uint32_t latency () const { return _n_samples; }

	uint32_t n_inputs  () const { return _irc < Stereo ? 1 : 2; }
	uint32_t n_outputs () const { return _irc == Mono  ? 1 : 2; }

	std::string const& path () const { return _path; }

	bool ready () const;

private:
	Readable*              _fs;
	std::vector<Readable*> _readables;
	Convproc               _convproc;

	std::string     _path;
	IRChannelConfig _irc;
	uint32_t        _initial_delay;
	int             _sched_policy;
	int             _sched_priority;


	uint32_t _n_samples;
	uint32_t _max_size;
	uint32_t _offset;
	bool     _configured;
};

} /* namespace */

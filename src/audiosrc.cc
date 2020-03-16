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

#include <cmath>
#include <string.h>

#include "audiosrc.h"

using namespace ZeroConvoLV2;

SrcSource::SrcSource (Readable* r, uint32_t target_rate)
    : _source (r)
    , _target_rate (target_rate)
    , _src_state (0)
    , _source_position (0)
    , _target_position (0)
    , _fract_position (0)
{
	_ratio              = target_rate / (double)_source->sample_rate ();
	_src_data.src_ratio = _ratio;

	uint32_t src_buffer_size = ceil (8192.0 / _ratio) + 2;
	_src_buffer              = new float[src_buffer_size];

	if (r->n_channels () != 1) {
		throw std::runtime_error ("Error: src_new failed, src channel count != 1");
	}

	int err;
	if (0 == (_src_state = src_new (SRC_SINC_BEST_QUALITY, 1, &err))) {
		std::string msg (std::string ("Error: src_new failed. ") + std::string (src_strerror (err)));
		throw std::runtime_error (msg);
	}
}

SrcSource::~SrcSource ()
{
	_src_state = src_delete (_src_state);
	delete[] _src_buffer;
	delete _source;
}

uint64_t
SrcSource::read (float* dst, uint64_t pos, uint64_t cnt, uint32_t) const
{
	int          err;
	const double srccnt = cnt / _ratio;

	if (_target_position != pos) {
		src_reset (_src_state);
		_fract_position  = 0;
		_source_position = pos / _ratio;
		_target_position = pos;
	}

	const int64_t scnt = ceilf (srccnt - _fract_position);
	_fract_position += (scnt - srccnt);

	_src_data.input_frames = _source->read (_src_buffer, _source_position, scnt, 0);

	if (_src_data.input_frames * _ratio <= cnt && _source_position + scnt >= _source->readable_length ()) {
		_src_data.end_of_input = true;
	} else {
		_src_data.end_of_input = false;
	}

	if (_src_data.input_frames < scnt) {
		_target_position += _src_data.input_frames * _ratio;
	} else {
		_target_position += cnt;
	}

	_src_data.output_frames = cnt;
	_src_data.data_in       = _src_buffer;
	_src_data.data_out      = dst;

	if ((err = src_process (_src_state, &_src_data))) {
		return 0;
	}

	if (_src_data.end_of_input && _src_data.output_frames_gen <= 0) {
		return 0;
	}

	_source_position += _src_data.input_frames_used;

	uint64_t saved_target = _target_position;
	uint64_t generated    = _src_data.output_frames_gen;

	while (generated < cnt) {
		uint64_t g = read (dst + generated, _target_position, cnt - generated, 0);
		generated += g;
		if (g == 0) break;
	}

	_target_position = saved_target;

	return generated;
}

/* ****************************************************************************/

MemSource::MemSource ()
	: _n_channels (4)
	, _sample_rate (44100)
	, _len (16)
{
	_buf = new float[_n_channels * _len];
	memset (_buf, 0, _n_channels * _len * sizeof (float));
	//             // Stereo    Mono2Stereo     Mono
	_buf[0] = 1.0; // L -> L      M -> L       M -> M
	_buf[1] = 0.1; // L -> R      M -> R
	_buf[2] = 0.5; // R -> L
	_buf[3] = 0.3; // R -> R
}

MemSource::~MemSource ()
{
	delete[] _buf;
}

uint64_t
MemSource::read (float* dst, uint64_t pos, uint64_t cnt, uint32_t channel) const
{
	if (channel >= _n_channels) {
		return 0;
	}

	if (pos >= _len) {
		return 0;
	} else if (pos + cnt > _len) {
		cnt = _len - pos;
	}

	if (_n_channels == 1) {
		memcpy (dst, &_buf[pos], cnt);
	} else {
		pos += channel;
		for (uint64_t i = 0; i < cnt; ++i, pos += _n_channels) {
			dst[i] = _buf[pos];
		}
	}
	return cnt;
}

/* ****************************************************************************/

SFSource::SFSource ()
{
	memset (&_info, 0, sizeof (_info));
}

SFSource::~SFSource ()
{
	if (_sndfile) {
		sf_close (_sndfile);
	}
}

uint64_t
SFSource::read (float* dst, uint64_t pos, uint64_t cnt, uint32_t channel) const
{
	if (!_sndfile) {
		return 0;
	}

	uint64_t length = readable_length ();

	if (pos >= length) {
		return 0;
	} else if (pos + cnt > length) {
		cnt = length - pos;
	}

	if (sf_seek (_sndfile, (sf_count_t)pos, SEEK_SET | SFM_READ) != (sf_count_t)pos) {
		return 0;
	}

	if (_info.channels == 1) {
		return sf_read_float (_sndfile, dst, cnt);
	}

	uint32_t interleave_buffer_size = cnt * _info.channels;
	float*   tmp                    = new float[interleave_buffer_size];

	int64_t nread = sf_read_float (_sndfile, tmp, interleave_buffer_size);

	float* ptr = tmp + channel;
	nread /= _info.channels;

	for (int64_t n = 0; n < nread; ++n) {
		dst[n] = *ptr;
		ptr += _info.channels;
	}

	delete[] tmp;
	return nread;
}

void
SFSource::post_init ()
{
	if (!_info.seekable) {
		sf_close (_sndfile);
		_sndfile = 0;
		memset (&_info, 0, sizeof (_info));
	}

	if (!_sndfile) {
		throw std::runtime_error ("Error: cannot open IR file");
	}

#ifndef NDEBUG
	printf ("SF rate: %d, n_chan: %d frames: %ld\n", _info.samplerate, _info.channels, _info.frames);
#endif
}

/* ****************************************************************************/

FileSource::FileSource (std::string const& path)
	: SFSource ()
{
	open (path);
	post_init ();
}

void
FileSource::open (std::string const& path)
{
	_sndfile = sf_open (path.c_str (), SFM_READ, &_info);
}

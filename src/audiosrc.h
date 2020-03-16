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

#include <stdexcept>

#include <samplerate.h>
#include <sndfile.h>

#define MINIMP3_NONSTANDARD_BUT_LOGICAL
#define MINIMP3_FLOAT_OUTPUT
#define MINIMP3_NO_SIMD // disable for portability

#include "readable.h"

namespace ZeroConvoLV2
{

/* use ZeroConvoLV2 namespace for minimp3 symbols */
#include "minimp3.h"

class SrcSource : public Readable
{
public:
	SrcSource (Readable*, uint32_t ratio);
	~SrcSource ();

	uint64_t read (float*, uint64_t pos, uint64_t cnt, uint32_t channel) const;

	uint64_t readable_length () const { return ceil (_source->readable_length () * _ratio) - 1; }
	uint32_t n_channels () const { return _source->n_channels (); }
	uint32_t sample_rate () const { return _target_rate; }

private:
	Readable* _source;
	uint32_t  _target_rate;
	double    _ratio;

	mutable SRC_STATE* _src_state;
	mutable SRC_DATA   _src_data;

	mutable float*   _src_buffer;
	mutable uint64_t _source_position;
	mutable uint64_t _target_position;
	mutable double   _fract_position;
};

class ChanWrap : public Readable
{
public:
	ChanWrap (Readable* r, uint32_t chn)
	    : _r (r)
	    , _chn (chn)
	{
		if (r->n_channels () < chn) {
			throw std::runtime_error ("ChanWrap: channel out of bounds");
		}
	}

	uint64_t
	read (float* dst, uint64_t pos, uint64_t cnt, uint32_t) const
	{
		return _r->read (dst, pos, cnt, _chn);
	}

	uint64_t readable_length () const { return _r->readable_length (); }
	uint32_t n_channels () const { return 1; }
	uint32_t sample_rate () const { return _r->sample_rate (); }

private:
	Readable* _r;
	uint32_t  _chn;
};

class MemSource : public Readable
{
public:
	MemSource ();
	~MemSource ();

	uint64_t read (float*, uint64_t pos, uint64_t cnt, uint32_t channel) const;
	uint64_t readable_length () const { return _len; }
	uint32_t n_channels () const { return _n_channels ; }
	virtual uint32_t sample_rate () const { return _sample_rate ; }

protected:
	uint32_t _n_channels;
	uint32_t _sample_rate;
	uint64_t _len;
	float*   _buf;
};

class SFSource : public Readable
{
public:
	SFSource ();
	virtual ~SFSource ();

	uint64_t read (float*, uint64_t pos, uint64_t cnt, uint32_t channel) const;

	uint64_t readable_length () const { return _info.frames; }
	uint32_t n_channels () const { return _info.channels; }
	uint32_t sample_rate () const { return _info.samplerate; }

protected:
	void post_init ();
	SNDFILE* _sndfile;
	SF_INFO  _info;
};

class FileSource : public SFSource
{
public:
	FileSource (std::string const& path);

private:
	void open (std::string const&);
};

class Mp3Source : public Readable
{
public:
	Mp3Source (std::string const&);
	~Mp3Source ();

	uint64_t read (float*, uint64_t pos, uint64_t cnt, uint32_t channel) const;
	uint64_t readable_length () const { return _len; }
	uint32_t n_channels () const { return _info.channels; }
	virtual uint32_t sample_rate () const { return _info.hz; }

private:
	void unmap_mem ();
	void seek (uint64_t);
	int  decode_mp3 (bool parse_only = false);

	uint64_t _read (float*, uint64_t pos, uint64_t cnt, uint32_t channel);

	mp3dec_t            _mp3d;
	mp3dec_frame_info_t _info;
	uint64_t            _len;

	int                 _fd;
	const uint8_t*      _map_addr;
	size_t              _map_length;

	const uint8_t*      _buffer;
	size_t              _remain;

	uint64_t            _read_position;
	mp3d_sample_t       _pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
	size_t              _pcm_off;
	int                 _n_frames;
};

}

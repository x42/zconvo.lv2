#pragma once

#include <stdint.h>

namespace ZeroConvoLV2 {

class Readable {
public:
	Readable () {}
	virtual ~Readable () {}

	virtual uint64_t read (float*, uint64_t pos, uint64_t cnt, uint32_t channel) const = 0;
	virtual uint64_t readable_length () const = 0;
	virtual uint32_t n_channels () const = 0;
	virtual uint32_t sample_rate () const = 0;
};

}

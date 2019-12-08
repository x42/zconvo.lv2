/* zeroconvolv -- Preset based LV2 convolution plugin
 *
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
#include <stdexcept>
#include <stdlib.h>

#include "convolver.h"

#include "lv2/lv2plug.in/ns/ext/atom/atom.h"
#include "lv2/lv2plug.in/ns/ext/urid/urid.h"
#include <lv2/lv2plug.in/ns/ext/buf-size/buf-size.h>
#include <lv2/lv2plug.in/ns/ext/log/log.h>
#include <lv2/lv2plug.in/ns/ext/log/logger.h>
#include <lv2/lv2plug.in/ns/ext/options/options.h>
#include <lv2/lv2plug.in/ns/ext/state/state.h>
#include <lv2/lv2plug.in/ns/ext/worker/worker.h>
#include <lv2/lv2plug.in/ns/lv2core/lv2.h>

#define ZC_PREFIX "http://gareus.org/oss/lv2/zeroconvolv#"

#define ZC_ir        ZC_PREFIX "ir"
#define ZC_gain      ZC_PREFIX "gain"
#define ZC_predelay  ZC_PREFIX "predelay"
#define ZC_chn_gain  ZC_PREFIX "channel_gain"
#define ZC_chn_delay ZC_PREFIX "channel_predelay"
#define ZC_sum_ins   ZC_PREFIX "sum_inputs"

#ifndef LV2_BUF_SIZE__nominalBlockLength
# define LV2_BUF_SIZE__nominalBlockLength "http://lv2plug.in/ns/ext/buf-size#nominalBlockLength"
#endif

enum {
	CMD_APPLY = 0,
	CMD_FREE  = 1,
};

typedef struct {
	LV2_URID_Map*        map;
	LV2_Worker_Schedule* schedule;

	LV2_Log_Log*   log;
	LV2_Log_Logger logger;

	float const* input[2];
	float*       output[2];
	float*       p_latency;

	LV2_URID atom_String;
	LV2_URID atom_Path;
	LV2_URID atom_Int;
	LV2_URID atom_Float;
	LV2_URID atom_Bool;
	LV2_URID atom_Vector;
	LV2_URID zc_chn_delay;
	LV2_URID zc_predelay;
	LV2_URID zc_chn_gain;
	LV2_URID zc_gain;
	LV2_URID zc_sum_ins;
	LV2_URID zc_ir;
	LV2_URID bufsz_len;

	ZeroConvoLV2::Convolver* clv_online;  ///< currently active engine
	ZeroConvoLV2::Convolver* clv_offline; ///< inactive engine being configured

	ZeroConvoLV2::Convolver::IRChannelConfig chn_cfg;

	int rate;    ///< sample-rate -- constant per instance
	int chn_in;  ///< input channel count -- constant per instance
	int chn_out; ///< output channel count --constant per instance

	uint32_t block_size;
	int      rt_policy;
	int      rt_priority;
} zeroConvolv;

typedef struct {
	uint32_t child_size;
	uint32_t child_type;
	union {
		float    f[4];
		uint32_t i[4];
	};
} stateVector;


static LV2_Handle
instantiate (const LV2_Descriptor*     descriptor,
             double                    rate,
             const char*               bundle_path,
             const LV2_Feature* const* features)
{
	const LV2_Options_Option* options  = NULL;
	LV2_URID_Map*             map      = NULL;
	LV2_Worker_Schedule*      schedule = NULL;
	LV2_Log_Log*              log      = NULL;

	for (int i = 0; features[i]; ++i) {
		if (!strcmp (features[i]->URI, LV2_URID__map)) {
			map = (LV2_URID_Map*)features[i]->data;
		} else if (!strcmp (features[i]->URI, LV2_WORKER__schedule)) {
			schedule = (LV2_Worker_Schedule*)features[i]->data;
		} else if (!strcmp (features[i]->URI, LV2_OPTIONS__options)) {
			options = (const LV2_Options_Option*)features[i]->data;
		} else if (!strcmp (features[i]->URI, LV2_LOG__log)) {
			log = (LV2_Log_Log*)features[i]->data;
		}
	}

	// Initialise logger (if map is unavailable, will fallback to printf)
	LV2_Log_Logger logger;
	lv2_log_logger_init (&logger, map, log);

	if (!map) {
		lv2_log_error (&logger, "ZConvolv: Missing feature uri:map\n");
		return NULL;
	} else if (!schedule) {
		lv2_log_error (&logger, "ZConvolv: Missing feature work:schedule\n");
		return NULL;
	} else if (!options) {
		lv2_log_error (&logger, "ZConvolv: Missing options\n");
		return NULL;
	}

	LV2_URID bufsz_max = map->map (map->handle, LV2_BUF_SIZE__maxBlockLength);
	LV2_URID bufsz_len = map->map (map->handle, LV2_BUF_SIZE__nominalBlockLength);
	LV2_URID tshed_pol = map->map (map->handle, "http://ardour.org/lv2/threads/#schedPolicy");
	LV2_URID tshed_pri = map->map (map->handle, "http://ardour.org/lv2/threads/#schedPriority");
	LV2_URID atom_Int  = map->map (map->handle, LV2_ATOM__Int);

	uint32_t max_block   = 0;
	uint32_t block_size  = 0;
	uint32_t rt_priority = 0;
#ifdef _WIN32
	uint32_t rt_policy = SCHED_OTHER;
#else
	uint32_t rt_policy = SCHED_FIFO;
#endif

	for (const LV2_Options_Option* o = options; o->key; ++o) {
		if (o->context == LV2_OPTIONS_INSTANCE &&
		    o->key == bufsz_len &&
		    o->type == atom_Int) {
			block_size = *(const int32_t*)o->value;
		}
		if (o->context == LV2_OPTIONS_INSTANCE &&
		    o->key == bufsz_max &&
		    o->type == atom_Int) {
			max_block = *(const int32_t*)o->value;
		}
		if (o->context == LV2_OPTIONS_INSTANCE &&
		    o->key == tshed_pol &&
		    o->type == atom_Int) {
			rt_policy = *(const int32_t*)o->value;
		}
		if (o->context == LV2_OPTIONS_INSTANCE &&
		    o->key == tshed_pri &&
		    o->type == atom_Int) {
			rt_priority = *(const int32_t*)o->value;
		}
	}

	if (block_size == 0 && max_block == 0) {
		lv2_log_error (&logger, "ZConvolv: No nominal nor max block-size given\n");
		return NULL;
	}

	if (block_size == 0) {
		lv2_log_warning (&logger, "ZConvolv: No nominal block-size given, using max block-size\n");
		block_size = max_block;
	}
	if (block_size > 8192) {
		lv2_log_error (&logger, "Buffer size %u out of range (max. 8192)\n", block_size);
		return NULL;
	}
	if (block_size < 64) {
		lv2_log_note (&logger, "Buffer size %u is too small, using 64.\n", block_size);
		block_size = 64;
	}

	if (rt_priority == 0) {
		const int p_min = sched_get_priority_min (rt_policy);
		const int p_max = sched_get_priority_max (rt_policy);
		rt_priority     = (p_min + p_max) * .5;
		lv2_log_note (&logger, "ZConvolv: Using default rt-priority: %d\n", rt_priority);
	} else {
		/* note: zita-convolver enforces min/max range */
		lv2_log_note (&logger, "ZConvolv: Using rt-priority: %d\n", rt_priority);
	}

	lv2_log_trace (&logger, "ZConvolv: Buffer size: %u\n", block_size);

	zeroConvolv* self = (zeroConvolv*)calloc (1, sizeof (zeroConvolv));
	if (!self) {
		return NULL;
	}

	if (!strcmp (descriptor->URI, ZC_PREFIX "Mono")) {
		self->chn_in  = 1;
		self->chn_out = 1;
		self->chn_cfg = ZeroConvoLV2::Convolver::Mono;
	} else if (!strcmp (descriptor->URI, ZC_PREFIX "Stereo")) {
		self->chn_in  = 2;
		self->chn_out = 2;
		self->chn_cfg = ZeroConvoLV2::Convolver::Stereo;
	} else if (!strcmp (descriptor->URI, ZC_PREFIX "MonoToStereo")) {
		self->chn_in  = 1;
		self->chn_out = 2;
		self->chn_cfg = ZeroConvoLV2::Convolver::MonoToStereo;
	} else {
		lv2_log_error (&logger, "ZConvolv: Invalid URI\n");
		free (self);
		return NULL;
	}

	self->map         = map;
	self->schedule    = schedule;
	self->log         = log;
	self->logger      = logger;
	self->block_size  = block_size;
	self->rt_policy   = rt_policy;
	self->rt_priority = rt_priority;

	self->rate = rate;

	self->clv_online  = NULL;
	self->clv_offline = NULL;

	self->atom_String  = map->map (map->handle, LV2_ATOM__String);
	self->atom_Path    = map->map (map->handle, LV2_ATOM__Path);
	self->atom_Int     = map->map (map->handle, LV2_ATOM__Int);
	self->atom_Float   = map->map (map->handle, LV2_ATOM__Float);
	self->atom_Bool    = map->map (map->handle, LV2_ATOM__Bool);
	self->atom_Vector  = map->map (map->handle, LV2_ATOM__Vector);
	self->zc_chn_delay = map->map (map->handle, ZC_chn_delay);
	self->zc_predelay  = map->map (map->handle, ZC_predelay);
	self->zc_chn_gain  = map->map (map->handle, ZC_chn_gain);
	self->zc_gain      = map->map (map->handle, ZC_gain);
	self->zc_sum_ins   = map->map (map->handle, ZC_sum_ins);
	self->zc_ir        = map->map (map->handle, ZC_ir);
	self->bufsz_len    = map->map (map->handle, LV2_BUF_SIZE__nominalBlockLength);

	return (LV2_Handle)self;
}

static void
connect_port (LV2_Handle instance,
              uint32_t   port,
              void*      data)
{
	zeroConvolv* self = (zeroConvolv*)instance;

	switch (port) {
		case 0:
			self->p_latency = (float*)data;
			break;
		case 1:
			self->output[0] = (float*)data;
			break;
		case 3:
			self->output[1] = (float*)data;
			break;
		case 2:
			self->input[0] = (const float*)data;
			break;
		case 4:
			self->input[1] = (const float*)data;
			break;
		default:
			break;
	}
}

static void
activate (LV2_Handle instance)
{
	zeroConvolv* self = (zeroConvolv*)instance;
	if (self->clv_online) {
		self->clv_online->reconfigure (self->block_size);
	}
}

static inline void
copy_no_inplace_buffers (float* out, float const* in, uint32_t n_samples)
{
	if (out == in) {
		return;
	}
	memcpy (out, in, sizeof (float) * n_samples);
}

static void
run (LV2_Handle instance, uint32_t n_samples)
{
	zeroConvolv* self = (zeroConvolv*)instance;

	if (!self->clv_online) {
		*self->p_latency = 0;
		for (int i = 0; i < self->chn_out; i++) {
			memset (self->output[i], 0, sizeof (float) * n_samples);
		}
		return;
	}

	assert (self->clv_online->ready ());
	*self->p_latency = self->clv_online->latency ();

	copy_no_inplace_buffers (self->output[0], self->input[0], n_samples);

	if (self->chn_in == 2) {
		assert (self->chn_out == 2);
		if (self->clv_online->sum_inputs ()) {
			/* fake stereo, sum inputs to mono */
			for (uint32_t i = 0; i < n_samples; ++i) {
				self->output[0][i] = 0.5 * (self->output[0][i] + self->input[1][i]);
			}
			memcpy (self->output[1], self->output[0], sizeof (float) * n_samples);
		} else {
			copy_no_inplace_buffers (self->output[1], self->input[1], n_samples);
		}
		self->clv_online->run_stereo (self->output[0], self->output[1], n_samples);
	} else if (self->chn_out == 2) {
		assert (self->chn_in == 1);
		self->clv_online->run_stereo (self->output[0], self->output[1], n_samples);
	} else {
		assert (self->chn_in == 1);
		assert (self->chn_out == 1);
		self->clv_online->run (self->output[0], n_samples);
	}
}

static void
cleanup (LV2_Handle instance)
{
	zeroConvolv* self = (zeroConvolv*)instance;
	delete self->clv_online;
	delete self->clv_offline;
	free (instance);
}

static LV2_Worker_Status
work_response (LV2_Handle  instance,
               uint32_t    size,
               const void* data)
{
	zeroConvolv* self = (zeroConvolv*)instance;

	if (!self->clv_offline) {
		return LV2_WORKER_SUCCESS;
	}

	/* swap engine instances */
	ZeroConvoLV2::Convolver* old = self->clv_online;

	self->clv_online  = self->clv_offline;
	self->clv_offline = old;

	assert (self->clv_online != self->clv_offline || self->clv_online == NULL);

	int d = CMD_FREE;
	self->schedule->schedule_work (self->schedule->handle, sizeof (int), &d);
	return LV2_WORKER_SUCCESS;
}

static LV2_Worker_Status
work (LV2_Handle                  instance,
      LV2_Worker_Respond_Function respond,
      LV2_Worker_Respond_Handle   handle,
      uint32_t                    size,
      const void*                 data)
{
	zeroConvolv* self = (zeroConvolv*)instance;

	if (size != sizeof (int)) {
		return LV2_WORKER_ERR_UNKNOWN;
	}

	switch (*((const int*)data)) {
		case CMD_APPLY:
			respond (handle, 1, "");
			break;
		case CMD_FREE:
			delete self->clv_offline;
			self->clv_offline = 0;
			break;
		default:
			break;
	}
	return LV2_WORKER_SUCCESS;
}

static LV2_State_Status
save (LV2_Handle                instance,
      LV2_State_Store_Function  store,
      LV2_State_Handle          handle,
      uint32_t                  flags,
      const LV2_Feature* const* features)
{
	zeroConvolv* self = (zeroConvolv*)instance;

	LV2_State_Map_Path*  map_path = NULL;
#ifdef LV2_STATE__freePath
	LV2_State_Free_Path* free_path = NULL;
#endif

	for (int i = 0; features[i]; ++i) {
		if (!strcmp (features[i]->URI, LV2_STATE__mapPath)) {
			map_path = (LV2_State_Map_Path*)features[i]->data;
		}
#ifdef LV2_STATE__freePath
		else if (!strcmp(features[i]->URI, LV2_STATE__freePath)) {
			free_path = (LV2_State_Free_Path*)features[i]->data;
		}
#endif
	}

	if (!map_path) {
		return LV2_STATE_ERR_NO_FEATURE;
	}
	if (!self->clv_online) {
		/* no state to save */
		return LV2_STATE_SUCCESS;
	}

	char* apath = map_path->abstract_path (map_path->handle, self->clv_online->path ().c_str ());
	store (handle, self->zc_ir, apath, strlen (apath) + 1, self->atom_Path, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
#ifdef LV2_STATE__freePath
	if (free_path) {
		free_path->free_path (free_path->handle, apath);
	} else
#endif
#ifndef _WIN32 // https://github.com/drobilla/lilv/issues/14
	{
		free (apath);
	}
#endif

	ZeroConvoLV2::Convolver::IRSettings const& irs (self->clv_online->settings ());

	store (handle, self->zc_gain, &irs.gain, sizeof (float), self->atom_Float,
	       LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);

	store (handle, self->zc_predelay, &irs.pre_delay, sizeof (uint32_t), self->atom_Int,
	       LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);

	int32_t lv2bool = irs.sum_inputs ? 1 : 0;
	store (handle, self->zc_sum_ins, &lv2bool, sizeof (int32_t), self->atom_Bool,
	       LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);

	stateVector sv;

	sv.child_type = self->atom_Float;
	sv.child_size = sizeof (float);
	memcpy (sv.f, irs.channel_gain, sizeof (irs.channel_gain));
	store (handle, self->zc_chn_gain, (void*)&sv, sizeof (sv),
	       self->atom_Vector, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);

	sv.child_type = self->atom_Int;
	sv.child_size = sizeof (uint32_t);
	memcpy (sv.i, irs.channel_delay, sizeof (irs.channel_delay));
	store (handle, self->zc_chn_delay, (void*)&sv, sizeof (sv),
	       self->atom_Vector, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);

	return LV2_STATE_SUCCESS;
}

static LV2_State_Status
restore (LV2_Handle                  instance,
         LV2_State_Retrieve_Function retrieve,
         LV2_State_Handle            handle,
         uint32_t                    flags,
         const LV2_Feature* const*   features)
{
	zeroConvolv* self = (zeroConvolv*)instance;
	size_t       size;
	uint32_t     type;
	uint32_t     valflags;

	/* Get the work scheduler provided to restore() (state:threadSafeRestore
	 * support), but fall back to instantiate() schedules (spec-violating
	 * workaround for broken hosts). */
	LV2_Worker_Schedule* schedule = self->schedule;
	LV2_State_Map_Path*  map_path = NULL;
#ifdef LV2_STATE__freePath
	LV2_State_Free_Path* free_path = NULL;
#endif
	for (int i = 0; features[i]; ++i) {
		if (!strcmp (features[i]->URI, LV2_WORKER__schedule)) {
			lv2_log_note (&self->logger, "ZConvolv State: using thread-safe restore scheduler\n");
			schedule = (LV2_Worker_Schedule*)features[i]->data;
		} else if (!strcmp (features[i]->URI, LV2_STATE__mapPath)) {
			map_path = (LV2_State_Map_Path*)features[i]->data;
		}
#ifdef LV2_STATE__freePath
		else if (!strcmp(features[i]->URI, LV2_STATE__freePath)) {
			free_path = (LV2_State_Free_Path*)features[i]->data;
		}
#endif
	}
	if (!map_path) {
		return LV2_STATE_ERR_NO_FEATURE;
	}
	if (schedule == self->schedule) {
		lv2_log_warning (&self->logger, "ZConvolv State: using run() scheduler to restore\n");
	}

	if (self->clv_offline) {
		lv2_log_warning (&self->logger, "ZConvolv State: offline instance in-use, state ignored.\n");
		return LV2_STATE_ERR_UNKNOWN;
	}

	bool ok = false;
	const void* value;
	ZeroConvoLV2::Convolver::IRSettings irs;

	value = retrieve (handle, self->zc_predelay, &size, &type, &valflags);
	if (value && size == sizeof (int32_t) && type == self->atom_Int) {
		irs.pre_delay = *((int32_t*)value);
	}

	value = retrieve (handle, self->zc_gain, &size, &type, &valflags);
	if (value && size == sizeof (int32_t) && type == self->atom_Float) {
		irs.gain = *((float*)value);
	}

	value = retrieve (handle, self->zc_chn_delay, &size, &type, &valflags);
	if (value && size == sizeof (LV2_Atom) + sizeof (irs.channel_delay) && type == self->atom_Vector) {
		if (((LV2_Atom*)value)->type == self->atom_Int) {
			memcpy (irs.channel_delay, LV2_ATOM_BODY (value), sizeof (irs.channel_delay));
		}
	}

	value = retrieve (handle, self->zc_sum_ins, &size, &type, &valflags);
	if (value && size == sizeof (int32_t) && type == self->atom_Bool) {
		irs.sum_inputs = *((int32_t*)value) ? true : false;
	}

	value = retrieve (handle, self->zc_chn_gain, &size, &type, &valflags);
	if (value && size == sizeof (LV2_Atom) + sizeof (irs.channel_gain) && type == self->atom_Vector) {
		if (((LV2_Atom*)value)->type == self->atom_Float) {
			memcpy (irs.channel_gain, LV2_ATOM_BODY (value), sizeof (irs.channel_gain));
		}
	}

	value = retrieve (handle, self->zc_ir, &size, &type, &valflags);

	if (value) {
		char* path = map_path->absolute_path (map_path->handle, (const char*)value);
		lv2_log_note (&self->logger, "ZConvolv State: ir=%s\n", path);
		try {
			self->clv_offline = new ZeroConvoLV2::Convolver (path, self->rate, self->rt_policy, self->rt_priority, self->chn_cfg, irs);
			self->clv_offline->reconfigure (self->block_size);
			ok = self->clv_offline->ready ();
		} catch (std::runtime_error& err) {
			lv2_log_warning (&self->logger, "ZConvolv Convolver: %s.\n", err.what ());
		}
#ifdef LV2_STATE__freePath
		if (free_path) {
			free_path->free_path (free_path->handle, path);
		} else
#endif
#ifndef _WIN32 // https://github.com/drobilla/lilv/issues/14
		{
			free (path);
		}
#endif
	}

	if (!ok) {
		//lv2_log_note (&self->logger, "ZConvolv State: configuration failed.\n");
		delete self->clv_offline;
		self->clv_offline = 0;
		return LV2_STATE_ERR_NO_PROPERTY;
	} else {
		int d = CMD_APPLY;
		schedule->schedule_work (self->schedule->handle, sizeof (int), &d);
		return LV2_STATE_SUCCESS;
	}
}

static uint32_t
opts_get (LV2_Handle instance, LV2_Options_Option* options)
{
	return 0;
}

static uint32_t
opts_set (LV2_Handle instance, const LV2_Options_Option* options)
{
	zeroConvolv* self = (zeroConvolv*)instance;
	if (options->context != LV2_OPTIONS_INSTANCE || options->subject != 0) {
		return LV2_OPTIONS_ERR_BAD_SUBJECT;
	}
	if (options->key != self->bufsz_len) {
		return LV2_OPTIONS_ERR_BAD_KEY;
	}
	if (options->size != sizeof (int32_t) || options->type != self->atom_Int) {
		return LV2_OPTIONS_ERR_BAD_VALUE;
	}

	self->block_size = *((int32_t*)options->value);
	if (self->clv_online) {
		self->clv_online->reconfigure (self->block_size);
	}
	return LV2_OPTIONS_SUCCESS;
}

static const void*
extension_data (const char* uri)
{
	static const LV2_Worker_Interface  worker = {work, work_response, NULL};
	static const LV2_State_Interface   state  = {save, restore};
	static const LV2_Options_Interface opts   = {opts_get, opts_set};

	if (!strcmp (uri, LV2_WORKER__interface)) {
		return &worker;
	} else if (!strcmp (uri, LV2_STATE__interface)) {
		return &state;
	} else if (!strcmp (uri, LV2_OPTIONS__interface)) {
		return &opts;
	}
	return NULL;
}

static const LV2_Descriptor descriptor0 = {
    ZC_PREFIX "Mono",
    instantiate,
    connect_port,
    activate,
    run,
    NULL, // deactivate,
    cleanup,
    extension_data};

static const LV2_Descriptor descriptor1 = {
    ZC_PREFIX "Stereo",
    instantiate,
    connect_port,
    activate,
    run,
    NULL, // deactivate,
    cleanup,
    extension_data};

static const LV2_Descriptor descriptor2 = {
    ZC_PREFIX "MonoToStereo",
    instantiate,
    connect_port,
    activate,
    run,
    NULL, // deactivate,
    cleanup,
    extension_data};

#undef LV2_SYMBOL_EXPORT
#ifdef _WIN32
# define LV2_SYMBOL_EXPORT __declspec(dllexport)
#else
# define LV2_SYMBOL_EXPORT __attribute__ ((visibility ("default")))
#endif
LV2_SYMBOL_EXPORT
const LV2_Descriptor*
lv2_descriptor (uint32_t index)
{
	switch (index) {
		case 0:
			return &descriptor0;
		case 1:
			return &descriptor1;
		case 2:
			return &descriptor2;
		default:
			return NULL;
	}
}

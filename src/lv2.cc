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
#include <math.h>
#include <pthread.h>
#include <stdexcept>
#include <stdlib.h>

#include <string>

#include "convolver.h"

#include <lv2/lv2plug.in/ns/ext/atom/atom.h>
#include <lv2/lv2plug.in/ns/ext/atom/forge.h>
#include <lv2/lv2plug.in/ns/ext/buf-size/buf-size.h>
#include <lv2/lv2plug.in/ns/ext/log/log.h>
#include <lv2/lv2plug.in/ns/ext/log/logger.h>
#include <lv2/lv2plug.in/ns/ext/options/options.h>
#include <lv2/lv2plug.in/ns/ext/patch/patch.h>
#include <lv2/lv2plug.in/ns/ext/state/state.h>
#include <lv2/lv2plug.in/ns/ext/urid/urid.h>
#include <lv2/lv2plug.in/ns/ext/worker/worker.h>
#include <lv2/lv2plug.in/ns/lv2core/lv2.h>

#define ZC_PREFIX "http://gareus.org/oss/lv2/zeroconvolv#"

/* clang-format off */
#define ZC_ir        ZC_PREFIX "ir"
#define ZC_gain      ZC_PREFIX "gain"
#define ZC_predelay  ZC_PREFIX "predelay"
#define ZC_latency   ZC_PREFIX "artificial_latency"
#define ZC_chn_gain  ZC_PREFIX "channel_gain"
#define ZC_chn_delay ZC_PREFIX "channel_predelay"
#define ZC_sum_ins   ZC_PREFIX "sum_inputs"

#ifndef LV2_BUF_SIZE__nominalBlockLength
# define LV2_BUF_SIZE__nominalBlockLength "http://lv2plug.in/ns/ext/buf-size#nominalBlockLength"
#endif

#ifndef LV2_STATE__StateChanged
# define LV2_STATE__StateChanged "http://lv2plug.in/ns/ext/state#StateChanged"
#endif

#ifdef HAVE_LV2_1_8
# define x_forge_object lv2_atom_forge_object
#else
# define x_forge_object lv2_atom_forge_blank
#endif
/* clang-format on */

#ifdef WITH_STATIC_FFTW_CLEANUP
static pthread_mutex_t instance_count_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned int    instance_count      = 0;
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

	/* ports */
	float const* input[2];
	float*       output[2];
	float*       p_latency;
	float*       p_ctrl[3];

	/* settings */
	bool  buffered;
	float db_dry;
	float db_wet;

	float dry_coeff;
	float dry_target;

	/* cfg ports */
	LV2_Atom_Forge           forge;
	LV2_Atom_Forge_Frame     frame;
	const LV2_Atom_Sequence* control;
	LV2_Atom_Sequence*       notify;

	LV2_URID atom_Blank;
	LV2_URID atom_Object;
	LV2_URID atom_String;
	LV2_URID atom_Path;
	LV2_URID atom_URID;
	LV2_URID atom_Int;
	LV2_URID atom_Float;
	LV2_URID atom_Bool;
	LV2_URID atom_Vector;
	LV2_URID bufsz_len;
	LV2_URID patch_Get;
	LV2_URID patch_Set;
	LV2_URID patch_property;
	LV2_URID patch_value;
	LV2_URID state_Changed;
	LV2_URID zc_chn_delay;
	LV2_URID zc_predelay;
	LV2_URID zc_latency;
	LV2_URID zc_chn_gain;
	LV2_URID zc_gain;
	LV2_URID zc_sum_ins;
	LV2_URID zc_ir;

	ZeroConvoLV2::Convolver* clv_online;  ///< currently active engine
	ZeroConvoLV2::Convolver* clv_offline; ///< inactive engine being configured

	bool pset_dirty; // unset before scheduling work for state-restore.

	pthread_mutex_t state_lock;

	/* configuration */
	ZeroConvoLV2::Convolver::IRChannelConfig chn_cfg;

	int rate;    ///< sample-rate -- constant per instance
	int chn_in;  ///< input channel count -- constant per instance
	int chn_out; ///< output channel count --constant per instance

	uint32_t block_size;
	int      rt_policy;
	int      rt_priority;
	float    tc64;

	/* next IR file to load, acting as queue */
	std::string next_queued_file;
} zeroConvolv;

typedef struct {
	uint32_t child_size;
	uint32_t child_type;
	union {
		float    f[4];
		uint32_t i[4];
	};
} stateVector;

static void  inform_ui (zeroConvolv* self, bool mark_dirty);
static float db_to_coeff (float db);

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
	} else if (!strcmp (descriptor->URI, ZC_PREFIX "CfgMono")) {
		self->chn_in  = 1;
		self->chn_out = 1;
		self->chn_cfg = ZeroConvoLV2::Convolver::Mono;
	} else if (!strcmp (descriptor->URI, ZC_PREFIX "Stereo")) {
		self->chn_in  = 2;
		self->chn_out = 2;
		self->chn_cfg = ZeroConvoLV2::Convolver::Stereo;
	} else if (!strcmp (descriptor->URI, ZC_PREFIX "CfgStereo")) {
		self->chn_in  = 2;
		self->chn_out = 2;
		self->chn_cfg = ZeroConvoLV2::Convolver::Stereo;
	} else if (!strcmp (descriptor->URI, ZC_PREFIX "MonoToStereo")) {
		self->chn_in  = 1;
		self->chn_out = 2;
		self->chn_cfg = ZeroConvoLV2::Convolver::MonoToStereo;
	} else if (!strcmp (descriptor->URI, ZC_PREFIX "CfgMonoToStereo")) {
		self->chn_in  = 1;
		self->chn_out = 2;
		self->chn_cfg = ZeroConvoLV2::Convolver::MonoToStereo;
	} else {
		lv2_log_error (&logger, "ZConvolv: Invalid URI\n");
		free (self);
		return NULL;
	}

	pthread_mutex_init (&self->state_lock, NULL);

	self->map         = map;
	self->schedule    = schedule;
	self->log         = log;
	self->logger      = logger;
	self->block_size  = block_size;
	self->rt_policy   = rt_policy;
	self->rt_priority = rt_priority;
	self->rate        = rate;
	self->buffered    = true;
	self->db_wet      = 0.f;
	self->db_dry      = -60.f;
	self->dry_coeff   = 0.f;
	self->dry_target  = 0.f;
	self->tc64        = 2950.f / rate; // ~20Hz for 90%
	self->pset_dirty  = true;

	lv2_atom_forge_init (&self->forge, map);

	self->atom_Blank     = map->map (map->handle, LV2_ATOM__Blank);
	self->atom_Object    = map->map (map->handle, LV2_ATOM__Object);
	self->atom_String    = map->map (map->handle, LV2_ATOM__String);
	self->atom_Path      = map->map (map->handle, LV2_ATOM__Path);
	self->atom_URID      = map->map (map->handle, LV2_ATOM__URID);
	self->atom_Int       = map->map (map->handle, LV2_ATOM__Int);
	self->atom_Float     = map->map (map->handle, LV2_ATOM__Float);
	self->atom_Bool      = map->map (map->handle, LV2_ATOM__Bool);
	self->atom_Vector    = map->map (map->handle, LV2_ATOM__Vector);
	self->bufsz_len      = map->map (map->handle, LV2_BUF_SIZE__nominalBlockLength);
	self->patch_Get      = map->map (map->handle, LV2_PATCH__Get);
	self->patch_Set      = map->map (map->handle, LV2_PATCH__Set);
	self->patch_property = map->map (map->handle, LV2_PATCH__property);
	self->patch_value    = map->map (map->handle, LV2_PATCH__value);
	self->state_Changed  = map->map (map->handle, LV2_STATE__StateChanged);
	self->zc_chn_delay   = map->map (map->handle, ZC_chn_delay);
	self->zc_predelay    = map->map (map->handle, ZC_predelay);
	self->zc_latency     = map->map (map->handle, ZC_latency);
	self->zc_chn_gain    = map->map (map->handle, ZC_chn_gain);
	self->zc_gain        = map->map (map->handle, ZC_gain);
	self->zc_sum_ins     = map->map (map->handle, ZC_sum_ins);
	self->zc_ir          = map->map (map->handle, ZC_ir);

#ifdef WITH_STATIC_FFTW_CLEANUP
	pthread_mutex_lock (&instance_count_lock);
	++instance_count;
	pthread_mutex_unlock (&instance_count_lock);
#endif

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

	const bool buffered = self->buffered;

	assert (self->clv_online->ready ());
	*self->p_latency = self->clv_online->artificial_latency () + (buffered ? self->clv_online->latency () : 0);

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
		if (buffered) {
			self->clv_online->run_buffered_stereo (self->output[0], self->output[1], n_samples);
		} else {
			self->clv_online->run_stereo (self->output[0], self->output[1], n_samples);
		}
	} else if (self->chn_out == 2) {
		assert (self->chn_in == 1);
		if (buffered) {
			self->clv_online->run_buffered_stereo (self->output[0], self->output[1], n_samples);
		} else {
			self->clv_online->run_stereo (self->output[0], self->output[1], n_samples);
		}
	} else {
		assert (self->chn_in == 1);
		assert (self->chn_out == 1);
		if (buffered) {
			self->clv_online->run_buffered_mono (self->output[0], n_samples);
		} else {
			self->clv_online->run_mono (self->output[0], n_samples);
		}
	}
}

static void
cleanup (LV2_Handle instance)
{
	zeroConvolv* self = (zeroConvolv*)instance;
	delete self->clv_online;
	delete self->clv_offline;
	pthread_mutex_destroy (&self->state_lock);

#ifdef WITH_STATIC_FFTW_CLEANUP
	pthread_mutex_lock (&instance_count_lock);
	if (instance_count > 0) {
		--instance_count;
	}
	/* use this only when statically linking to a local fftw!
	 *
	 * "After calling fftw_cleanup, all existing plans become undefined,
	 *  and you should not attempt to execute them nor to destroy them."
	 * [http://www.fftw.org/fftw3_doc/Using-Plans.html]
	 *
	 * If libfftwf is shared with other plugins or the host this can
	 * cause undefined behavior.
	 */
	if (instance_count == 0) {
		fftwf_cleanup ();
	}

	pthread_mutex_unlock (&instance_count_lock);
#endif

	free (instance);
}

static LV2_Worker_Status
work_response (LV2_Handle  instance,
               uint32_t    size,
               const void* data)
{
	zeroConvolv* self = (zeroConvolv*)instance;

	if (!self->clv_offline) {
		/* If loading an IR file fails (NULL == clv_offline),
		 * there may still be a file in the queue. A "Free"
		 * command will trigger processing the queue.
		 */
		if (!self->next_queued_file.empty ()) {
			uint32_t d = CMD_FREE;
			self->schedule->schedule_work (self->schedule->handle, sizeof (uint32_t), &d);
		}
		return LV2_WORKER_SUCCESS;
	}

	/* swap engine instances */
	ZeroConvoLV2::Convolver* old = self->clv_online;

	self->clv_online  = self->clv_offline;
	self->clv_offline = old;

	/* set gain coefficients for new instance */
	self->clv_online->set_output_gain (db_to_coeff (self->db_dry), db_to_coeff (self->db_wet), false);

	assert (self->clv_online != self->clv_offline || self->clv_online == NULL);

	inform_ui (self, self->pset_dirty);

	self->pset_dirty = true;

	uint32_t d = CMD_FREE;
	self->schedule->schedule_work (self->schedule->handle, sizeof (uint32_t), &d);
	return LV2_WORKER_SUCCESS;
}

static LV2_Worker_Status
load_ir_worker (zeroConvolv*                self,
                LV2_Worker_Respond_Function respond,
                LV2_Worker_Respond_Handle   handle,
                std::string const&          ir_path,
                bool&                       ok)
{
	pthread_mutex_lock (&self->state_lock);
	ok = false;

	if (self->clv_offline) {
		self->next_queued_file = ir_path;
		pthread_mutex_unlock (&self->state_lock);
		lv2_log_note (&self->logger, "ZConvolv Work: queueing for later: ir=%s\n", ir_path.c_str ());
		return LV2_WORKER_SUCCESS;
	}

	lv2_log_note (&self->logger, "ZConvolv opening: ir=%s\n", ir_path.c_str ());

	try {
		self->clv_offline = new ZeroConvoLV2::Convolver (ir_path, self->rate, self->rt_policy, self->rt_priority, self->chn_cfg);
		self->clv_offline->reconfigure (self->block_size);
		if (!(ok = self->clv_offline->ready ())) {
			delete self->clv_offline;
			self->clv_offline = NULL;
		}
	} catch (std::runtime_error& err) {
		lv2_log_warning (&self->logger, "ZConvolv Convolver: %s.\n", err.what ());
		self->clv_offline = NULL;
	}

	pthread_mutex_unlock (&self->state_lock);

	if (!ok) {
		lv2_log_note (&self->logger, "ZConvolv Load: configuration failed.\n");
		return LV2_WORKER_ERR_UNKNOWN;
	} else if (respond) {
		respond (handle, 1, "");
	}
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
	bool         unused;

	if (size == sizeof (uint32_t)) {
		switch (*((const uint32_t*)data)) {
			case CMD_APPLY:
				respond (handle, 1, "");
				break;
			case CMD_FREE:
				{
					pthread_mutex_lock (&self->state_lock);
					delete self->clv_offline;
					self->clv_offline = NULL;

					std::string queue_file;
					self->next_queued_file.swap (queue_file);
					pthread_mutex_unlock (&self->state_lock);

					if (!queue_file.empty ()) {
						lv2_log_note (&self->logger, "ZConvolv process queue: ir=%s\n", queue_file.c_str ());
						return load_ir_worker (self, respond, handle, queue_file, unused);
					}
				}
				break;
			default:
				return LV2_WORKER_ERR_UNKNOWN;
				break;
		}
		return LV2_WORKER_SUCCESS;
	}

	const LV2_Atom* file_path = (const LV2_Atom*)data;
	const char*     fn        = (const char*)(file_path + 1);
	lv2_log_note (&self->logger, "ZConvolv request load: ir=%s\n", fn);

	return load_ir_worker (self, respond, handle, std::string (fn, file_path->size), unused);
}

static LV2_State_Status
save (LV2_Handle                instance,
      LV2_State_Store_Function  store,
      LV2_State_Handle          handle,
      uint32_t                  flags,
      const LV2_Feature* const* features)
{
	zeroConvolv* self = (zeroConvolv*)instance;

	LV2_State_Map_Path* map_path = NULL;
#ifdef LV2_STATE__freePath
	LV2_State_Free_Path* free_path = NULL;
#endif

	for (int i = 0; features[i]; ++i) {
		if (!strcmp (features[i]->URI, LV2_STATE__mapPath)) {
			map_path = (LV2_State_Map_Path*)features[i]->data;
		}
#ifdef LV2_STATE__freePath
		else if (!strcmp (features[i]->URI, LV2_STATE__freePath)) {
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
	{
#ifndef _WIN32 // https://github.com/drobilla/lilv/issues/14
		free (apath);
#endif
	}

	ZeroConvoLV2::Convolver::IRSettings const& irs (self->clv_online->settings ());

	store (handle, self->zc_gain, &irs.gain, sizeof (float), self->atom_Float,
	       LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);

	store (handle, self->zc_predelay, &irs.pre_delay, sizeof (int32_t), self->atom_Int,
	       LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);

	store (handle, self->zc_latency, &irs.artificial_latency, sizeof (int32_t), self->atom_Int,
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
	sv.child_size = sizeof (int32_t);
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
		else if (!strcmp (features[i]->URI, LV2_STATE__freePath)) {
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

	const void*                         value;
	ZeroConvoLV2::Convolver::IRSettings irs;

	value = retrieve (handle, self->zc_predelay, &size, &type, &valflags);
	if (value && size == sizeof (int32_t) && type == self->atom_Int) {
		irs.pre_delay = *((int32_t*)value);
	}

	value = retrieve (handle, self->zc_latency, &size, &type, &valflags);
	if (value && size == sizeof (int32_t) && type == self->atom_Int) {
		irs.artificial_latency = *((int32_t*)value);
	}

	value = retrieve (handle, self->zc_gain, &size, &type, &valflags);
	if (value && size == sizeof (float) && type == self->atom_Float) {
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
	if (!value) {
		return LV2_STATE_ERR_NO_PROPERTY;
	}

	char* path = map_path->absolute_path (map_path->handle, (const char*)value);
	lv2_log_note (&self->logger, "ZConvolv State: ir=%s\n", path);

	LV2_State_Status rv = LV2_STATE_SUCCESS;
	bool             ok = false;

	switch (load_ir_worker (self, NULL, NULL, path, ok)) {
		case LV2_WORKER_ERR_UNKNOWN:
			rv = LV2_STATE_ERR_NO_PROPERTY;
			break;
		default:
			break;
	}

#ifdef LV2_STATE__freePath
	if (free_path) {
		free_path->free_path (free_path->handle, path);
	} else
#endif
	{
#ifndef _WIN32 // https://github.com/drobilla/lilv/issues/14
		free (path);
#endif
	}

	if (!ok) {
		return rv;
	} else {
		self->pset_dirty = false;
		uint32_t d       = CMD_APPLY;
		schedule->schedule_work (self->schedule->handle, sizeof (uint32_t), &d);
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

/* ****************************************************************************/

static float
db_to_coeff (float db)
{
	if (db <= -60.f) {
		return 0;
	}
	if (db > 6.02f) {
		return 2;
	}
	return powf (10.f, .05f * db);
}

static void
inform_ui (zeroConvolv* self, bool mark_dirty)
{
	if (!self->control || !self->notify) {
		return;
	}
	if (!self->clv_online || self->clv_online->path ().empty ()) {
		return;
	}
	if (!self->next_queued_file.empty ()) {
		return;
	}

	const char* path = self->clv_online->path ().c_str ();

	LV2_Atom_Forge_Frame frame;
	lv2_atom_forge_frame_time (&self->forge, 0);
	x_forge_object (&self->forge, &frame, 1, self->patch_Set);
	lv2_atom_forge_property_head (&self->forge, self->patch_property, 0);
	lv2_atom_forge_urid (&self->forge, self->zc_ir);
	lv2_atom_forge_property_head (&self->forge, self->patch_value, 0);
	lv2_atom_forge_path (&self->forge, path, strlen (path));
	lv2_atom_forge_pop (&self->forge, &frame);

	if (mark_dirty) {
		lv2_atom_forge_frame_time (&self->forge, 0);
		x_forge_object (&self->forge, &frame, 1, self->state_Changed);
		lv2_atom_forge_pop (&self->forge, &frame);
	}
}

static const LV2_Atom*
parse_patch_msg (zeroConvolv* self, const LV2_Atom_Object* obj)
{
	const LV2_Atom* property  = NULL;
	const LV2_Atom* file_path = NULL;

	if (obj->body.otype != self->patch_Set) {
		return NULL;
	}

	lv2_atom_object_get (obj, self->patch_property, &property, 0);
	if (!property || property->type != self->atom_URID) {
		return NULL;
	} else if (((const LV2_Atom_URID*)property)->body != self->zc_ir) {
		return NULL;
	}

	lv2_atom_object_get (obj, self->patch_value, &file_path, 0);
	if (!file_path || file_path->type != self->atom_Path) {
		return NULL;
	}

	return file_path;
}

static void
connect_port_cfg (LV2_Handle instance,
                  uint32_t   port,
                  void*      data)
{
	zeroConvolv* self = (zeroConvolv*)instance;

	switch (port) {
		case 0:
			self->control = (const LV2_Atom_Sequence*)data;
			break;
		case 1:
			self->notify = (LV2_Atom_Sequence*)data;
			break;
		case 2:
		case 3:
		case 4:
			self->p_ctrl[port - 2] = (float*)data;
			break;
		default:
			connect_port (instance, port - 5, data);
			break;
	}
}

static void
run_cfg (LV2_Handle instance, uint32_t n_samples)
{
	zeroConvolv* self = (zeroConvolv*)instance;
	if (!self->control || !self->notify) {
		return;
	}

	const uint32_t capacity = self->notify->atom.size;
	lv2_atom_forge_set_buffer (&self->forge, (uint8_t*)self->notify, capacity);
	lv2_atom_forge_sequence_head (&self->forge, &self->frame, 0);

	LV2_ATOM_SEQUENCE_FOREACH (self->control, ev)
	{
		const LV2_Atom_Object* obj = (LV2_Atom_Object*)&ev->body;
		if (ev->body.type != self->atom_Blank && ev->body.type != self->atom_Object) {
			continue;
		}
		if (obj->body.otype == self->patch_Get) {
			inform_ui (self, false);
		} else if (obj->body.otype == self->patch_Set) {
			const LV2_Atom* file_path = parse_patch_msg (self, obj);
			if (!file_path || file_path->size < 1 || file_path->size > 1024) {
				continue;
			}
			self->schedule->schedule_work (self->schedule->handle, lv2_atom_total_size (file_path), file_path);
		}
	}

	self->buffered = *self->p_ctrl[0] > 0;

	float db_dry = *self->p_ctrl[1];
	float db_wet = *self->p_ctrl[2];

	if (self->db_dry != db_dry || self->db_wet != db_wet) {
		self->db_dry     = db_dry;
		self->db_wet     = db_wet;
		self->dry_target = db_to_coeff (db_dry);

		if (self->clv_online) {
			self->clv_online->set_output_gain (self->dry_target, db_to_coeff (db_wet));
			self->dry_coeff = self->dry_target; // assume convolver completes interpolation
		}
	}

	if (self->clv_online) {
		run (instance, n_samples);
		*self->p_latency = self->clv_online->artificial_latency ();
		return;
	}

	/* forward audio, apply gain */
	*self->p_latency = 0;

	copy_no_inplace_buffers (self->output[0], self->input[0], n_samples);

	if (self->chn_in == 2) {
		assert (self->chn_out == 2);
		copy_no_inplace_buffers (self->output[1], self->input[1], n_samples);
	} else if (self->chn_out == 2) {
		assert (self->chn_in == 1);
		copy_no_inplace_buffers (self->output[1], self->input[0], n_samples);
	}

	/* apply fixed gain */
	if (self->dry_coeff == self->dry_target) {
		if (self->dry_coeff == 1.f) {
			; /* relax */
		} else if (self->dry_coeff == 0.f) {
			for (int c = 0; c < self->chn_out; ++c) {
				memset (self->output[c], 0, sizeof (float) * n_samples);
			}
			return;
		} else {
			const float gain = self->dry_coeff;
			for (int c = 0; c < self->chn_out; ++c) {
				for (uint32_t i = 0; i < n_samples; ++i) {
					self->output[c][i] *= gain;
				}
			}
		}
		return;
	}

	/* interpolate gain */
	const float alpha  = self->tc64;
	uint32_t    remain = n_samples;
	uint32_t    done   = 0;
	float       cur    = self->dry_coeff;
	float       tgt    = self->dry_target;

	while (remain > 0) {
		uint32_t ns = std::min (remain, (uint32_t)64);
		cur += alpha * (tgt - cur) + 1e-10f;

		for (int c = 0; c < self->chn_out; ++c) {
			for (uint32_t i = 0; i < ns; ++i) {
				self->output[c][done + i] *= cur;
			}
		}
		remain -= ns;
		done   += ns;
	}

	if (fabsf (cur - tgt) < 1e-5f) {
		self->dry_coeff = self->dry_target;
	} else {
		self->dry_coeff = cur;
	}
}

/* ****************************************************************************/

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

static const LV2_Descriptor descriptor3 = {
    ZC_PREFIX "CfgMono",
    instantiate,
    connect_port_cfg,
    activate,
    run_cfg,
    NULL, // deactivate,
    cleanup,
    extension_data};

static const LV2_Descriptor descriptor4 = {
    ZC_PREFIX "CfgStereo",
    instantiate,
    connect_port_cfg,
    activate,
    run_cfg,
    NULL, // deactivate,
    cleanup,
    extension_data};

static const LV2_Descriptor descriptor5 = {
    ZC_PREFIX "CfgMonoToStereo",
    instantiate,
    connect_port_cfg,
    activate,
    run_cfg,
    NULL, // deactivate,
    cleanup,
    extension_data};

/* clang-format off */
#undef LV2_SYMBOL_EXPORT
#ifdef _WIN32
# define LV2_SYMBOL_EXPORT __declspec(dllexport)
#else
# define LV2_SYMBOL_EXPORT __attribute__ ((visibility ("default")))
#endif
/* clang-format on */
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
		case 3:
			return &descriptor3;
		case 4:
			return &descriptor4;
		case 5:
			return &descriptor5;
		default:
			return NULL;
	}
}

/*
 * $Id: pa_linux_alsa.c 1911 2013-10-17 12:44:09Z gineera $
 * PortAudio Portable Real-Time Audio Library
 * Latest Version at: http://www.portaudio.com
 * ALSA implementation by Joshua Haberman and Arve Knudsen
 *
 * Copyright (c) 2002 Joshua Haberman <joshua@haberman.com>
 * Copyright (c) 2005-2009 Arve Knudsen <arve.knudsen@gmail.com>
 * Copyright (c) 2008 Kevin Kofler <kevin.kofler@chello.at>
 *
 * Based on the Open Source API proposed by Ross Bencina
 * Copyright (c) 1999-2002 Ross Bencina, Phil Burk
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge,
 * publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
 * CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * The text above constitutes the entire PortAudio license; however,
 * the PortAudio community also makes the following non-binding requests:
 *
 * Any person wishing to distribute modifications to the Software is
 * requested to send the modifications to the original developer so that
 * they can be incorporated into the canonical version. It is also
 * requested that these non-binding requests be included along with the
 * license above.
 */

/**
 @file
 @ingroup hostapi_src
*/

#define ALSA_PCM_NEW_HW_PARAMS_API
#define ALSA_PCM_NEW_SW_PARAMS_API
#include <alsa/asoundlib.h>
#undef ALSA_PCM_NEW_HW_PARAMS_API
#undef ALSA_PCM_NEW_SW_PARAMS_API

#include <sys/poll.h>
#include <string.h> /* strlen() */
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <sys/mman.h>
#include <signal.h> /* For sig_atomic_t */
#ifdef PA_ALSA_DYNAMIC
    #include <dlfcn.h> /* For dlXXX functions */
#endif

#include "portaudio.h"
#include "pa_util.h"
#include "pa_unix_util.h"
#include "pa_allocation.h"
#include "pa_hostapi.h"
#include "pa_stream.h"
#include "pa_cpuload.h"
#include "pa_process.h"
#include "pa_endianness.h"
#include "pa_debugprint.h"

#include "pa_linux_alsa.h"

/* Add missing define (for compatibility with older ALSA versions) */
#ifndef SND_PCM_TSTAMP_ENABLE
    #define SND_PCM_TSTAMP_ENABLE SND_PCM_TSTAMP_MMAP
#endif

/* Combine version elements into a single (unsigned) integer */
#define ALSA_VERSION_INT(major, minor, subminor)  ((major << 16) | (minor << 8) | subminor)

/* The acceptable tolerance of sample rate set, to that requested (as a ratio, eg 50 is 2%, 100 is 1%) */
#define RATE_MAX_DEVIATE_RATIO 100

/* Defines Alsa function types and pointers to these functions. */
#define _PA_DEFINE_FUNC(x)  typedef typeof(x) x##_ft; static x##_ft *alsa_##x = 0

/* Alloca helper. */
#define __alsa_snd_alloca(ptr,type) do { size_t __alsa_alloca_size = alsa_##type##_sizeof(); (*ptr) = (type##_t *) alloca(__alsa_alloca_size); memset(*ptr, 0, __alsa_alloca_size); } while (0)

_PA_DEFINE_FUNC(snd_pcm_open);
_PA_DEFINE_FUNC(snd_pcm_close);
_PA_DEFINE_FUNC(snd_pcm_nonblock);
_PA_DEFINE_FUNC(snd_pcm_frames_to_bytes);
_PA_DEFINE_FUNC(snd_pcm_prepare);
_PA_DEFINE_FUNC(snd_pcm_start);
_PA_DEFINE_FUNC(snd_pcm_resume);
_PA_DEFINE_FUNC(snd_pcm_wait);
_PA_DEFINE_FUNC(snd_pcm_state);
_PA_DEFINE_FUNC(snd_pcm_avail_update);
_PA_DEFINE_FUNC(snd_pcm_areas_silence);
_PA_DEFINE_FUNC(snd_pcm_mmap_begin);
_PA_DEFINE_FUNC(snd_pcm_mmap_commit);
_PA_DEFINE_FUNC(snd_pcm_readi);
_PA_DEFINE_FUNC(snd_pcm_readn);
_PA_DEFINE_FUNC(snd_pcm_writei);
_PA_DEFINE_FUNC(snd_pcm_writen);
_PA_DEFINE_FUNC(snd_pcm_drain);
_PA_DEFINE_FUNC(snd_pcm_recover);
_PA_DEFINE_FUNC(snd_pcm_drop);
_PA_DEFINE_FUNC(snd_pcm_area_copy);
_PA_DEFINE_FUNC(snd_pcm_poll_descriptors);
_PA_DEFINE_FUNC(snd_pcm_poll_descriptors_count);
_PA_DEFINE_FUNC(snd_pcm_poll_descriptors_revents);
_PA_DEFINE_FUNC(snd_pcm_format_size);
_PA_DEFINE_FUNC(snd_pcm_link);
_PA_DEFINE_FUNC(snd_pcm_delay);

_PA_DEFINE_FUNC(snd_pcm_hw_params_sizeof);
_PA_DEFINE_FUNC(snd_pcm_hw_params_malloc);
_PA_DEFINE_FUNC(snd_pcm_hw_params_free);
_PA_DEFINE_FUNC(snd_pcm_hw_params_any);
_PA_DEFINE_FUNC(snd_pcm_hw_params_set_access);
_PA_DEFINE_FUNC(snd_pcm_hw_params_set_format);
_PA_DEFINE_FUNC(snd_pcm_hw_params_set_channels);
//_PA_DEFINE_FUNC(snd_pcm_hw_params_set_periods_near);
_PA_DEFINE_FUNC(snd_pcm_hw_params_set_rate_near); //!!!
_PA_DEFINE_FUNC(snd_pcm_hw_params_set_rate);
_PA_DEFINE_FUNC(snd_pcm_hw_params_set_rate_resample);
//_PA_DEFINE_FUNC(snd_pcm_hw_params_set_buffer_time_near);
_PA_DEFINE_FUNC(snd_pcm_hw_params_set_buffer_size);
_PA_DEFINE_FUNC(snd_pcm_hw_params_set_buffer_size_near); //!!!
_PA_DEFINE_FUNC(snd_pcm_hw_params_set_buffer_size_min);
//_PA_DEFINE_FUNC(snd_pcm_hw_params_set_period_time_near);
_PA_DEFINE_FUNC(snd_pcm_hw_params_set_period_size_near);
_PA_DEFINE_FUNC(snd_pcm_hw_params_set_periods_integer);
_PA_DEFINE_FUNC(snd_pcm_hw_params_set_periods_min);

_PA_DEFINE_FUNC(snd_pcm_hw_params_get_buffer_size);
//_PA_DEFINE_FUNC(snd_pcm_hw_params_get_period_size);
//_PA_DEFINE_FUNC(snd_pcm_hw_params_get_access);
//_PA_DEFINE_FUNC(snd_pcm_hw_params_get_periods);
//_PA_DEFINE_FUNC(snd_pcm_hw_params_get_rate);
_PA_DEFINE_FUNC(snd_pcm_hw_params_get_channels_min);
_PA_DEFINE_FUNC(snd_pcm_hw_params_get_channels_max);

_PA_DEFINE_FUNC(snd_pcm_hw_params_test_period_size);
_PA_DEFINE_FUNC(snd_pcm_hw_params_test_format);
_PA_DEFINE_FUNC(snd_pcm_hw_params_test_access);
_PA_DEFINE_FUNC(snd_pcm_hw_params_dump);
_PA_DEFINE_FUNC(snd_pcm_hw_params);

_PA_DEFINE_FUNC(snd_pcm_hw_params_get_periods_min);
_PA_DEFINE_FUNC(snd_pcm_hw_params_get_periods_max);
_PA_DEFINE_FUNC(snd_pcm_hw_params_set_period_size);
_PA_DEFINE_FUNC(snd_pcm_hw_params_get_period_size_min);
_PA_DEFINE_FUNC(snd_pcm_hw_params_get_period_size_max);
_PA_DEFINE_FUNC(snd_pcm_hw_params_get_buffer_size_max);
_PA_DEFINE_FUNC(snd_pcm_hw_params_get_rate_min);
_PA_DEFINE_FUNC(snd_pcm_hw_params_get_rate_max);
_PA_DEFINE_FUNC(snd_pcm_hw_params_get_rate_numden);
#define alsa_snd_pcm_hw_params_alloca(ptr) __alsa_snd_alloca(ptr, snd_pcm_hw_params)

_PA_DEFINE_FUNC(snd_pcm_sw_params_sizeof);
_PA_DEFINE_FUNC(snd_pcm_sw_params_malloc);
_PA_DEFINE_FUNC(snd_pcm_sw_params_current);
_PA_DEFINE_FUNC(snd_pcm_sw_params_set_avail_min);
_PA_DEFINE_FUNC(snd_pcm_sw_params);
_PA_DEFINE_FUNC(snd_pcm_sw_params_free);
_PA_DEFINE_FUNC(snd_pcm_sw_params_set_start_threshold);
_PA_DEFINE_FUNC(snd_pcm_sw_params_set_stop_threshold);
_PA_DEFINE_FUNC(snd_pcm_sw_params_get_boundary);
_PA_DEFINE_FUNC(snd_pcm_sw_params_set_silence_threshold);
_PA_DEFINE_FUNC(snd_pcm_sw_params_set_silence_size);
_PA_DEFINE_FUNC(snd_pcm_sw_params_set_xfer_align);
_PA_DEFINE_FUNC(snd_pcm_sw_params_set_tstamp_mode);
#define alsa_snd_pcm_sw_params_alloca(ptr) __alsa_snd_alloca(ptr, snd_pcm_sw_params)

_PA_DEFINE_FUNC(snd_pcm_info);
_PA_DEFINE_FUNC(snd_pcm_info_sizeof);
_PA_DEFINE_FUNC(snd_pcm_info_malloc);
_PA_DEFINE_FUNC(snd_pcm_info_free);
_PA_DEFINE_FUNC(snd_pcm_info_set_device);
_PA_DEFINE_FUNC(snd_pcm_info_set_subdevice);
_PA_DEFINE_FUNC(snd_pcm_info_set_stream);
_PA_DEFINE_FUNC(snd_pcm_info_get_name);
_PA_DEFINE_FUNC(snd_pcm_info_get_card);
#define alsa_snd_pcm_info_alloca(ptr) __alsa_snd_alloca(ptr, snd_pcm_info)

_PA_DEFINE_FUNC(snd_ctl_pcm_next_device);
_PA_DEFINE_FUNC(snd_ctl_pcm_info);
_PA_DEFINE_FUNC(snd_ctl_open);
_PA_DEFINE_FUNC(snd_ctl_close);
_PA_DEFINE_FUNC(snd_ctl_card_info_malloc);
_PA_DEFINE_FUNC(snd_ctl_card_info_free);
_PA_DEFINE_FUNC(snd_ctl_card_info);
_PA_DEFINE_FUNC(snd_ctl_card_info_sizeof);
_PA_DEFINE_FUNC(snd_ctl_card_info_get_name);
#define alsa_snd_ctl_card_info_alloca(ptr) __alsa_snd_alloca(ptr, snd_ctl_card_info)

_PA_DEFINE_FUNC(snd_config);
_PA_DEFINE_FUNC(snd_config_update);
_PA_DEFINE_FUNC(snd_config_search);
_PA_DEFINE_FUNC(snd_config_iterator_entry);
_PA_DEFINE_FUNC(snd_config_iterator_first);
_PA_DEFINE_FUNC(snd_config_iterator_end);
_PA_DEFINE_FUNC(snd_config_iterator_next);
_PA_DEFINE_FUNC(snd_config_get_string);
_PA_DEFINE_FUNC(snd_config_get_id);
_PA_DEFINE_FUNC(snd_config_update_free_global);

_PA_DEFINE_FUNC(snd_pcm_status);
_PA_DEFINE_FUNC(snd_pcm_status_sizeof);
_PA_DEFINE_FUNC(snd_pcm_status_get_tstamp);
_PA_DEFINE_FUNC(snd_pcm_status_get_state);
_PA_DEFINE_FUNC(snd_pcm_status_get_trigger_tstamp);
_PA_DEFINE_FUNC(snd_pcm_status_get_delay);
#define alsa_snd_pcm_status_alloca(ptr) __alsa_snd_alloca(ptr, snd_pcm_status)

_PA_DEFINE_FUNC(snd_card_next);
_PA_DEFINE_FUNC(snd_asoundlib_version);
_PA_DEFINE_FUNC(snd_strerror);
_PA_DEFINE_FUNC(snd_output_stdio_attach);

#define alsa_snd_config_for_each(pos, next, node)\
    for (pos = alsa_snd_config_iterator_first(node),\
         next = alsa_snd_config_iterator_next(pos);\
         pos != alsa_snd_config_iterator_end(node); pos = next, next = alsa_snd_config_iterator_next(pos))

#undef _PA_DEFINE_FUNC

/* Redefine 'PA_ALSA_PATHNAME' to a different Alsa library name if desired. */
#ifndef PA_ALSA_PATHNAME
    #define PA_ALSA_PATHNAME "libasound.so"
#endif
static const char *g_AlsaLibName = PA_ALSA_PATHNAME;

/* Handle to dynamically loaded library. */
static void *g_AlsaLib = NULL;

#ifdef PA_ALSA_DYNAMIC

#define _PA_LOCAL_IMPL(x) __pa_local_##x

int _PA_LOCAL_IMPL(snd_pcm_hw_params_set_rate_near) (snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int *val, int *dir)
{
    int ret;

    if(( ret = alsa_snd_pcm_hw_params_set_rate(pcm, params, (*val), (*dir)) ) < 0 )
        return ret;

    return 0;
}

int _PA_LOCAL_IMPL(snd_pcm_hw_params_set_buffer_size_near) (snd_pcm_t *pcm, snd_pcm_hw_params_t *params, snd_pcm_uframes_t *val)
{
    int ret;

    if(( ret = alsa_snd_pcm_hw_params_set_buffer_size(pcm, params, (*val)) ) < 0 )
        return ret;

    return 0;
}

int _PA_LOCAL_IMPL(snd_pcm_hw_params_set_period_size_near) (snd_pcm_t *pcm, snd_pcm_hw_params_t *params, snd_pcm_uframes_t *val, int *dir)
{
    int ret;

    if(( ret = alsa_snd_pcm_hw_params_set_period_size(pcm, params, (*val), (*dir)) ) < 0 )
        return ret;

    return 0;
}

int _PA_LOCAL_IMPL(snd_pcm_hw_params_get_channels_min) (const snd_pcm_hw_params_t *params, unsigned int *val)
{
    (*val) = 1;
    return 0;
}

int _PA_LOCAL_IMPL(snd_pcm_hw_params_get_channels_max) (const snd_pcm_hw_params_t *params, unsigned int *val)
{
    (*val) = 2;
    return 0;
}

int _PA_LOCAL_IMPL(snd_pcm_hw_params_get_periods_min) (const snd_pcm_hw_params_t *params, unsigned int *val, int *dir)
{
    (*val) = 2;
    return 0;
}

int _PA_LOCAL_IMPL(snd_pcm_hw_params_get_periods_max) (const snd_pcm_hw_params_t *params, unsigned int *val, int *dir)
{
    (*val) = 8;
    return 0;
}

int _PA_LOCAL_IMPL(snd_pcm_hw_params_get_period_size_min) (const snd_pcm_hw_params_t *params, snd_pcm_uframes_t *frames, int *dir)
{
    (*frames) = 64;
    return 0;
}

int _PA_LOCAL_IMPL(snd_pcm_hw_params_get_period_size_max) (const snd_pcm_hw_params_t *params, snd_pcm_uframes_t *frames, int *dir)
{
    (*frames) = 512;
    return 0;
}

int _PA_LOCAL_IMPL(snd_pcm_hw_params_get_buffer_size_max) (const snd_pcm_hw_params_t *params, snd_pcm_uframes_t *val)
{
    int ret;
    int dir                = 0;
    snd_pcm_uframes_t pmax = 0;
    unsigned int      pcnt = 0;

    if(( ret = _PA_LOCAL_IMPL(snd_pcm_hw_params_get_period_size_max)(params, &pmax, &dir) ) < 0 )
        return ret;
    if(( ret = _PA_LOCAL_IMPL(snd_pcm_hw_params_get_periods_max)(params, &pcnt, &dir) ) < 0 )
        return ret;

    (*val) = pmax * pcnt;
    return 0;
}

int _PA_LOCAL_IMPL(snd_pcm_hw_params_get_rate_min) (const snd_pcm_hw_params_t *params, unsigned int *val, int *dir)
{
    (*val) = 44100;
    return 0;
}

int _PA_LOCAL_IMPL(snd_pcm_hw_params_get_rate_max) (const snd_pcm_hw_params_t *params, unsigned int *val, int *dir)
{
    (*val) = 44100;
    return 0;
}

#endif // PA_ALSA_DYNAMIC

/* Trying to load Alsa library dynamically if 'PA_ALSA_DYNAMIC' is defined, othervise
   will link during compilation.
*/
static int PaAlsa_LoadLibrary()
{
#ifdef PA_ALSA_DYNAMIC

    PA_DEBUG(( "%s: loading ALSA library file - %s\n", __FUNCTION__, g_AlsaLibName ));

    dlerror();
    g_AlsaLib = dlopen(g_AlsaLibName, (RTLD_NOW|RTLD_GLOBAL) );
    if (g_AlsaLib == NULL)
    {
        PA_DEBUG(( "%s: failed dlopen() ALSA library file - %s, error: %s\n", __FUNCTION__, g_AlsaLibName, dlerror() ));
        return 0;
    }

    PA_DEBUG(( "%s: loading ALSA API\n", __FUNCTION__ ));

    #define _PA_LOAD_FUNC(x) do {             \
        alsa_##x = dlsym( g_AlsaLib, #x );      \
        if( alsa_##x == NULL ) {               \
            PA_DEBUG(( "%s: symbol [%s] not found in - %s, error: %s\n", __FUNCTION__, #x, g_AlsaLibName, dlerror() )); }\
        } while(0)

#else

    #define _PA_LOAD_FUNC(x) alsa_##x = &x

#endif

    _PA_LOAD_FUNC(snd_pcm_open);
    _PA_LOAD_FUNC(snd_pcm_close);
    _PA_LOAD_FUNC(snd_pcm_nonblock);
    _PA_LOAD_FUNC(snd_pcm_frames_to_bytes);
    _PA_LOAD_FUNC(snd_pcm_prepare);
    _PA_LOAD_FUNC(snd_pcm_start);
    _PA_LOAD_FUNC(snd_pcm_resume);
    _PA_LOAD_FUNC(snd_pcm_wait);
    _PA_LOAD_FUNC(snd_pcm_state);
    _PA_LOAD_FUNC(snd_pcm_avail_update);
    _PA_LOAD_FUNC(snd_pcm_areas_silence);
    _PA_LOAD_FUNC(snd_pcm_mmap_begin);
    _PA_LOAD_FUNC(snd_pcm_mmap_commit);
    _PA_LOAD_FUNC(snd_pcm_readi);
    _PA_LOAD_FUNC(snd_pcm_readn);
    _PA_LOAD_FUNC(snd_pcm_writei);
    _PA_LOAD_FUNC(snd_pcm_writen);
    _PA_LOAD_FUNC(snd_pcm_drain);
    _PA_LOAD_FUNC(snd_pcm_recover);
    _PA_LOAD_FUNC(snd_pcm_drop);
    _PA_LOAD_FUNC(snd_pcm_area_copy);
    _PA_LOAD_FUNC(snd_pcm_poll_descriptors);
    _PA_LOAD_FUNC(snd_pcm_poll_descriptors_count);
    _PA_LOAD_FUNC(snd_pcm_poll_descriptors_revents);
    _PA_LOAD_FUNC(snd_pcm_format_size);
    _PA_LOAD_FUNC(snd_pcm_link);
    _PA_LOAD_FUNC(snd_pcm_delay);

    _PA_LOAD_FUNC(snd_pcm_hw_params_sizeof);
    _PA_LOAD_FUNC(snd_pcm_hw_params_malloc);
    _PA_LOAD_FUNC(snd_pcm_hw_params_free);
    _PA_LOAD_FUNC(snd_pcm_hw_params_any);
    _PA_LOAD_FUNC(snd_pcm_hw_params_set_access);
    _PA_LOAD_FUNC(snd_pcm_hw_params_set_format);
    _PA_LOAD_FUNC(snd_pcm_hw_params_set_channels);
//    _PA_LOAD_FUNC(snd_pcm_hw_params_set_periods_near);
    _PA_LOAD_FUNC(snd_pcm_hw_params_set_rate_near);
    _PA_LOAD_FUNC(snd_pcm_hw_params_set_rate);
    _PA_LOAD_FUNC(snd_pcm_hw_params_set_rate_resample);
//    _PA_LOAD_FUNC(snd_pcm_hw_params_set_buffer_time_near);
    _PA_LOAD_FUNC(snd_pcm_hw_params_set_buffer_size);
    _PA_LOAD_FUNC(snd_pcm_hw_params_set_buffer_size_near);
    _PA_LOAD_FUNC(snd_pcm_hw_params_set_buffer_size_min);
//    _PA_LOAD_FUNC(snd_pcm_hw_params_set_period_time_near);
    _PA_LOAD_FUNC(snd_pcm_hw_params_set_period_size_near);
    _PA_LOAD_FUNC(snd_pcm_hw_params_set_periods_integer);
    _PA_LOAD_FUNC(snd_pcm_hw_params_set_periods_min);

    _PA_LOAD_FUNC(snd_pcm_hw_params_get_buffer_size);
//    _PA_LOAD_FUNC(snd_pcm_hw_params_get_period_size);
//    _PA_LOAD_FUNC(snd_pcm_hw_params_get_access);
//    _PA_LOAD_FUNC(snd_pcm_hw_params_get_periods);
//    _PA_LOAD_FUNC(snd_pcm_hw_params_get_rate);
    _PA_LOAD_FUNC(snd_pcm_hw_params_get_channels_min);
    _PA_LOAD_FUNC(snd_pcm_hw_params_get_channels_max);

    _PA_LOAD_FUNC(snd_pcm_hw_params_test_period_size);
    _PA_LOAD_FUNC(snd_pcm_hw_params_test_format);
    _PA_LOAD_FUNC(snd_pcm_hw_params_test_access);
    _PA_LOAD_FUNC(snd_pcm_hw_params_dump);
    _PA_LOAD_FUNC(snd_pcm_hw_params);

    _PA_LOAD_FUNC(snd_pcm_hw_params_get_periods_min);
    _PA_LOAD_FUNC(snd_pcm_hw_params_get_periods_max);
    _PA_LOAD_FUNC(snd_pcm_hw_params_set_period_size);
    _PA_LOAD_FUNC(snd_pcm_hw_params_get_period_size_min);
    _PA_LOAD_FUNC(snd_pcm_hw_params_get_period_size_max);
    _PA_LOAD_FUNC(snd_pcm_hw_params_get_buffer_size_max);
    _PA_LOAD_FUNC(snd_pcm_hw_params_get_rate_min);
    _PA_LOAD_FUNC(snd_pcm_hw_params_get_rate_max);
    _PA_LOAD_FUNC(snd_pcm_hw_params_get_rate_numden);

    _PA_LOAD_FUNC(snd_pcm_sw_params_sizeof);
    _PA_LOAD_FUNC(snd_pcm_sw_params_malloc);
    _PA_LOAD_FUNC(snd_pcm_sw_params_current);
    _PA_LOAD_FUNC(snd_pcm_sw_params_set_avail_min);
    _PA_LOAD_FUNC(snd_pcm_sw_params);
    _PA_LOAD_FUNC(snd_pcm_sw_params_free);
    _PA_LOAD_FUNC(snd_pcm_sw_params_set_start_threshold);
    _PA_LOAD_FUNC(snd_pcm_sw_params_set_stop_threshold);
    _PA_LOAD_FUNC(snd_pcm_sw_params_get_boundary);
    _PA_LOAD_FUNC(snd_pcm_sw_params_set_silence_threshold);
    _PA_LOAD_FUNC(snd_pcm_sw_params_set_silence_size);
    _PA_LOAD_FUNC(snd_pcm_sw_params_set_xfer_align);
    _PA_LOAD_FUNC(snd_pcm_sw_params_set_tstamp_mode);

    _PA_LOAD_FUNC(snd_pcm_info);
    _PA_LOAD_FUNC(snd_pcm_info_sizeof);
    _PA_LOAD_FUNC(snd_pcm_info_malloc);
    _PA_LOAD_FUNC(snd_pcm_info_free);
    _PA_LOAD_FUNC(snd_pcm_info_set_device);
    _PA_LOAD_FUNC(snd_pcm_info_set_subdevice);
    _PA_LOAD_FUNC(snd_pcm_info_set_stream);
    _PA_LOAD_FUNC(snd_pcm_info_get_name);
    _PA_LOAD_FUNC(snd_pcm_info_get_card);

    _PA_LOAD_FUNC(snd_ctl_pcm_next_device);
    _PA_LOAD_FUNC(snd_ctl_pcm_info);
    _PA_LOAD_FUNC(snd_ctl_open);
    _PA_LOAD_FUNC(snd_ctl_close);
    _PA_LOAD_FUNC(snd_ctl_card_info_malloc);
    _PA_LOAD_FUNC(snd_ctl_card_info_free);
    _PA_LOAD_FUNC(snd_ctl_card_info);
    _PA_LOAD_FUNC(snd_ctl_card_info_sizeof);
    _PA_LOAD_FUNC(snd_ctl_card_info_get_name);

    _PA_LOAD_FUNC(snd_config);
    _PA_LOAD_FUNC(snd_config_update);
    _PA_LOAD_FUNC(snd_config_search);
    _PA_LOAD_FUNC(snd_config_iterator_entry);
    _PA_LOAD_FUNC(snd_config_iterator_first);
    _PA_LOAD_FUNC(snd_config_iterator_end);
    _PA_LOAD_FUNC(snd_config_iterator_next);
    _PA_LOAD_FUNC(snd_config_get_string);
    _PA_LOAD_FUNC(snd_config_get_id);
    _PA_LOAD_FUNC(snd_config_update_free_global);

    _PA_LOAD_FUNC(snd_pcm_status);
    _PA_LOAD_FUNC(snd_pcm_status_sizeof);
    _PA_LOAD_FUNC(snd_pcm_status_get_tstamp);
    _PA_LOAD_FUNC(snd_pcm_status_get_state);
    _PA_LOAD_FUNC(snd_pcm_status_get_trigger_tstamp);
    _PA_LOAD_FUNC(snd_pcm_status_get_delay);

    _PA_LOAD_FUNC(snd_card_next);
    _PA_LOAD_FUNC(snd_asoundlib_version);
    _PA_LOAD_FUNC(snd_strerror);
    _PA_LOAD_FUNC(snd_output_stdio_attach);
#undef _PA_LOAD_FUNC

#ifdef PA_ALSA_DYNAMIC
    PA_DEBUG(( "%s: loaded ALSA API - ok\n", __FUNCTION__ ));

#define _PA_VALIDATE_LOAD_REPLACEMENT(x)\
    do {\
        if( alsa_##x == NULL )\
        {\
            alsa_##x = &_PA_LOCAL_IMPL(x);\
            PA_DEBUG(( "%s: replacing [%s] with local implementation\n", __FUNCTION__, #x ));\
        }\
    } while (0)

    _PA_VALIDATE_LOAD_REPLACEMENT(snd_pcm_hw_params_set_rate_near);
    _PA_VALIDATE_LOAD_REPLACEMENT(snd_pcm_hw_params_set_buffer_size_near);
    _PA_VALIDATE_LOAD_REPLACEMENT(snd_pcm_hw_params_set_period_size_near);
    _PA_VALIDATE_LOAD_REPLACEMENT(snd_pcm_hw_params_get_channels_min);
    _PA_VALIDATE_LOAD_REPLACEMENT(snd_pcm_hw_params_get_channels_max);
    _PA_VALIDATE_LOAD_REPLACEMENT(snd_pcm_hw_params_get_periods_min);
    _PA_VALIDATE_LOAD_REPLACEMENT(snd_pcm_hw_params_get_periods_max);
    _PA_VALIDATE_LOAD_REPLACEMENT(snd_pcm_hw_params_get_period_size_min);
    _PA_VALIDATE_LOAD_REPLACEMENT(snd_pcm_hw_params_get_period_size_max);
    _PA_VALIDATE_LOAD_REPLACEMENT(snd_pcm_hw_params_get_buffer_size_max);
    _PA_VALIDATE_LOAD_REPLACEMENT(snd_pcm_hw_params_get_rate_min);
    _PA_VALIDATE_LOAD_REPLACEMENT(snd_pcm_hw_params_get_rate_max);

#undef _PA_LOCAL_IMPL
#undef _PA_VALIDATE_LOAD_REPLACEMENT

#endif // PA_ALSA_DYNAMIC

    return 1;
}

void PaAlsa_SetLibraryPathName( const char *pathName )
{
#ifdef PA_ALSA_DYNAMIC
    g_AlsaLibName = pathName;
#else
    (void)pathName;
#endif
}

/* Close handle to Alsa library. */
static void PaAlsa_CloseLibrary()
{
#ifdef PA_ALSA_DYNAMIC
    dlclose(g_AlsaLib);
    g_AlsaLib = NULL;
#endif
}

/* Check return value of ALSA function, and map it to PaError */
#define ENSURE_(expr, code) \
    do { \
        int __pa_unsure_error_id;\
        if( UNLIKELY( (__pa_unsure_error_id = (expr)) < 0 ) ) \
        { \
            /* PaUtil_SetLastHostErrorInfo should only be used in the main thread */ \
            if( (code) == paUnanticipatedHostError && pthread_equal( pthread_self(), paUnixMainThread) ) \
            { \
                PaUtil_SetLastHostErrorInfo( paALSA, __pa_unsure_error_id, alsa_snd_strerror( __pa_unsure_error_id ) ); \
            } \
            PaUtil_DebugPrint( "Expression '" #expr "' failed in '" __FILE__ "', line: " STRINGIZE( __LINE__ ) "\n" ); \
            if( (code) == paUnanticipatedHostError ) \
                PA_DEBUG(( "Host error description: %s\n", alsa_snd_strerror( __pa_unsure_error_id ) )); \
            result = (code); \
            goto error; \
        } \
    } while (0)

#define ASSERT_CALL_(expr, success) \
    do {\
        int __pa_assert_error_id;\
        __pa_assert_error_id = (expr);\
        assert( success == __pa_assert_error_id );\
    } while (0)

static int numPeriods_ = 4;
static int busyRetries_ = 100;

int PaAlsa_SetNumPeriods( int numPeriods )
{
    numPeriods_ = numPeriods;
    return paNoError;
}

typedef enum
{
    StreamDirection_In,
    StreamDirection_Out
} StreamDirection;

typedef struct
{
    PaSampleFormat hostSampleFormat;
    int numUserChannels, numHostChannels;
    int userInterleaved, hostInterleaved;
    int canMmap;
    void *nonMmapBuffer;
    unsigned int nonMmapBufferSize;
    PaDeviceIndex device;     /* Keep the device index */
    int deviceIsPlug; /* Distinguish plug types from direct 'hw:' devices */
    int useReventFix; /* Alsa older than 1.0.16, plug devices need a fix */

    snd_pcm_t *pcm;
    snd_pcm_uframes_t framesPerPeriod, alsaBufferSize;
    snd_pcm_format_t nativeFormat;
    unsigned int nfds;
    int ready;  /* Marked ready from poll */
    void **userBuffers;
    snd_pcm_uframes_t offset;
    StreamDirection streamDir;

    snd_pcm_channel_area_t *channelAreas;  /* Needed for channel adaption */
} PaAlsaStreamComponent;

/* Implementation specific stream structure */
typedef struct PaAlsaStream
{
    PaUtilStreamRepresentation streamRepresentation;
    PaUtilCpuLoadMeasurer cpuLoadMeasurer;
    PaUtilBufferProcessor bufferProcessor;
    PaUnixThread thread;

    unsigned long framesPerUserBuffer, maxFramesPerHostBuffer;

    int primeBuffers;
    int callbackMode;              /* bool: are we running in callback mode? */
    int pcmsSynced;                /* Have we successfully synced pcms */
    int rtSched;

    /* the callback thread uses these to poll the sound device(s), waiting
     * for data to be ready/available */
    struct pollfd* pfds;
    int pollTimeout;

    /* Used in communication between threads */
    volatile sig_atomic_t callback_finished; /* bool: are we in the "callback finished" state? */
    volatile sig_atomic_t callbackAbort;    /* Drop frames? */
    volatile sig_atomic_t isActive;         /* Is stream in active state? (Between StartStream and StopStream || !paContinue) */
    PaUnixMutex stateMtx;                   /* Used to synchronize access to stream state */

    int neverDropInput;

    PaTime underrun;
    PaTime overrun;

    PaAlsaStreamComponent capture, playback;
}
PaAlsaStream;

/* PaAlsaHostApiRepresentation - host api datastructure specific to this implementation */

typedef struct PaAlsaHostApiRepresentation
{
    PaUtilHostApiRepresentation baseHostApiRep;
    PaUtilStreamInterface callbackStreamInterface;
    PaUtilStreamInterface blockingStreamInterface;

    PaUtilAllocationGroup *allocations;

    PaHostApiIndex hostApiIndex;
    PaUint32 alsaLibVersion; /* Retrieved from the library at run-time */
}
PaAlsaHostApiRepresentation;

typedef struct PaAlsaDeviceInfo
{
    PaDeviceInfo baseDeviceInfo;
    char *alsaName;
    int isPlug;
    int minInputChannels;
    int minOutputChannels;
}
PaAlsaDeviceInfo;

/* prototypes for functions declared in this file */

static void Terminate( struct PaUtilHostApiRepresentation *hostApi );
static PaError IsFormatSupported( struct PaUtilHostApiRepresentation *hostApi,
                                  const PaStreamParameters *inputParameters,
                                  const PaStreamParameters *outputParameters,
                                  double sampleRate );
static PaError OpenStream( struct PaUtilHostApiRepresentation *hostApi,
                           PaStream** s,
                           const PaStreamParameters *inputParameters,
                           const PaStreamParameters *outputParameters,
                           double sampleRate,
                           unsigned long framesPerBuffer,
                           PaStreamFlags streamFlags,
                           PaStreamCallback *callback,
                           void *userData );
static PaError CloseStream( PaStream* stream );
static PaError StartStream( PaStream *stream );
static PaError StopStream( PaStream *stream );
static PaError AbortStream( PaStream *stream );
static PaError IsStreamStopped( PaStream *s );
static PaError IsStreamActive( PaStream *stream );
static PaTime GetStreamTime( PaStream *stream );
static double GetStreamCpuLoad( PaStream* stream );
static PaError BuildDeviceList( PaAlsaHostApiRepresentation *hostApi );
static int SetApproximateSampleRate( snd_pcm_t *pcm, snd_pcm_hw_params_t *hwParams, double sampleRate );
static int GetExactSampleRate( snd_pcm_hw_params_t *hwParams, double *sampleRate );
static PaUint32 PaAlsaVersionNum(void);

/* Callback prototypes */
static void *CallbackThreadFunc( void *userData );

/* Blocking prototypes */
static signed long GetStreamReadAvailable( PaStream* s );
static signed long GetStreamWriteAvailable( PaStream* s );
static PaError ReadStream( PaStream* stream, void *buffer, unsigned long frames );
static PaError WriteStream( PaStream* stream, const void *buffer, unsigned long frames );


static const PaAlsaDeviceInfo *GetDeviceInfo( const PaUtilHostApiRepresentation *hostApi, int device )
{
    return (const PaAlsaDeviceInfo *)hostApi->deviceInfos[device];
}

/** Uncommented because AlsaErrorHandler is unused for anything good yet. If AlsaErrorHandler is
    to be used, do not forget to register this callback in PaAlsa_Initialize, and unregister in Terminate.
*/
/*static void AlsaErrorHandler(const char *file, int line, const char *function, int err, const char *fmt, ...)
{
}*/

PaError PaAlsa_Initialize( PaUtilHostApiRepresentation **hostApi, PaHostApiIndex hostApiIndex )
{
    PaError result = paNoError;
    PaAlsaHostApiRepresentation *alsaHostApi = NULL;

    /* Try loading Alsa library. */
    if (!PaAlsa_LoadLibrary())
        return paHostApiNotFound;

    PA_UNLESS( alsaHostApi = (PaAlsaHostApiRepresentation*) PaUtil_AllocateMemory(
                sizeof(PaAlsaHostApiRepresentation) ), paInsufficientMemory );
    PA_UNLESS( alsaHostApi->allocations = PaUtil_CreateAllocationGroup(), paInsufficientMemory );
    alsaHostApi->hostApiIndex = hostApiIndex;
    alsaHostApi->alsaLibVersion = PaAlsaVersionNum();

    *hostApi = (PaUtilHostApiRepresentation*)alsaHostApi;
    (*hostApi)->info.structVersion = 1;
    (*hostApi)->info.type = paALSA;
    (*hostApi)->info.name = "ALSA";

    (*hostApi)->Terminate = Terminate;
    (*hostApi)->OpenStream = OpenStream;
    (*hostApi)->IsFormatSupported = IsFormatSupported;

    /** If AlsaErrorHandler is to be used, do not forget to unregister callback pointer in
        Terminate function.
    */
    /*ENSURE_( snd_lib_error_set_handler(AlsaErrorHandler), paUnanticipatedHostError );*/

    PA_ENSURE( BuildDeviceList( alsaHostApi ) );

    PaUtil_InitializeStreamInterface( &alsaHostApi->callbackStreamInterface,
                                      CloseStream, StartStream,
                                      StopStream, AbortStream,
                                      IsStreamStopped, IsStreamActive,
                                      GetStreamTime, GetStreamCpuLoad,
                                      PaUtil_DummyRead, PaUtil_DummyWrite,
                                      PaUtil_DummyGetReadAvailable,
                                      PaUtil_DummyGetWriteAvailable );

    PaUtil_InitializeStreamInterface( &alsaHostApi->blockingStreamInterface,
                                      CloseStream, StartStream,
                                      StopStream, AbortStream,
                                      IsStreamStopped, IsStreamActive,
                                      GetStreamTime, PaUtil_DummyGetCpuLoad,
                                      ReadStream, WriteStream,
                                      GetStreamReadAvailable,
                                      GetStreamWriteAvailable );

    PA_ENSURE( PaUnixThreading_Initialize() );

    return result;

error:
    if( alsaHostApi )
    {
        if( alsaHostApi->allocations )
        {
            PaUtil_FreeAllAllocations( alsaHostApi->allocations );
            PaUtil_DestroyAllocationGroup( alsaHostApi->allocations );
        }

        PaUtil_FreeMemory( alsaHostApi );
    }

    return result;
}

static void Terminate( struct PaUtilHostApiRepresentation *hostApi )
{
    PaAlsaHostApiRepresentation *alsaHostApi = (PaAlsaHostApiRepresentation*)hostApi;

    assert( hostApi );

    /** See AlsaErrorHandler and PaAlsa_Initialize for details.
    */
    /*snd_lib_error_set_handler(NULL);*/

    if( alsaHostApi->allocations )
    {
        PaUtil_FreeAllAllocations( alsaHostApi->allocations );
        PaUtil_DestroyAllocationGroup( alsaHostApi->allocations );
    }

    PaUtil_FreeMemory( alsaHostApi );
    alsa_snd_config_update_free_global();

    /* Close Alsa library. */
    PaAlsa_CloseLibrary();
}

/** Determine max channels and default latencies.
 *
 * This function provides functionality to grope an opened (might be opened for capture or playback) pcm device for
 * traits like max channels, suitable default latencies and default sample rate. Upon error, max channels is set to zero,
 * and a suitable result returned. The device is closed before returning.
 */
static PaError GropeDevice( snd_pcm_t* pcm, int isPlug, StreamDirection mode, int openBlocking,
        PaAlsaDeviceInfo* devInfo )
{
    PaError result = paNoError;
    snd_pcm_hw_params_t *hwParams;
    snd_pcm_uframes_t alsaBufferFrames, alsaPeriodFrames;
    unsigned int minChans, maxChans;
    int* minChannels, * maxChannels;
    double * defaultLowLatency, * defaultHighLatency, * defaultSampleRate =
        &devInfo->baseDeviceInfo.defaultSampleRate;
    double defaultSr = *defaultSampleRate;
    int dir;

    assert( pcm );

    PA_DEBUG(( "%s: collecting info ..\n", __FUNCTION__ ));

    if( StreamDirection_In == mode )
    {
        minChannels = &devInfo->minInputChannels;
        maxChannels = &devInfo->baseDeviceInfo.maxInputChannels;
        defaultLowLatency = &devInfo->baseDeviceInfo.defaultLowInputLatency;
        defaultHighLatency = &devInfo->baseDeviceInfo.defaultHighInputLatency;
    }
    else
    {
        minChannels = &devInfo->minOutputChannels;
        maxChannels = &devInfo->baseDeviceInfo.maxOutputChannels;
        defaultLowLatency = &devInfo->baseDeviceInfo.defaultLowOutputLatency;
        defaultHighLatency = &devInfo->baseDeviceInfo.defaultHighOutputLatency;
    }

    ENSURE_( alsa_snd_pcm_nonblock( pcm, 0 ), paUnanticipatedHostError );

    alsa_snd_pcm_hw_params_alloca( &hwParams );
    alsa_snd_pcm_hw_params_any( pcm, hwParams );

    if( defaultSr >= 0 )
    {
        /* Could be that the device opened in one mode supports samplerates that the other mode wont have,
         * so try again .. */
        if( SetApproximateSampleRate( pcm, hwParams, defaultSr ) < 0 )
        {
            defaultSr = -1.;
            alsa_snd_pcm_hw_params_any( pcm, hwParams ); /* Clear any params (rate) that might have been set */
            PA_DEBUG(( "%s: Original default samplerate failed, trying again ..\n", __FUNCTION__ ));
        }
    }

    if( defaultSr < 0. )           /* Default sample rate not set */
    {
        unsigned int sampleRate = 44100;        /* Will contain approximate rate returned by alsa-lib */

        /* Don't allow rate resampling when probing for the default rate (but ignore if this call fails) */
        alsa_snd_pcm_hw_params_set_rate_resample( pcm, hwParams, 0 );
        if( alsa_snd_pcm_hw_params_set_rate_near( pcm, hwParams, &sampleRate, NULL ) < 0 )
        {
            result = paUnanticipatedHostError;
            goto error;
        }
        ENSURE_( GetExactSampleRate( hwParams, &defaultSr ), paUnanticipatedHostError );
    }

    ENSURE_( alsa_snd_pcm_hw_params_get_channels_min( hwParams, &minChans ), paUnanticipatedHostError );
    ENSURE_( alsa_snd_pcm_hw_params_get_channels_max( hwParams, &maxChans ), paUnanticipatedHostError );
    assert( maxChans <= INT_MAX );
    assert( maxChans > 0 );    /* Weird linking issue could cause wrong version of ALSA symbols to be called,
                                   resulting in zeroed values */

    /* XXX: Limit to sensible number (ALSA plugins accept a crazy amount of channels)? */
    if( isPlug && maxChans > 128 )
    {
        maxChans = 128;
        PA_DEBUG(( "%s: Limiting number of plugin channels to %u\n", __FUNCTION__, maxChans ));
    }

    /* TWEAKME:
     * Giving values for default min and max latency is not straightforward.
     *  * for low latency, we want to give the lowest value that will work reliably.
     *      This varies based on the sound card, kernel, CPU, etc.  Better to give
     *      sub-optimal latency than to give a number too low and cause dropouts.
     *  * for high latency we want to give a large enough value that dropouts are basically impossible.
     *      This doesn't really require as much tweaking, since providing too large a number will
     *      just cause us to select the nearest setting that will work at stream config time.
     */
    /* Try low latency values, (sometimes the buffer & period that result are larger) */
    alsaBufferFrames = 512;
    alsaPeriodFrames = 128;
    ENSURE_( alsa_snd_pcm_hw_params_set_buffer_size_near( pcm, hwParams, &alsaBufferFrames ), paUnanticipatedHostError );
    ENSURE_( alsa_snd_pcm_hw_params_set_period_size_near( pcm, hwParams, &alsaPeriodFrames, &dir ), paUnanticipatedHostError );
    *defaultLowLatency = (double) (alsaBufferFrames - alsaPeriodFrames) / defaultSr;

    /* Base the high latency case on values four times larger */
    alsaBufferFrames = 2048;
    alsaPeriodFrames = 512;
    /* Have to reset hwParams, to set new buffer size; need to also set sample rate again */
    ENSURE_( alsa_snd_pcm_hw_params_any( pcm, hwParams ), paUnanticipatedHostError );
    ENSURE_( SetApproximateSampleRate( pcm, hwParams, defaultSr ), paUnanticipatedHostError );
    ENSURE_( alsa_snd_pcm_hw_params_set_buffer_size_near( pcm, hwParams, &alsaBufferFrames ), paUnanticipatedHostError );
    ENSURE_( alsa_snd_pcm_hw_params_set_period_size_near( pcm, hwParams, &alsaPeriodFrames, &dir ), paUnanticipatedHostError );
    *defaultHighLatency = (double) (alsaBufferFrames - alsaPeriodFrames) / defaultSr;

    *minChannels = (int)minChans;
    *maxChannels = (int)maxChans;
    *defaultSampleRate = defaultSr;

end:
    alsa_snd_pcm_close( pcm );
    return result;

error:
    goto end;
}

/* Initialize device info with invalid values (maxInputChannels and maxOutputChannels are set to zero since these indicate
 * whether input/output is available) */
static void InitializeDeviceInfo( PaDeviceInfo *deviceInfo )
{
    deviceInfo->structVersion = -1;
    deviceInfo->name = NULL;
    deviceInfo->hostApi = -1;
    deviceInfo->maxInputChannels = 0;
    deviceInfo->maxOutputChannels = 0;
    deviceInfo->defaultLowInputLatency = -1.;
    deviceInfo->defaultLowOutputLatency = -1.;
    deviceInfo->defaultHighInputLatency = -1.;
    deviceInfo->defaultHighOutputLatency = -1.;
    deviceInfo->defaultSampleRate = -1.;
}


/* Retrieve the version of the runtime Alsa-lib, as a single number equivalent to
 * SND_LIB_VERSION.  Only a version string is available ("a.b.c") so this has to be converted.
 * Assume 'a' and 'b' are single digits only.
 */
static PaUint32 PaAlsaVersionNum(void)
{
    char* verStr;
    PaUint32 verNum;

    verStr = (char*) alsa_snd_asoundlib_version();
    verNum = ALSA_VERSION_INT( atoi(verStr), atoi(verStr + 2), atoi(verStr + 4) );
    PA_DEBUG(( "ALSA version (build): " SND_LIB_VERSION_STR "\nALSA version (runtime): %s\n", verStr ));

    return verNum;
}


/* Helper struct */
typedef struct
{
    char *alsaName;
    char *name;
    int isPlug;
    int hasPlayback;
    int hasCapture;
} HwDevInfo;


HwDevInfo predefinedNames[] = {
    { "center_lfe", NULL, 0, 1, 0 },
/* { "default", NULL, 0, 1, 1 }, */
    { "dmix", NULL, 0, 1, 0 },
/* { "dpl", NULL, 0, 1, 0 }, */
/* { "dsnoop", NULL, 0, 0, 1 }, */
    { "front", NULL, 0, 1, 0 },
    { "iec958", NULL, 0, 1, 0 },
/* { "modem", NULL, 0, 1, 0 }, */
    { "rear", NULL, 0, 1, 0 },
    { "side", NULL, 0, 1, 0 },
/*     { "spdif", NULL, 0, 0, 0 }, */
    { "surround40", NULL, 0, 1, 0 },
    { "surround41", NULL, 0, 1, 0 },
    { "surround50", NULL, 0, 1, 0 },
    { "surround51", NULL, 0, 1, 0 },
    { "surround71", NULL, 0, 1, 0 },

    { "AndroidPlayback_Earpiece_normal",         NULL, 0, 1, 0 },
    { "AndroidPlayback_Speaker_normal",          NULL, 0, 1, 0 },
    { "AndroidPlayback_Bluetooth_normal",        NULL, 0, 1, 0 },
    { "AndroidPlayback_Headset_normal",          NULL, 0, 1, 0 },
    { "AndroidPlayback_Speaker_Headset_normal",  NULL, 0, 1, 0 },
    { "AndroidPlayback_Bluetooth-A2DP_normal",   NULL, 0, 1, 0 },
    { "AndroidPlayback_ExtraDockSpeaker_normal", NULL, 0, 1, 0 },
    { "AndroidPlayback_TvOut_normal",            NULL, 0, 1, 0 },

    { "AndroidRecord_Microphone",                NULL, 0, 0, 1 },
    { "AndroidRecord_Earpiece_normal",           NULL, 0, 0, 1 },
    { "AndroidRecord_Speaker_normal",            NULL, 0, 0, 1 },
    { "AndroidRecord_Headset_normal",            NULL, 0, 0, 1 },
    { "AndroidRecord_Bluetooth_normal",          NULL, 0, 0, 1 },
    { "AndroidRecord_Speaker_Headset_normal",    NULL, 0, 0, 1 },

    { NULL, NULL, 0, 1, 0 }
};

static const HwDevInfo *FindDeviceName( const char *name )
{
    int i;

    for( i = 0; predefinedNames[i].alsaName; i++ )
    {
        if( strcmp( name, predefinedNames[i].alsaName ) == 0 )
        {
            return &predefinedNames[i];
        }
    }

    return NULL;
}

static PaError PaAlsa_StrDup( PaAlsaHostApiRepresentation *alsaApi,
        char **dst,
        const char *src)
{
    PaError result = paNoError;
    int len = strlen( src ) + 1;

    /* PA_DEBUG(("PaStrDup %s %d\n", src, len)); */

    PA_UNLESS( *dst = (char *)PaUtil_GroupAllocateMemory( alsaApi->allocations, len ),
            paInsufficientMemory );
    strncpy( *dst, src, len );

error:
    return result;
}

/* Disregard some standard plugins
 */
static int IgnorePlugin( const char *pluginId )
{
    static const char *ignoredPlugins[] = {"hw", "plughw", "plug", "dsnoop", "tee",
        "file", "null", "shm", "cards", "rate_convert", NULL};
    int i = 0;
    while( ignoredPlugins[i] )
    {
        if( !strcmp( pluginId, ignoredPlugins[i] ) )
        {
            return 1;
        }
        ++i;
    }

    return 0;
}

/* Skip past parts at the beginning of a (pcm) info name that are already in the card name, to avoid duplication */
static char *SkipCardDetailsInName( char *infoSkipName, char *cardRefName )
{
    char *lastSpacePosn = infoSkipName;

    /* Skip matching chars; but only in chunks separated by ' ' (not part words etc), so track lastSpacePosn */
    while( *cardRefName )
    {
        while( *infoSkipName && *cardRefName && *infoSkipName == *cardRefName)
        {
            infoSkipName++;
            cardRefName++;
            if( *infoSkipName == ' ' || *infoSkipName == '\0' )
                lastSpacePosn = infoSkipName;
        }
        infoSkipName = lastSpacePosn;
        /* Look for another chunk; post-increment means ends pointing to next char */
        while( *cardRefName && ( *cardRefName++ != ' ' ));
    }
    if( *infoSkipName == '\0' )
        return "-"; /* The 2 names were identical; instead of a nul-string, return a marker string */

    /* Now want to move to the first char after any spaces */
    while( *lastSpacePosn && *lastSpacePosn == ' ' )
        lastSpacePosn++;
    /* Skip a single separator char if present in the remaining pcm name; (pa will add its own) */
    if(( *lastSpacePosn == '-' || *lastSpacePosn == ':' ) && *(lastSpacePosn + 1) == ' ' )
        lastSpacePosn += 2;

    return lastSpacePosn;
}

/** Open PCM device.
 *
 * Wrapper around alsa_snd_pcm_open which may repeatedly retry opening a device if it is busy, for
 * a certain time. This is because dmix may temporarily hold on to a device after it (dmix)
 * has been opened and closed.
 * @param mode: Open mode (e.g., SND_PCM_BLOCKING).
 * @param waitOnBusy: Retry opening busy device for up to one second?
 **/
static int OpenPcm( snd_pcm_t **pcmp, const char *name, snd_pcm_stream_t stream, int mode, int waitOnBusy )
{
    int ret, tries = 0, maxTries = waitOnBusy ? busyRetries_ : 0;

    ret = alsa_snd_pcm_open( pcmp, name, stream, mode );

    for( tries = 0; tries < maxTries && -EBUSY == ret; ++tries )
    {
        Pa_Sleep( 10 );
        ret = alsa_snd_pcm_open( pcmp, name, stream, mode );
        if( -EBUSY != ret )
        {
            PA_DEBUG(( "%s: Successfully opened initially busy device after %d tries\n", __FUNCTION__, tries ));
        }
    }
    if( -EBUSY == ret )
    {
        PA_DEBUG(( "%s: Failed to open busy device '%s'\n", __FUNCTION__, name ));
    }
    else
    {
        if( ret < 0 )
            PA_DEBUG(( "%s: Opened device '%s' ptr[%p] - result: [%d:%s]\n", __FUNCTION__, name, *pcmp, ret, alsa_snd_strerror(ret) ));
    }

    return ret;
}

static PaError FillInDevInfo( PaAlsaHostApiRepresentation *alsaApi, HwDevInfo* deviceHwInfo, int blocking,
        PaAlsaDeviceInfo* devInfo, int* devIdx )
{
    PaError result = 0;
    PaDeviceInfo *baseDeviceInfo = &devInfo->baseDeviceInfo;
    snd_pcm_t *pcm = NULL;
    PaUtilHostApiRepresentation *baseApi = &alsaApi->baseHostApiRep;

    PA_DEBUG(( "%s: Filling device info for: %s\n", __FUNCTION__, deviceHwInfo->name ));

    /* Zero fields */
    InitializeDeviceInfo( baseDeviceInfo );

    /* To determine device capabilities, we must open the device and query the
     * hardware parameter configuration space */

    /* Query capture */
    if( deviceHwInfo->hasCapture &&
        OpenPcm( &pcm, deviceHwInfo->alsaName, SND_PCM_STREAM_CAPTURE, blocking, 0 ) >= 0 )
    {
        if( GropeDevice( pcm, deviceHwInfo->isPlug, StreamDirection_In, blocking, devInfo ) != paNoError )
        {
            /* Error */
            PA_DEBUG(( "%s: Failed groping %s for capture\n", __FUNCTION__, deviceHwInfo->alsaName ));
            goto end;
        }
    }

    /* Query playback */
    if( deviceHwInfo->hasPlayback &&
        OpenPcm( &pcm, deviceHwInfo->alsaName, SND_PCM_STREAM_PLAYBACK, blocking, 0 ) >= 0 )
    {
        if( GropeDevice( pcm, deviceHwInfo->isPlug, StreamDirection_Out, blocking, devInfo ) != paNoError )
        {
            /* Error */
            PA_DEBUG(( "%s: Failed groping %s for playback\n", __FUNCTION__, deviceHwInfo->alsaName ));
            goto end;
        }
    }

    baseDeviceInfo->structVersion = 2;
    baseDeviceInfo->hostApi = alsaApi->hostApiIndex;
    baseDeviceInfo->name = deviceHwInfo->name;
    devInfo->alsaName = deviceHwInfo->alsaName;
    devInfo->isPlug = deviceHwInfo->isPlug;

    /* A: Storing pointer to PaAlsaDeviceInfo object as pointer to PaDeviceInfo object.
     * Should now be safe to add device info, unless the device supports neither capture nor playback
     */
    if( baseDeviceInfo->maxInputChannels > 0 || baseDeviceInfo->maxOutputChannels > 0 )
    {
        /* Make device default if there isn't already one or it is the ALSA "default" device */
        if( ( baseApi->info.defaultInputDevice == paNoDevice ||
            !strcmp( deviceHwInfo->alsaName, "default" ) ) && baseDeviceInfo->maxInputChannels > 0 )
        {
            baseApi->info.defaultInputDevice = *devIdx;
            PA_DEBUG(( "Default input device: %s\n", deviceHwInfo->name ));
        }
        if( ( baseApi->info.defaultOutputDevice == paNoDevice ||
            !strcmp( deviceHwInfo->alsaName, "default" ) ) && baseDeviceInfo->maxOutputChannels > 0 )
        {
            baseApi->info.defaultOutputDevice = *devIdx;
            PA_DEBUG(( "Default output device: %s\n", deviceHwInfo->name ));
        }
        PA_DEBUG(( "%s: Adding device %s: %d\n", __FUNCTION__, deviceHwInfo->name, *devIdx ));
        baseApi->deviceInfos[*devIdx] = (PaDeviceInfo *) devInfo;
        (*devIdx) += 1;
    }
    else
    {
        PA_DEBUG(( "%s: Skipped device: %s, all channels == 0\n", __FUNCTION__, deviceHwInfo->name ));
    }

end:
    return result;
}

/* Build PaDeviceInfo list, ignore devices for which we cannot determine capabilities (possibly busy, sigh) */
static PaError BuildDeviceList( PaAlsaHostApiRepresentation *alsaApi )
{
    PaUtilHostApiRepresentation *baseApi = &alsaApi->baseHostApiRep;
    PaAlsaDeviceInfo *deviceInfoArray;
    int cardIdx = -1, devIdx = 0;
    snd_ctl_card_info_t *cardInfo;
    PaError result = paNoError;
    size_t numDeviceNames = 0, maxDeviceNames = 1, i;
    HwDevInfo *hwDevInfos = NULL;
    snd_config_t *topNode = NULL;
    snd_pcm_info_t *pcmInfo;
    int res;
    int blocking = SND_PCM_NONBLOCK;
    int usePlughw = 0;
    char *hwPrefix = "";
    char alsaCardName[50];
#ifdef PA_ENABLE_DEBUG_OUTPUT
    PaTime startTime = PaUtil_GetTime();
#endif

    if( getenv( "PA_ALSA_INITIALIZE_BLOCK" ) && atoi( getenv( "PA_ALSA_INITIALIZE_BLOCK" ) ) )
        blocking = 0;

    /* If PA_ALSA_PLUGHW is 1 (non-zero), use the plughw: pcm throughout instead of hw: */
    if( getenv( "PA_ALSA_PLUGHW" ) && atoi( getenv( "PA_ALSA_PLUGHW" ) ) )
    {
        usePlughw = 1;
        hwPrefix = "plug";
        PA_DEBUG(( "%s: Using Plughw\n", __FUNCTION__ ));
    }

    /* These two will be set to the first working input and output device, respectively */
    baseApi->info.defaultInputDevice = paNoDevice;
    baseApi->info.defaultOutputDevice = paNoDevice;

    /* Gather info about hw devices

     * alsa_snd_card_next() modifies the integer passed to it to be:
     *      the index of the first card if the parameter is -1
     *      the index of the next card if the parameter is the index of a card
     *      -1 if there are no more cards
     *
     * The function itself returns 0 if it succeeded. */
    cardIdx = -1;
    alsa_snd_ctl_card_info_alloca( &cardInfo );
    alsa_snd_pcm_info_alloca( &pcmInfo );
    while( alsa_snd_card_next( &cardIdx ) == 0 && cardIdx >= 0 )
    {
        char *cardName;
        int devIdx = -1;
        snd_ctl_t *ctl;
        char buf[50];

        snprintf( alsaCardName, sizeof (alsaCardName), "hw:%d", cardIdx );

        /* Acquire name of card */
        if( alsa_snd_ctl_open( &ctl, alsaCardName, 0 ) < 0 )
        {
            /* Unable to open card :( */
            PA_DEBUG(( "%s: Unable to open device %s\n", __FUNCTION__, alsaCardName ));
            continue;
        }
        alsa_snd_ctl_card_info( ctl, cardInfo );

        PA_ENSURE( PaAlsa_StrDup( alsaApi, &cardName, alsa_snd_ctl_card_info_get_name( cardInfo )) );

        while( alsa_snd_ctl_pcm_next_device( ctl, &devIdx ) == 0 && devIdx >= 0 )
        {
            char *alsaDeviceName, *deviceName, *infoName;
            size_t len;
            int hasPlayback = 0, hasCapture = 0;

            snprintf( buf, sizeof (buf), "%s%s,%d", hwPrefix, alsaCardName, devIdx );

            /* Obtain info about this particular device */
            alsa_snd_pcm_info_set_device( pcmInfo, devIdx );
            alsa_snd_pcm_info_set_subdevice( pcmInfo, 0 );
            alsa_snd_pcm_info_set_stream( pcmInfo, SND_PCM_STREAM_CAPTURE );
            if( alsa_snd_ctl_pcm_info( ctl, pcmInfo ) >= 0 )
            {
                hasCapture = 1;
            }

            alsa_snd_pcm_info_set_stream( pcmInfo, SND_PCM_STREAM_PLAYBACK );
            if( alsa_snd_ctl_pcm_info( ctl, pcmInfo ) >= 0 )
            {
                hasPlayback = 1;
            }

            if( !hasPlayback && !hasCapture )
            {
                /* Error */
                continue;
            }

            infoName = SkipCardDetailsInName( (char *)alsa_snd_pcm_info_get_name( pcmInfo ), cardName );

            /* The length of the string written by snprintf plus terminating 0 */
            len = snprintf( NULL, 0, "%s: %s (%s)", cardName, infoName, buf ) + 1;
            PA_UNLESS( deviceName = (char *)PaUtil_GroupAllocateMemory( alsaApi->allocations, len ),
                    paInsufficientMemory );
            snprintf( deviceName, len, "%s: %s (%s)", cardName, infoName, buf );

            ++numDeviceNames;
            if( !hwDevInfos || numDeviceNames > maxDeviceNames )
            {
                maxDeviceNames *= 2;
                PA_UNLESS( hwDevInfos = (HwDevInfo *) realloc( hwDevInfos, maxDeviceNames * sizeof (HwDevInfo) ),
                        paInsufficientMemory );
            }

            PA_ENSURE( PaAlsa_StrDup( alsaApi, &alsaDeviceName, buf ) );

            hwDevInfos[ numDeviceNames - 1 ].alsaName = alsaDeviceName;
            hwDevInfos[ numDeviceNames - 1 ].name = deviceName;
            hwDevInfos[ numDeviceNames - 1 ].isPlug = usePlughw;
            hwDevInfos[ numDeviceNames - 1 ].hasPlayback = hasPlayback;
            hwDevInfos[ numDeviceNames - 1 ].hasCapture = hasCapture;
        }
        alsa_snd_ctl_close( ctl );
    }

    /* Iterate over plugin devices */
    if( NULL == (*alsa_snd_config) )
    {
        /* alsa_snd_config_update is called implicitly by some functions, if this hasn't happened snd_config will be NULL (bleh) */
        ENSURE_( alsa_snd_config_update(), paUnanticipatedHostError );
        PA_DEBUG(( "Updating snd_config\n" ));
    }
    assert( *alsa_snd_config );
    if( ( res = alsa_snd_config_search( *alsa_snd_config, "pcm", &topNode ) ) >= 0 )
    {
        snd_config_iterator_t i, next;

        alsa_snd_config_for_each( i, next, topNode )
        {
            const char *tpStr = "unknown", *idStr = NULL;
            int err = 0;

            char *alsaDeviceName, *deviceName;
            const HwDevInfo *predefined = NULL;
            snd_config_t *n = alsa_snd_config_iterator_entry( i ), * tp = NULL;;

            if( (err = alsa_snd_config_search( n, "type", &tp )) < 0 )
            {
                if( -ENOENT != err )
                {
                    ENSURE_(err, paUnanticipatedHostError);
                }
            }
            else
            {
                ENSURE_( alsa_snd_config_get_string( tp, &tpStr ), paUnanticipatedHostError );
            }
            ENSURE_( alsa_snd_config_get_id( n, &idStr ), paUnanticipatedHostError );
            if( IgnorePlugin( idStr ) )
            {
                PA_DEBUG(( "%s: Ignoring ALSA plugin device [%s] of type [%s]\n", __FUNCTION__, idStr, tpStr ));
                continue;
            }
            PA_DEBUG(( "%s: Found plugin [%s] of type [%s]\n", __FUNCTION__, idStr, tpStr ));

            PA_UNLESS( alsaDeviceName = (char*)PaUtil_GroupAllocateMemory( alsaApi->allocations,
                                                            strlen(idStr) + 6 ), paInsufficientMemory );
            strcpy( alsaDeviceName, idStr );
            PA_UNLESS( deviceName = (char*)PaUtil_GroupAllocateMemory( alsaApi->allocations,
                                                            strlen(idStr) + 1 ), paInsufficientMemory );
            strcpy( deviceName, idStr );

            ++numDeviceNames;
            if( !hwDevInfos || numDeviceNames > maxDeviceNames )
            {
                maxDeviceNames *= 2;
                PA_UNLESS( hwDevInfos = (HwDevInfo *) realloc( hwDevInfos, maxDeviceNames * sizeof (HwDevInfo) ),
                        paInsufficientMemory );
            }

            predefined = FindDeviceName( alsaDeviceName );

            hwDevInfos[numDeviceNames - 1].alsaName = alsaDeviceName;
            hwDevInfos[numDeviceNames - 1].name     = deviceName;
            hwDevInfos[numDeviceNames - 1].isPlug   = 1;

            if( predefined )
            {
                hwDevInfos[numDeviceNames - 1].hasPlayback = predefined->hasPlayback;
                hwDevInfos[numDeviceNames - 1].hasCapture  = predefined->hasCapture;
            }
            else
            {
                hwDevInfos[numDeviceNames - 1].hasPlayback = 1;
                hwDevInfos[numDeviceNames - 1].hasCapture  = 1;
            }
        }
    }
    else
        PA_DEBUG(( "%s: Iterating over ALSA plugins failed: %s\n", __FUNCTION__, alsa_snd_strerror( res ) ));

    /* allocate deviceInfo memory based on the number of devices */
    PA_UNLESS( baseApi->deviceInfos = (PaDeviceInfo**)PaUtil_GroupAllocateMemory(
            alsaApi->allocations, sizeof(PaDeviceInfo*) * (numDeviceNames) ), paInsufficientMemory );

    /* allocate all device info structs in a contiguous block */
    PA_UNLESS( deviceInfoArray = (PaAlsaDeviceInfo*)PaUtil_GroupAllocateMemory(
            alsaApi->allocations, sizeof(PaAlsaDeviceInfo) * numDeviceNames ), paInsufficientMemory );

    /* Loop over list of cards, filling in info. If a device is deemed unavailable (can't get name),
     * it's ignored.
     *
     * Note that we do this in two stages. This is a workaround owing to the fact that the 'dmix'
     * plugin may cause the underlying hardware device to be busy for a short while even after it
     * (dmix) is closed. The 'default' plugin may also point to the dmix plugin, so the same goes
     * for this.
     */
    PA_DEBUG(( "%s: Filling device info for %d devices\n", __FUNCTION__, numDeviceNames ));
    for( i = 0, devIdx = 0; i < numDeviceNames; ++i )
    {
        PaAlsaDeviceInfo* devInfo = &deviceInfoArray[i];
        HwDevInfo* hwInfo = &hwDevInfos[i];
        if( !strcmp( hwInfo->name, "dmix" ) || !strcmp( hwInfo->name, "default" ) )
        {
            continue;
        }

        PA_ENSURE( FillInDevInfo( alsaApi, hwInfo, blocking, devInfo, &devIdx ) );
    }
    assert( devIdx < numDeviceNames );
    /* Now inspect 'dmix' and 'default' plugins */
    for( i = 0; i < numDeviceNames; ++i )
    {
        PaAlsaDeviceInfo* devInfo = &deviceInfoArray[i];
        HwDevInfo* hwInfo = &hwDevInfos[i];
        if( strcmp( hwInfo->name, "dmix" ) && strcmp( hwInfo->name, "default" ) )
        {
            continue;
        }

        PA_ENSURE( FillInDevInfo( alsaApi, hwInfo, blocking, devInfo, &devIdx ) );
    }
    free( hwDevInfos );

    baseApi->info.deviceCount = devIdx;   /* Number of successfully queried devices */

#ifdef PA_ENABLE_DEBUG_OUTPUT
    PA_DEBUG(( "%s: Building device list took %f seconds\n", __FUNCTION__, PaUtil_GetTime() - startTime ));
#endif

end:
    return result;

error:
    /* No particular action */
    goto end;
}

/* Check against known device capabilities */
static PaError ValidateParameters( const PaStreamParameters *parameters, PaUtilHostApiRepresentation *hostApi, StreamDirection mode )
{
    PaError result = paNoError;
    int maxChans;
    const PaAlsaDeviceInfo *deviceInfo = NULL;
    assert( parameters );

    if( parameters->device != paUseHostApiSpecificDeviceSpecification )
    {
        assert( parameters->device < hostApi->info.deviceCount );
        PA_UNLESS( parameters->hostApiSpecificStreamInfo == NULL, paBadIODeviceCombination );
        deviceInfo = GetDeviceInfo( hostApi, parameters->device );
    }
    else
    {
        const PaAlsaStreamInfo *streamInfo = parameters->hostApiSpecificStreamInfo;

        PA_UNLESS( parameters->device == paUseHostApiSpecificDeviceSpecification, paInvalidDevice );
        PA_UNLESS( streamInfo->size == sizeof (PaAlsaStreamInfo) && streamInfo->version == 1,
                paIncompatibleHostApiSpecificStreamInfo );
        PA_UNLESS( streamInfo->deviceString != NULL, paInvalidDevice );

        /* Skip further checking */
        return paNoError;
    }

    assert( deviceInfo );
    assert( parameters->hostApiSpecificStreamInfo == NULL );
    maxChans = ( StreamDirection_In == mode ? deviceInfo->baseDeviceInfo.maxInputChannels :
        deviceInfo->baseDeviceInfo.maxOutputChannels );
    PA_UNLESS( parameters->channelCount <= maxChans, paInvalidChannelCount );

error:
    return result;
}

/* Given an open stream, what sample formats are available? */
static PaSampleFormat GetAvailableFormats( snd_pcm_t *pcm )
{
    PaSampleFormat available = 0;
    snd_pcm_hw_params_t *hwParams;
    alsa_snd_pcm_hw_params_alloca( &hwParams );

    alsa_snd_pcm_hw_params_any( pcm, hwParams );

    if( alsa_snd_pcm_hw_params_test_format( pcm, hwParams, SND_PCM_FORMAT_FLOAT ) >= 0)
        available |= paFloat32;

    if( alsa_snd_pcm_hw_params_test_format( pcm, hwParams, SND_PCM_FORMAT_S32 ) >= 0)
        available |= paInt32;

#ifdef PA_LITTLE_ENDIAN
    if( alsa_snd_pcm_hw_params_test_format( pcm, hwParams, SND_PCM_FORMAT_S24_3LE ) >= 0)
        available |= paInt24;
#elif defined PA_BIG_ENDIAN
    if( alsa_snd_pcm_hw_params_test_format( pcm, hwParams, SND_PCM_FORMAT_S24_3BE ) >= 0)
        available |= paInt24;
#endif

    if( alsa_snd_pcm_hw_params_test_format( pcm, hwParams, SND_PCM_FORMAT_S16 ) >= 0)
        available |= paInt16;

    if( alsa_snd_pcm_hw_params_test_format( pcm, hwParams, SND_PCM_FORMAT_U8 ) >= 0)
        available |= paUInt8;

    if( alsa_snd_pcm_hw_params_test_format( pcm, hwParams, SND_PCM_FORMAT_S8 ) >= 0)
        available |= paInt8;

    return available;
}

/* Output to console all formats supported by device */
static void LogAllAvailableFormats( snd_pcm_t *pcm )
{
    PaSampleFormat available = 0;
    snd_pcm_hw_params_t *hwParams;
    alsa_snd_pcm_hw_params_alloca( &hwParams );

    alsa_snd_pcm_hw_params_any( pcm, hwParams );

    PA_DEBUG(( " --- Supported Formats ---\n" ));

    if( alsa_snd_pcm_hw_params_test_format( pcm, hwParams, SND_PCM_FORMAT_S8 ) >= 0)
        PA_DEBUG(( "SND_PCM_FORMAT_S8\n" ));
    if( alsa_snd_pcm_hw_params_test_format( pcm, hwParams, SND_PCM_FORMAT_U8 ) >= 0)
        PA_DEBUG(( "SND_PCM_FORMAT_U8\n" ));

    if( alsa_snd_pcm_hw_params_test_format( pcm, hwParams, SND_PCM_FORMAT_S16_LE ) >= 0)
        PA_DEBUG(( "SND_PCM_FORMAT_S16_LE\n" ));
    if( alsa_snd_pcm_hw_params_test_format( pcm, hwParams, SND_PCM_FORMAT_S16_BE ) >= 0)
        PA_DEBUG(( "SND_PCM_FORMAT_S16_BE\n" ));

    if( alsa_snd_pcm_hw_params_test_format( pcm, hwParams, SND_PCM_FORMAT_U16_LE ) >= 0)
        PA_DEBUG(( "SND_PCM_FORMAT_U16_LE\n" ));
    if( alsa_snd_pcm_hw_params_test_format( pcm, hwParams, SND_PCM_FORMAT_U16_BE ) >= 0)
        PA_DEBUG(( "SND_PCM_FORMAT_U16_BE\n" ));

    if( alsa_snd_pcm_hw_params_test_format( pcm, hwParams, SND_PCM_FORMAT_S24_LE ) >= 0)
        PA_DEBUG(( "SND_PCM_FORMAT_S24_LE\n" ));
    if( alsa_snd_pcm_hw_params_test_format( pcm, hwParams, SND_PCM_FORMAT_S24_BE ) >= 0)
        PA_DEBUG(( "SND_PCM_FORMAT_S24_BE\n" ));

    if( alsa_snd_pcm_hw_params_test_format( pcm, hwParams, SND_PCM_FORMAT_U24_LE ) >= 0)
        PA_DEBUG(( "SND_PCM_FORMAT_U24_LE\n" ));
    if( alsa_snd_pcm_hw_params_test_format( pcm, hwParams, SND_PCM_FORMAT_U24_BE ) >= 0)
        PA_DEBUG(( "SND_PCM_FORMAT_U24_BE\n" ));

    if( alsa_snd_pcm_hw_params_test_format( pcm, hwParams, SND_PCM_FORMAT_FLOAT_LE ) >= 0)
        PA_DEBUG(( "SND_PCM_FORMAT_FLOAT_LE\n" ));
    if( alsa_snd_pcm_hw_params_test_format( pcm, hwParams, SND_PCM_FORMAT_FLOAT_BE ) >= 0)
        PA_DEBUG(( "SND_PCM_FORMAT_FLOAT_BE\n" ));

    if( alsa_snd_pcm_hw_params_test_format( pcm, hwParams, SND_PCM_FORMAT_FLOAT64_LE ) >= 0)
        PA_DEBUG(( "SND_PCM_FORMAT_FLOAT64_LE\n" ));
    if( alsa_snd_pcm_hw_params_test_format( pcm, hwParams, SND_PCM_FORMAT_FLOAT64_BE ) >= 0)
        PA_DEBUG(( "SND_PCM_FORMAT_FLOAT64_BE\n" ));

    if( alsa_snd_pcm_hw_params_test_format( pcm, hwParams, SND_PCM_FORMAT_IEC958_SUBFRAME_LE ) >= 0)
        PA_DEBUG(( "SND_PCM_FORMAT_IEC958_SUBFRAME_LE\n" ));
    if( alsa_snd_pcm_hw_params_test_format( pcm, hwParams, SND_PCM_FORMAT_IEC958_SUBFRAME_BE ) >= 0)
        PA_DEBUG(( "SND_PCM_FORMAT_IEC958_SUBFRAME_BE\n" ));

    if( alsa_snd_pcm_hw_params_test_format( pcm, hwParams, SND_PCM_FORMAT_MU_LAW ) >= 0)
        PA_DEBUG(( "SND_PCM_FORMAT_MU_LAW\n" ));
    if( alsa_snd_pcm_hw_params_test_format( pcm, hwParams, SND_PCM_FORMAT_A_LAW ) >= 0)
        PA_DEBUG(( "SND_PCM_FORMAT_A_LAW\n" ));

    if( alsa_snd_pcm_hw_params_test_format( pcm, hwParams, SND_PCM_FORMAT_IMA_ADPCM ) >= 0)
        PA_DEBUG(( "SND_PCM_FORMAT_IMA_ADPCM\n" ));
    if( alsa_snd_pcm_hw_params_test_format( pcm, hwParams, SND_PCM_FORMAT_MPEG ) >= 0)
        PA_DEBUG(( "SND_PCM_FORMAT_MPEG\n" ));

    if( alsa_snd_pcm_hw_params_test_format( pcm, hwParams, SND_PCM_FORMAT_GSM ) >= 0)
        PA_DEBUG(( "SND_PCM_FORMAT_GSM\n" ));
    if( alsa_snd_pcm_hw_params_test_format( pcm, hwParams, SND_PCM_FORMAT_SPECIAL ) >= 0)
        PA_DEBUG(( "SND_PCM_FORMAT_SPECIAL\n" ));

    if( alsa_snd_pcm_hw_params_test_format( pcm, hwParams, SND_PCM_FORMAT_S24_3LE ) >= 0)
        PA_DEBUG(( "SND_PCM_FORMAT_S24_3LE\n" ));
    if( alsa_snd_pcm_hw_params_test_format( pcm, hwParams, SND_PCM_FORMAT_S24_3BE ) >= 0)
        PA_DEBUG(( "SND_PCM_FORMAT_S24_3BE\n" ));

    if( alsa_snd_pcm_hw_params_test_format( pcm, hwParams, SND_PCM_FORMAT_U24_3LE ) >= 0)
        PA_DEBUG(( "SND_PCM_FORMAT_U24_3LE\n" ));
    if( alsa_snd_pcm_hw_params_test_format( pcm, hwParams, SND_PCM_FORMAT_U24_3BE ) >= 0)
        PA_DEBUG(( "SND_PCM_FORMAT_U24_3BE\n" ));

    if( alsa_snd_pcm_hw_params_test_format( pcm, hwParams, SND_PCM_FORMAT_S20_3LE ) >= 0)
        PA_DEBUG(( "SND_PCM_FORMAT_S20_3LE\n" ));
    if( alsa_snd_pcm_hw_params_test_format( pcm, hwParams, SND_PCM_FORMAT_S20_3BE ) >= 0)
        PA_DEBUG(( "SND_PCM_FORMAT_S20_3BE\n" ));

    if( alsa_snd_pcm_hw_params_test_format( pcm, hwParams, SND_PCM_FORMAT_U20_3LE ) >= 0)
        PA_DEBUG(( "SND_PCM_FORMAT_U20_3LE\n" ));
    if( alsa_snd_pcm_hw_params_test_format( pcm, hwParams, SND_PCM_FORMAT_U20_3BE ) >= 0)
        PA_DEBUG(( "SND_PCM_FORMAT_U20_3BE\n" ));

    if( alsa_snd_pcm_hw_params_test_format( pcm, hwParams, SND_PCM_FORMAT_S18_3LE ) >= 0)
        PA_DEBUG(( "SND_PCM_FORMAT_S18_3LE\n" ));
    if( alsa_snd_pcm_hw_params_test_format( pcm, hwParams, SND_PCM_FORMAT_S18_3BE ) >= 0)
        PA_DEBUG(( "SND_PCM_FORMAT_S18_3BE\n" ));

    if( alsa_snd_pcm_hw_params_test_format( pcm, hwParams, SND_PCM_FORMAT_U18_3LE ) >= 0)
        PA_DEBUG(( "SND_PCM_FORMAT_U18_3LE\n" ));
    if( alsa_snd_pcm_hw_params_test_format( pcm, hwParams, SND_PCM_FORMAT_U18_3BE ) >= 0)
        PA_DEBUG(( "SND_PCM_FORMAT_U18_3BE\n" ));

    if( alsa_snd_pcm_hw_params_test_format( pcm, hwParams, SND_PCM_FORMAT_S16 ) >= 0)
        PA_DEBUG(( "SND_PCM_FORMAT_S16\n" ));
    if( alsa_snd_pcm_hw_params_test_format( pcm, hwParams, SND_PCM_FORMAT_U16 ) >= 0)
        PA_DEBUG(( "SND_PCM_FORMAT_U16\n" ));

    if( alsa_snd_pcm_hw_params_test_format( pcm, hwParams, SND_PCM_FORMAT_S24 ) >= 0)
        PA_DEBUG(( "SND_PCM_FORMAT_S24\n" ));
    if( alsa_snd_pcm_hw_params_test_format( pcm, hwParams, SND_PCM_FORMAT_U24 ) >= 0)
        PA_DEBUG(( "SND_PCM_FORMAT_U24\n" ));

    if( alsa_snd_pcm_hw_params_test_format( pcm, hwParams, SND_PCM_FORMAT_S32 ) >= 0)
        PA_DEBUG(( "SND_PCM_FORMAT_S32\n" ));
    if( alsa_snd_pcm_hw_params_test_format( pcm, hwParams, SND_PCM_FORMAT_U32 ) >= 0)
        PA_DEBUG(( "SND_PCM_FORMAT_U32\n" ));

    if( alsa_snd_pcm_hw_params_test_format( pcm, hwParams, SND_PCM_FORMAT_FLOAT ) >= 0)
        PA_DEBUG(( "SND_PCM_FORMAT_FLOAT\n" ));
    if( alsa_snd_pcm_hw_params_test_format( pcm, hwParams, SND_PCM_FORMAT_FLOAT64 ) >= 0)
        PA_DEBUG(( "SND_PCM_FORMAT_FLOAT64\n" ));

    if( alsa_snd_pcm_hw_params_test_format( pcm, hwParams, SND_PCM_FORMAT_IEC958_SUBFRAME ) >= 0)
        PA_DEBUG(( "SND_PCM_FORMAT_IEC958_SUBFRAME\n" ));

    PA_DEBUG(( " -------------------------\n" ));
}

static snd_pcm_format_t Pa2AlsaFormat( PaSampleFormat paFormat )
{
    switch( paFormat )
    {
        case paFloat32:
            return SND_PCM_FORMAT_FLOAT;

        case paInt16:
            return SND_PCM_FORMAT_S16;

        case paInt24:
#ifdef PA_LITTLE_ENDIAN
            return SND_PCM_FORMAT_S24_3LE;
#elif defined PA_BIG_ENDIAN
            return SND_PCM_FORMAT_S24_3BE;
#endif

        case paInt32:
            return SND_PCM_FORMAT_S32;

        case paInt8:
            return SND_PCM_FORMAT_S8;

        case paUInt8:
            return SND_PCM_FORMAT_U8;

        default:
            return SND_PCM_FORMAT_UNKNOWN;
    }
}

/** Open an ALSA pcm handle.
 *
 * The device to be open can be specified by name in a custom PaAlsaStreamInfo struct, or it will be by
 * the Portaudio device number supplied in the stream parameters.
 */
static PaError AlsaOpen( const PaUtilHostApiRepresentation *hostApi, const PaStreamParameters *params, StreamDirection
        streamDir, snd_pcm_t **pcm )
{
    PaError result = paNoError;
    int ret;
    const char* deviceName = "";
    const PaAlsaDeviceInfo *deviceInfo = NULL;
    PaAlsaStreamInfo *streamInfo = (PaAlsaStreamInfo *)params->hostApiSpecificStreamInfo;

    if( !streamInfo )
    {
        deviceInfo = GetDeviceInfo( hostApi, params->device );
        deviceName = deviceInfo->alsaName;
    }
    else
        deviceName = streamInfo->deviceString;

    PA_DEBUG(( "%s: Opening device %s\n", __FUNCTION__, deviceName ));
    if( (ret = OpenPcm( pcm, deviceName, streamDir == StreamDirection_In ? SND_PCM_STREAM_CAPTURE : SND_PCM_STREAM_PLAYBACK,
                    SND_PCM_NONBLOCK, 1 )) < 0 )
    {
        /* Not to be closed */
        *pcm = NULL;
        ENSURE_( ret, -EBUSY == ret ? paDeviceUnavailable : paBadIODeviceCombination );
    }
    ENSURE_( alsa_snd_pcm_nonblock( *pcm, 0 ), paUnanticipatedHostError );

end:
    return result;

error:
    goto end;
}

static PaError TestParameters( const PaUtilHostApiRepresentation *hostApi, const PaStreamParameters *parameters,
        double sampleRate, StreamDirection streamDir )
{
    PaError result = paNoError;
    snd_pcm_t *pcm = NULL;
    PaSampleFormat availableFormats;
    /* We are able to adapt to a number of channels less than what the device supports */
    unsigned int numHostChannels;
    PaSampleFormat hostFormat;
    snd_pcm_hw_params_t *hwParams;
    alsa_snd_pcm_hw_params_alloca( &hwParams );

    if( !parameters->hostApiSpecificStreamInfo )
    {
        const PaAlsaDeviceInfo *devInfo = GetDeviceInfo( hostApi, parameters->device );
        numHostChannels = PA_MAX( parameters->channelCount, StreamDirection_In == streamDir ?
                devInfo->minInputChannels : devInfo->minOutputChannels );
    }
    else
        numHostChannels = parameters->channelCount;

    PA_ENSURE( AlsaOpen( hostApi, parameters, streamDir, &pcm ) );

    alsa_snd_pcm_hw_params_any( pcm, hwParams );

    if( SetApproximateSampleRate( pcm, hwParams, sampleRate ) < 0 )
    {
        result = paInvalidSampleRate;
        goto error;
    }

    if( alsa_snd_pcm_hw_params_set_channels( pcm, hwParams, numHostChannels ) < 0 )
    {
        result = paInvalidChannelCount;
        goto error;
    }

    /* See if we can find a best possible match */
    availableFormats = GetAvailableFormats( pcm );
    PA_ENSURE( hostFormat = PaUtil_SelectClosestAvailableFormat( availableFormats, parameters->sampleFormat ) );

    /* Some specific hardware (reported: Audio8 DJ) can fail with assertion during this step. */
    ENSURE_( alsa_snd_pcm_hw_params_set_format( pcm, hwParams, Pa2AlsaFormat( hostFormat ) ), paUnanticipatedHostError );

    {
        /* It happens that this call fails because the device is busy */
        int ret = 0;
        if( ( ret = alsa_snd_pcm_hw_params( pcm, hwParams ) ) < 0 )
        {
            if( -EINVAL == ret )
            {
                /* Don't know what to return here */
                result = paBadIODeviceCombination;
                goto error;
            }
            else if( -EBUSY == ret )
            {
                result = paDeviceUnavailable;
                PA_DEBUG(( "%s: Device is busy\n", __FUNCTION__ ));
            }
            else
            {
                result = paUnanticipatedHostError;
            }

            ENSURE_( ret, result );
        }
    }

end:
    if( pcm )
    {
        alsa_snd_pcm_close( pcm );
    }
    return result;

error:
    goto end;
}

static PaError IsFormatSupported( struct PaUtilHostApiRepresentation *hostApi,
                                  const PaStreamParameters *inputParameters,
                                  const PaStreamParameters *outputParameters,
                                  double sampleRate )
{
    int inputChannelCount = 0, outputChannelCount = 0;
    PaSampleFormat inputSampleFormat, outputSampleFormat;
    PaError result = paFormatIsSupported;

    if( inputParameters )
    {
        PA_ENSURE( ValidateParameters( inputParameters, hostApi, StreamDirection_In ) );

        inputChannelCount = inputParameters->channelCount;
        inputSampleFormat = inputParameters->sampleFormat;
    }

    if( outputParameters )
    {
        PA_ENSURE( ValidateParameters( outputParameters, hostApi, StreamDirection_Out ) );

        outputChannelCount = outputParameters->channelCount;
        outputSampleFormat = outputParameters->sampleFormat;
    }

    if( inputChannelCount )
    {
        if( ( result = TestParameters( hostApi, inputParameters, sampleRate, StreamDirection_In ) )
                != paNoError )
            goto error;
    }
    if ( outputChannelCount )
    {
        if( ( result = TestParameters( hostApi, outputParameters, sampleRate, StreamDirection_Out ) )
                != paNoError )
            goto error;
    }

    return paFormatIsSupported;

error:
    return result;
}


static PaError PaAlsaStreamComponent_Initialize( PaAlsaStreamComponent *self, PaAlsaHostApiRepresentation *alsaApi,
        const PaStreamParameters *params, StreamDirection streamDir, int callbackMode )
{
    PaError result = paNoError;
    PaSampleFormat userSampleFormat = params->sampleFormat, hostSampleFormat = paNoError;
    assert( params->channelCount > 0 );

    /* Make sure things have an initial value */
    memset( self, 0, sizeof (PaAlsaStreamComponent) );

    if( NULL == params->hostApiSpecificStreamInfo )
    {
        const PaAlsaDeviceInfo *devInfo = GetDeviceInfo( &alsaApi->baseHostApiRep, params->device );
        self->numHostChannels = PA_MAX( params->channelCount, StreamDirection_In == streamDir ? devInfo->minInputChannels
                : devInfo->minOutputChannels );
        self->deviceIsPlug = devInfo->isPlug;
        PA_DEBUG(( "%s: Host Chans %c %i\n", __FUNCTION__, streamDir == StreamDirection_In ? 'C' : 'P', self->numHostChannels ));
    }
    else
    {
        /* We're blissfully unaware of the minimum channelCount */
        self->numHostChannels = params->channelCount;
        /* Check if device name does not start with hw: to determine if it is a 'plug' device */
        if( strncmp( "hw:", ((PaAlsaStreamInfo *)params->hostApiSpecificStreamInfo)->deviceString, 3 ) != 0  )
            self->deviceIsPlug = 1; /* An Alsa plug device, not a direct hw device */
    }
    if( self->deviceIsPlug && alsaApi->alsaLibVersion < ALSA_VERSION_INT( 1, 0, 16 ) )
        self->useReventFix = 1; /* Prior to Alsa1.0.16, plug devices may stutter without this fix */

    self->device = params->device;

    PA_ENSURE( AlsaOpen( &alsaApi->baseHostApiRep, params, streamDir, &self->pcm ) );
    self->nfds = alsa_snd_pcm_poll_descriptors_count( self->pcm );

    PA_ENSURE( hostSampleFormat = PaUtil_SelectClosestAvailableFormat( GetAvailableFormats( self->pcm ), userSampleFormat ) );

    self->hostSampleFormat = hostSampleFormat;
    self->nativeFormat = Pa2AlsaFormat( hostSampleFormat );
    self->hostInterleaved = self->userInterleaved = !( userSampleFormat & paNonInterleaved );
    self->numUserChannels = params->channelCount;
    self->streamDir = streamDir;
    self->canMmap = 0;
    self->nonMmapBuffer = NULL;
    self->nonMmapBufferSize = 0;

    if( !callbackMode && !self->userInterleaved )
    {
        /* Pre-allocate non-interleaved user provided buffers */
        PA_UNLESS( self->userBuffers = PaUtil_AllocateMemory( sizeof (void *) * self->numUserChannels ),
                paInsufficientMemory );
    }

error:

    /* Log all available formats. */
    if ( hostSampleFormat == paSampleFormatNotSupported )
    {
        LogAllAvailableFormats( self->pcm );
        PA_DEBUG(( "%s: Please provide the log output to PortAudio developers, your hardware does not have any sample format implemented yet.\n", __FUNCTION__ ));
    }

    return result;
}

static void PaAlsaStreamComponent_Terminate( PaAlsaStreamComponent *self )
{
    alsa_snd_pcm_close( self->pcm );
    PaUtil_FreeMemory( self->userBuffers ); /* (Ptr can be NULL; PaUtil_FreeMemory includes a NULL check) */
    PaUtil_FreeMemory( self->nonMmapBuffer );
}

/*
static int nearbyint_(float value) {
    if(  value - (int)value > .5 )
        return (int)ceil( value );
    return (int)floor( value );
}
*/

/** Initiate configuration, preparing for determining a period size suitable for both capture and playback components.
 *
 */
static PaError PaAlsaStreamComponent_InitialConfigure( PaAlsaStreamComponent *self, const PaStreamParameters *params,
        int primeBuffers, snd_pcm_hw_params_t *hwParams, double *sampleRate )
{
    /* Configuration consists of setting all of ALSA's parameters.
     * These parameters come in two flavors: hardware parameters
     * and software paramters.  Hardware parameters will affect
     * the way the device is initialized, software parameters
     * affect the way ALSA interacts with me, the user-level client.
     */

    PaError result = paNoError;
    snd_pcm_access_t accessMode, alternateAccessMode;
    int dir = 0;
    snd_pcm_t *pcm = self->pcm;
    double sr = *sampleRate;
    unsigned int minPeriods = 2;

    /* self->framesPerPeriod = framesPerHostBuffer; */

    /* ... fill up the configuration space with all possibile
     * combinations of parameters this device will accept */
    ENSURE_( alsa_snd_pcm_hw_params_any( pcm, hwParams ), paUnanticipatedHostError );

    ENSURE_( alsa_snd_pcm_hw_params_set_periods_integer( pcm, hwParams ), paUnanticipatedHostError );
    /* I think there should be at least 2 periods (even though ALSA doesn't appear to enforce this) */
    dir = 0;
    ENSURE_( alsa_snd_pcm_hw_params_set_periods_min( pcm, hwParams, &minPeriods, &dir ), paUnanticipatedHostError );

    if( self->userInterleaved )
    {
        accessMode          = SND_PCM_ACCESS_MMAP_INTERLEAVED;
        alternateAccessMode = SND_PCM_ACCESS_MMAP_NONINTERLEAVED;

        /* test if MMAP supported */
        self->canMmap = alsa_snd_pcm_hw_params_test_access( pcm, hwParams, accessMode ) >= 0 ||
                        alsa_snd_pcm_hw_params_test_access( pcm, hwParams, alternateAccessMode ) >= 0;

        PA_DEBUG(( "%s: device MMAP SND_PCM_ACCESS_MMAP_INTERLEAVED: %s\n", __FUNCTION__, ( alsa_snd_pcm_hw_params_test_access( pcm, hwParams, accessMode ) >= 0 ? "YES" : "NO" ) ));
        PA_DEBUG(( "%s: device MMAP SND_PCM_ACCESS_MMAP_NONINTERLEAVED: %s\n", __FUNCTION__, ( alsa_snd_pcm_hw_params_test_access( pcm, hwParams, alternateAccessMode ) >= 0 ? "YES" : "NO" ) ));

        if( !self->canMmap )
        {
            accessMode          = SND_PCM_ACCESS_RW_INTERLEAVED;
            alternateAccessMode = SND_PCM_ACCESS_RW_NONINTERLEAVED;
        }
    }
    else
    {
        accessMode          = SND_PCM_ACCESS_MMAP_NONINTERLEAVED;
        alternateAccessMode = SND_PCM_ACCESS_MMAP_INTERLEAVED;

        /* test if MMAP supported */
        self->canMmap = alsa_snd_pcm_hw_params_test_access( pcm, hwParams, accessMode ) >= 0 ||
                        alsa_snd_pcm_hw_params_test_access( pcm, hwParams, alternateAccessMode ) >= 0;

        PA_DEBUG((" %s: device MMAP SND_PCM_ACCESS_MMAP_NONINTERLEAVED: %s\n", __FUNCTION__, ( alsa_snd_pcm_hw_params_test_access( pcm, hwParams, accessMode ) >= 0 ? "YES" : "NO" ) ));
        PA_DEBUG(( "%s: device MMAP SND_PCM_ACCESS_MMAP_INTERLEAVED: %s\n", __FUNCTION__, ( alsa_snd_pcm_hw_params_test_access( pcm, hwParams, alternateAccessMode ) >= 0 ? "YES" : "NO" ) ));

        if( !self->canMmap )
        {
            accessMode          = SND_PCM_ACCESS_RW_NONINTERLEAVED;
            alternateAccessMode = SND_PCM_ACCESS_RW_INTERLEAVED;
        }
    }

    PA_DEBUG(( "%s: device can MMAP: %s\n", __FUNCTION__, ( self->canMmap ? "YES" : "NO" ) ));

    /* If requested access mode fails, try alternate mode */
    if( alsa_snd_pcm_hw_params_set_access( pcm, hwParams, accessMode ) < 0 )
    {
        int err = 0;
        if( ( err = alsa_snd_pcm_hw_params_set_access( pcm, hwParams, alternateAccessMode )) < 0 )
        {
            result = paUnanticipatedHostError;
            PaUtil_SetLastHostErrorInfo( paALSA, err, alsa_snd_strerror( err ) );
            goto error;
        }
        /* Flip mode */
        self->hostInterleaved = !self->userInterleaved;
    }

    /* Some specific hardware (reported: Audio8 DJ) can fail with assertion during this step. */
    ENSURE_( alsa_snd_pcm_hw_params_set_format( pcm, hwParams, self->nativeFormat ), paUnanticipatedHostError );

    if( ( result = SetApproximateSampleRate( pcm, hwParams, sr )) != paUnanticipatedHostError )
    {
        ENSURE_( GetExactSampleRate( hwParams, &sr ), paUnanticipatedHostError );
        if( result == paInvalidSampleRate ) /* From the SetApproximateSampleRate() call above */
        { /* The sample rate was returned as 'out of tolerance' of the one requested */
            PA_DEBUG(( "%s: Wanted %.3f, closest sample rate was %.3f\n", __FUNCTION__, sampleRate, sr ));
            PA_ENSURE( paInvalidSampleRate );
        }
    }
    else
    {
       PA_ENSURE( paUnanticipatedHostError );
    }

    ENSURE_( alsa_snd_pcm_hw_params_set_channels( pcm, hwParams, self->numHostChannels ), paInvalidChannelCount );

    *sampleRate = sr;

end:
    return result;

error:
    /* No particular action */
    goto end;
}

/** Finish the configuration of the component's ALSA device.
 *
 * As part of this method, the component's alsaBufferSize attribute will be set.
 * @param latency: The latency for this component.
 */
static PaError PaAlsaStreamComponent_FinishConfigure( PaAlsaStreamComponent *self, snd_pcm_hw_params_t* hwParams,
        const PaStreamParameters *params, int primeBuffers, double sampleRate, PaTime* latency )
{
    PaError result = paNoError;
    snd_pcm_sw_params_t* swParams;
    snd_pcm_uframes_t bufSz = 0;
    *latency = -1.;

    alsa_snd_pcm_sw_params_alloca( &swParams );

    bufSz = params->suggestedLatency * sampleRate + self->framesPerPeriod;
    ENSURE_( alsa_snd_pcm_hw_params_set_buffer_size_near( self->pcm, hwParams, &bufSz ), paUnanticipatedHostError );

    /* Set the parameter
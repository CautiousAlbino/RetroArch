/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2010-2014 - Hans-Kristian Arntzen
 *  Copyright (C) 2011-2015 - Daniel De Matteis
 * 
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include <string.h>
#include <string/string_list.h>
#include "audio_driver.h"
#include "audio_utils.h"
#include "audio_thread_wrapper.h"
#include "../driver.h"
#include "../general.h"
#include "../retroarch.h"
#include "../runloop.h"

static const audio_driver_t *audio_drivers[] = {
#ifdef HAVE_ALSA
   &audio_alsa,
#ifndef __QNX__
   &audio_alsathread,
#endif
#endif
#if defined(HAVE_OSS) || defined(HAVE_OSS_BSD)
   &audio_oss,
#endif
#ifdef HAVE_RSOUND
   &audio_rsound,
#endif
#ifdef HAVE_COREAUDIO
   &audio_coreaudio,
#endif
#ifdef HAVE_AL
   &audio_openal,
#endif
#ifdef HAVE_SL
   &audio_opensl,
#endif
#ifdef HAVE_ROAR
   &audio_roar,
#endif
#ifdef HAVE_JACK
   &audio_jack,
#endif
#if defined(HAVE_SDL) || defined(HAVE_SDL2)
   &audio_sdl,
#endif
#ifdef HAVE_XAUDIO
   &audio_xa,
#endif
#ifdef HAVE_DSOUND
   &audio_dsound,
#endif
#ifdef HAVE_PULSE
   &audio_pulse,
#endif
#ifdef __CELLOS_LV2__
   &audio_ps3,
#endif
#ifdef XENON
   &audio_xenon360,
#endif
#ifdef GEKKO
   &audio_gx,
#endif
#ifdef EMSCRIPTEN
   &audio_rwebaudio,
#endif
#ifdef PSP
   &audio_psp1,
#endif   
   &audio_null,
   NULL,
};

/**
 * compute_audio_buffer_statistics:
 *
 * Computes audio buffer statistics.
 *
 **/
static void compute_audio_buffer_statistics(void)
{
   unsigned i, low_water_size, high_water_size, avg, stddev;
   float avg_filled, deviation;
   uint64_t accum = 0, accum_var = 0;
   unsigned low_water_count = 0, high_water_count = 0;
   unsigned samples = min(g_runloop.measure_data.buffer_free_samples_count,
         AUDIO_BUFFER_FREE_SAMPLES_COUNT);

   if (samples < 3)
      return;

   for (i = 1; i < samples; i++)
      accum += g_runloop.measure_data.buffer_free_samples[i];

   avg = accum / (samples - 1);

   for (i = 1; i < samples; i++)
   {
      int diff = avg - g_runloop.measure_data.buffer_free_samples[i];
      accum_var += diff * diff;
   }

   stddev          = (unsigned)sqrt((double)accum_var / (samples - 2));
   avg_filled      = 1.0f - (float)avg / g_extern.audio_data.driver_buffer_size;
   deviation       = (float)stddev / g_extern.audio_data.driver_buffer_size;

   low_water_size  = g_extern.audio_data.driver_buffer_size * 3 / 4;
   high_water_size = g_extern.audio_data.driver_buffer_size / 4;

   for (i = 1; i < samples; i++)
   {
      if (g_runloop.measure_data.buffer_free_samples[i] >= low_water_size)
         low_water_count++;
      else if (g_runloop.measure_data.buffer_free_samples[i] <= high_water_size)
         high_water_count++;
   }

   RARCH_LOG("Average audio buffer saturation: %.2f %%, standard deviation (percentage points): %.2f %%.\n",
         avg_filled * 100.0, deviation * 100.0);
   RARCH_LOG("Amount of time spent close to underrun: %.2f %%. Close to blocking: %.2f %%.\n",
         (100.0 * low_water_count) / (samples - 1),
         (100.0 * high_water_count) / (samples - 1));
}

/**
 * audio_driver_find_handle:
 * @idx                : index of driver to get handle to.
 *
 * Returns: handle to audio driver at index. Can be NULL
 * if nothing found.
 **/
const void *audio_driver_find_handle(int idx)
{
   const void *drv = audio_drivers[idx];
   if (!drv)
      return NULL;
   return drv;
}

/**
 * audio_driver_find_ident:
 * @idx                : index of driver to get handle to.
 *
 * Returns: Human-readable identifier of audio driver at index. Can be NULL
 * if nothing found.
 **/
const char *audio_driver_find_ident(int idx)
{
   const audio_driver_t *drv = audio_drivers[idx];
   if (!drv)
      return NULL;
   return drv->ident;
}

/**
 * config_get_audio_driver_options:
 *
 * Get an enumerated list of all audio driver names, separated by '|'.
 *
 * Returns: string listing of all audio driver names, separated by '|'.
 **/
const char* config_get_audio_driver_options(void)
{
   union string_list_elem_attr attr;
   unsigned i;
   char *options = NULL;
   int options_len = 0;
   struct string_list *options_l = string_list_new();

   attr.i = 0;

   if (!options_l)
      return NULL;

   for (i = 0; audio_driver_find_handle(i); i++)
   {
      const char *opt = audio_driver_find_ident(i);
      options_len += strlen(opt) + 1;
      string_list_append(options_l, opt, attr);
   }

   options = (char*)calloc(options_len, sizeof(char));

   if (!options)
   {
      string_list_free(options_l);
      options_l = NULL;
      return NULL;
   }

   string_list_join_concat(options, options_len, options_l, "|");

   string_list_free(options_l);
   options_l = NULL;

   return options;
}

void find_audio_driver(void)
{
   int i = find_driver_index("audio_driver", g_settings.audio.driver);
   if (i >= 0)
      driver.audio = (const audio_driver_t*)audio_driver_find_handle(i);
   else
   {
      unsigned d;
      RARCH_ERR("Couldn't find any audio driver named \"%s\"\n",
            g_settings.audio.driver);
      RARCH_LOG_OUTPUT("Available audio drivers are:\n");
      for (d = 0; audio_driver_find_handle(d); d++)
         RARCH_LOG_OUTPUT("\t%s\n", audio_driver_find_ident(d));
      RARCH_WARN("Going to default to first audio driver...\n");

      driver.audio = (const audio_driver_t*)audio_driver_find_handle(0);

      if (!driver.audio)
         rarch_fail(1, "find_audio_driver()");
   }
}

void uninit_audio(void)
{
   if (driver.audio_data && driver.audio)
      driver.audio->free(driver.audio_data);

   free(g_extern.audio_data.conv_outsamples);
   g_extern.audio_data.conv_outsamples = NULL;
   g_extern.audio_data.data_ptr        = 0;

   free(g_extern.audio_data.rewind_buf);
   g_extern.audio_data.rewind_buf = NULL;

   if (!g_settings.audio.enable)
   {
      driver.audio_active = false;
      return;
   }

   rarch_resampler_freep(&driver.resampler,
         &driver.resampler_data);

   free(g_extern.audio_data.data);
   g_extern.audio_data.data = NULL;

   free(g_extern.audio_data.outsamples);
   g_extern.audio_data.outsamples = NULL;

   rarch_main_command(RARCH_CMD_DSP_FILTER_DEINIT);

   compute_audio_buffer_statistics();
}

void init_audio(void)
{
   size_t outsamples_max, max_bufsamples = AUDIO_CHUNK_SIZE_NONBLOCKING * 2;

   audio_convert_init_simd();

   /* Resource leaks will follow if audio is initialized twice. */
   if (driver.audio_data)
      return;

   /* Accomodate rewind since at some point we might have two full buffers. */
   outsamples_max = max_bufsamples * AUDIO_MAX_RATIO * 
      g_settings.slowmotion_ratio;

   /* Used for recording even if audio isn't enabled. */
   rarch_assert(g_extern.audio_data.conv_outsamples =
         (int16_t*)malloc(outsamples_max * sizeof(int16_t)));

   g_extern.audio_data.block_chunk_size    = AUDIO_CHUNK_SIZE_BLOCKING;
   g_extern.audio_data.nonblock_chunk_size = AUDIO_CHUNK_SIZE_NONBLOCKING;
   g_extern.audio_data.chunk_size          = 
      g_extern.audio_data.block_chunk_size;

   /* Needs to be able to hold full content of a full max_bufsamples
    * in addition to its own. */
   rarch_assert(g_extern.audio_data.rewind_buf = (int16_t*)
         malloc(max_bufsamples * sizeof(int16_t)));
   g_extern.audio_data.rewind_size             = max_bufsamples;

   if (!g_settings.audio.enable)
   {
      driver.audio_active = false;
      return;
   }

   find_audio_driver();
#ifdef HAVE_THREADS
   if (g_extern.system.audio_callback.callback)
   {
      RARCH_LOG("Starting threaded audio driver ...\n");
      if (!rarch_threaded_audio_init(&driver.audio, &driver.audio_data,
               *g_settings.audio.device ? g_settings.audio.device : NULL,
               g_settings.audio.out_rate, g_settings.audio.latency,
               driver.audio))
      {
         RARCH_ERR("Cannot open threaded audio driver ... Exiting ...\n");
         rarch_fail(1, "init_audio()");
      }
   }
   else
#endif
   {
      driver.audio_data = driver.audio->init(*g_settings.audio.device ?
            g_settings.audio.device : NULL,
            g_settings.audio.out_rate, g_settings.audio.latency);
   }

   if (!driver.audio_data)
   {
      RARCH_ERR("Failed to initialize audio driver. Will continue without audio.\n");
      driver.audio_active = false;
   }

   g_extern.audio_data.use_float = false;
   if (driver.audio_active && driver.audio->use_float(driver.audio_data))
      g_extern.audio_data.use_float = true;

   if (!g_settings.audio.sync && driver.audio_active)
   {
      rarch_main_command(RARCH_CMD_AUDIO_SET_NONBLOCKING_STATE);
      g_extern.audio_data.chunk_size = 
         g_extern.audio_data.nonblock_chunk_size;
   }

   if (g_extern.audio_data.in_rate <= 0.0f)
   {
      /* Should never happen. */
      RARCH_WARN("Input rate is invalid (%.3f Hz). Using output rate (%u Hz).\n",
            g_extern.audio_data.in_rate, g_settings.audio.out_rate);
      g_extern.audio_data.in_rate = g_settings.audio.out_rate;
   }

   g_extern.audio_data.orig_src_ratio =
      g_extern.audio_data.src_ratio =
      (double)g_settings.audio.out_rate / g_extern.audio_data.in_rate;

   if (!rarch_resampler_realloc(&driver.resampler_data,
            &driver.resampler,
         g_settings.audio.resampler, g_extern.audio_data.orig_src_ratio))
   {
      RARCH_ERR("Failed to initialize resampler \"%s\".\n",
            g_settings.audio.resampler);
      driver.audio_active = false;
   }

   rarch_assert(g_extern.audio_data.data = (float*)
         malloc(max_bufsamples * sizeof(float)));

   g_extern.audio_data.data_ptr = 0;

   rarch_assert(g_settings.audio.out_rate <
         g_extern.audio_data.in_rate * AUDIO_MAX_RATIO);
   rarch_assert(g_extern.audio_data.outsamples = (float*)
         malloc(outsamples_max * sizeof(float)));

   g_extern.audio_data.rate_control = false;
   if (!g_extern.system.audio_callback.callback && driver.audio_active &&
         g_settings.audio.rate_control)
   {
      if (driver.audio->buffer_size && driver.audio->write_avail)
      {
         g_extern.audio_data.driver_buffer_size = 
            driver.audio->buffer_size(driver.audio_data);
         g_extern.audio_data.rate_control = true;
      }
      else
         RARCH_WARN("Audio rate control was desired, but driver does not support needed features.\n");
   }

   rarch_main_command(RARCH_CMD_DSP_FILTER_DEINIT);

   g_runloop.measure_data.buffer_free_samples_count = 0;

   if (driver.audio_active && !g_settings.audio.mute_enable &&
         g_extern.system.audio_callback.callback)
   {
      /* Threaded driver is initially stopped. */
      driver.audio->start(driver.audio_data);
   }
}

bool audio_driver_mute_toggle(void)
{
   if (!driver.audio_data || !driver.audio_active)
      return false;

   g_settings.audio.mute_enable = !g_settings.audio.mute_enable;

   if (g_settings.audio.mute_enable)
      rarch_main_command(RARCH_CMD_AUDIO_STOP);
   else if (!rarch_main_command(RARCH_CMD_AUDIO_START))
   {
      driver.audio_active = false;
      return false;
   }

   return true;
}

/*
 * audio_driver_readjust_input_rate:
 *
 * Readjust the audio input rate.
 */
void audio_driver_readjust_input_rate(void)
{
   double direction, adjust;
   int half_size, delta_mid, avail;
   unsigned write_idx;

   avail = driver.audio->write_avail(driver.audio_data);

#if 0
   RARCH_LOG_OUTPUT("Audio buffer is %u%% full\n",
         (unsigned)(100 - (avail * 100) / g_extern.audio_data.driver_buffer_size));
#endif

   write_idx   = g_runloop.measure_data.buffer_free_samples_count++ &
      (AUDIO_BUFFER_FREE_SAMPLES_COUNT - 1);
   half_size   = g_extern.audio_data.driver_buffer_size / 2;
   delta_mid   = avail - half_size;
   direction   = (double)delta_mid / half_size;
   adjust      = 1.0 + g_settings.audio.rate_control_delta * direction;

   g_runloop.measure_data.buffer_free_samples[write_idx] = avail;
   g_extern.audio_data.src_ratio = g_extern.audio_data.orig_src_ratio * adjust;

#if 0
   RARCH_LOG_OUTPUT("New rate: %lf, Orig rate: %lf\n",
         g_extern.audio_data.src_ratio, g_extern.audio_data.orig_src_ratio);
#endif
}

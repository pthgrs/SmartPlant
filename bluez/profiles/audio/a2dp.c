/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2006-2010  Nokia Corporation
 *  Copyright (C) 2004-2010  Marcel Holtmann <marcel@holtmann.org>
 *  Copyright (C) 2011  BMW Car IT GmbH. All rights reserved.
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdlib.h>
#include <errno.h>

#include <dbus/dbus.h>
#include <glib.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/sdp.h>
#include <bluetooth/sdp_lib.h>

#include "../src/adapter.h"

#include "log.h"
#include "device.h"
#include "manager.h"
#include "avdtp.h"
#include "sink.h"
#include "source.h"
#include "a2dp.h"
#include "a2dp-codecs.h"
#include "sdpd.h"

/* The duration that streams without users are allowed to stay in
 * STREAMING state. */
#define SUSPEND_TIMEOUT 5
#define RECONFIGURE_TIMEOUT 500

struct a2dp_sep {
	struct a2dp_server *server;
	struct a2dp_endpoint *endpoint;
	uint8_t type;
	uint8_t codec;
	struct avdtp_local_sep *lsep;
	struct avdtp *session;
	struct avdtp_stream *stream;
	guint suspend_timer;
	gboolean delay_reporting;
	gboolean locked;
	gboolean suspending;
	gboolean starting;
	void *user_data;
	GDestroyNotify destroy;
};

struct a2dp_setup_cb {
	struct a2dp_setup *setup;
	a2dp_select_cb_t select_cb;
	a2dp_config_cb_t config_cb;
	a2dp_stream_cb_t resume_cb;
	a2dp_stream_cb_t suspend_cb;
	guint source_id;
	void *user_data;
	unsigned int id;
};

struct a2dp_setup {
	struct audio_device *dev;
	struct avdtp *session;
	struct a2dp_sep *sep;
	struct avdtp_remote_sep *rsep;
	struct avdtp_stream *stream;
	struct avdtp_error *err;
	avdtp_set_configuration_cb setconf_cb;
	GSList *caps;
	gboolean reconfigure;
	gboolean start;
	GSList *cb;
	int ref;
};

struct a2dp_server {
	struct btd_adapter *adapter;
	GSList *sinks;
	GSList *sources;
	uint32_t source_record_id;
	uint32_t sink_record_id;
	gboolean sink_enabled;
	gboolean source_enabled;
};

static GSList *servers = NULL;
static GSList *setups = NULL;
static unsigned int cb_id = 0;

static struct a2dp_setup *setup_ref(struct a2dp_setup *setup)
{
	setup->ref++;

	DBG("%p: ref=%d", setup, setup->ref);

	return setup;
}

static struct audio_device *a2dp_get_dev(struct avdtp *session)
{
	return manager_get_audio_device(avdtp_get_device(session), FALSE);
}

static struct a2dp_setup *setup_new(struct avdtp *session)
{
	struct audio_device *dev;
	struct a2dp_setup *setup;

	dev = a2dp_get_dev(session);
	if (!dev) {
		error("Unable to create setup");
		return NULL;
	}

	setup = g_new0(struct a2dp_setup, 1);
	setup->session = avdtp_ref(session);
	setup->dev = a2dp_get_dev(session);
	setups = g_slist_append(setups, setup);

	return setup;
}

static void setup_free(struct a2dp_setup *s)
{
	DBG("%p", s);

	setups = g_slist_remove(setups, s);
	if (s->session)
		avdtp_unref(s->session);
	g_slist_free_full(s->cb, g_free);
	g_slist_free_full(s->caps, g_free);
	g_free(s);
}

static void setup_unref(struct a2dp_setup *setup)
{
	setup->ref--;

	DBG("%p: ref=%d", setup, setup->ref);

	if (setup->ref > 0)
		return;

	setup_free(setup);
}

static struct a2dp_setup_cb *setup_cb_new(struct a2dp_setup *setup)
{
	struct a2dp_setup_cb *cb;

	cb = g_new0(struct a2dp_setup_cb, 1);
	cb->setup = setup;
	cb->id = ++cb_id;

	setup->cb = g_slist_append(setup->cb, cb);
	return cb;
}

static void setup_cb_free(struct a2dp_setup_cb *cb)
{
	struct a2dp_setup *setup = cb->setup;

	if (cb->source_id)
		g_source_remove(cb->source_id);

	setup->cb = g_slist_remove(setup->cb, cb);
	setup_unref(cb->setup);
	g_free(cb);
}

static void finalize_setup_errno(struct a2dp_setup *s, int err,
					GSourceFunc cb1, ...)
{
	GSourceFunc finalize;
	va_list args;
	struct avdtp_error avdtp_err;

	if (err < 0) {
		avdtp_error_init(&avdtp_err, AVDTP_ERRNO, -err);
		s->err = &avdtp_err;
	}

	va_start(args, cb1);
	finalize = cb1;
	setup_ref(s);
	while (finalize != NULL) {
		finalize(s);
		finalize = va_arg(args, GSourceFunc);
	}
	setup_unref(s);
	va_end(args);
}

static gboolean finalize_config(gpointer data)
{
	struct a2dp_setup *s = data;
	GSList *l;
	struct avdtp_stream *stream = s->err ? NULL : s->stream;

	for (l = s->cb; l != NULL; ) {
		struct a2dp_setup_cb *cb = l->data;

		l = l->next;

		if (!cb->config_cb)
			continue;

		cb->config_cb(s->session, s->sep, stream, s->err,
							cb->user_data);
		setup_cb_free(cb);
	}

	return FALSE;
}

static gboolean finalize_resume(gpointer data)
{
	struct a2dp_setup *s = data;
	GSList *l;

	for (l = s->cb; l != NULL; ) {
		struct a2dp_setup_cb *cb = l->data;

		l = l->next;

		if (!cb->resume_cb)
			continue;

		cb->resume_cb(s->session, s->err, cb->user_data);
		setup_cb_free(cb);
	}

	return FALSE;
}

static gboolean finalize_suspend(gpointer data)
{
	struct a2dp_setup *s = data;
	GSList *l;

	for (l = s->cb; l != NULL; ) {
		struct a2dp_setup_cb *cb = l->data;

		l = l->next;

		if (!cb->suspend_cb)
			continue;

		cb->suspend_cb(s->session, s->err, cb->user_data);
		setup_cb_free(cb);
	}

	return FALSE;
}

static void finalize_select(struct a2dp_setup *s)
{
	GSList *l;

	for (l = s->cb; l != NULL; ) {
		struct a2dp_setup_cb *cb = l->data;

		l = l->next;

		if (!cb->select_cb)
			continue;

		cb->select_cb(s->session, s->sep, s->caps, cb->user_data);
		setup_cb_free(cb);
	}
}

static struct a2dp_setup *find_setup_by_session(struct avdtp *session)
{
	GSList *l;

	for (l = setups; l != NULL; l = l->next) {
		struct a2dp_setup *setup = l->data;

		if (setup->session == session)
			return setup;
	}

	return NULL;
}

static struct a2dp_setup *a2dp_setup_get(struct avdtp *session)
{
	struct a2dp_setup *setup;

	setup = find_setup_by_session(session);
	if (!setup) {
		setup = setup_new(session);
		if (!setup)
			return NULL;
	}

	return setup_ref(setup);
}

static struct a2dp_setup *find_setup_by_dev(struct audio_device *dev)
{
	GSList *l;

	for (l = setups; l != NULL; l = l->next) {
		struct a2dp_setup *setup = l->data;

		if (setup->dev == dev)
			return setup;
	}

	return NULL;
}

static struct a2dp_setup *find_setup_by_stream(struct avdtp_stream *stream)
{
	GSList *l;

	for (l = setups; l != NULL; l = l->next) {
		struct a2dp_setup *setup = l->data;

		if (setup->stream == stream)
			return setup;
	}

	return NULL;
}

static void stream_state_changed(struct avdtp_stream *stream,
					avdtp_state_t old_state,
					avdtp_state_t new_state,
					struct avdtp_error *err,
					void *user_data)
{
	struct a2dp_sep *sep = user_data;

	if (new_state == AVDTP_STATE_OPEN) {
		struct a2dp_setup *setup;
		int err;

		setup = find_setup_by_stream(stream);
		if (!setup || !setup->start)
			return;

		setup->start = FALSE;

		err = avdtp_start(setup->session, stream);
		if (err < 0 && err != -EINPROGRESS) {
			error("avdtp_start: %s (%d)", strerror(-err), -err);
			finalize_setup_errno(setup, err, finalize_resume,
									NULL);
			return;
		}

		sep->starting = TRUE;

		return;
	}

	if (new_state != AVDTP_STATE_IDLE)
		return;

	if (sep->suspend_timer) {
		g_source_remove(sep->suspend_timer);
		sep->suspend_timer = 0;
	}

	if (sep->session) {
		avdtp_unref(sep->session);
		sep->session = NULL;
	}

	sep->stream = NULL;

	if (sep->endpoint && sep->endpoint->clear_configuration)
		sep->endpoint->clear_configuration(sep, sep->user_data);
}

static gboolean auto_config(gpointer data)
{
	struct a2dp_setup *setup = data;

	/* Check if configuration was aborted */
	if (setup->sep->stream == NULL)
		return FALSE;

	if (setup->err != NULL)
		goto done;

	avdtp_stream_add_cb(setup->session, setup->stream,
				stream_state_changed, setup->sep);

	if (setup->sep->type == AVDTP_SEP_TYPE_SOURCE)
		sink_new_stream(setup->dev, setup->session, setup->stream);
	else
		source_new_stream(setup->dev, setup->session, setup->stream);

done:
	if (setup->setconf_cb)
		setup->setconf_cb(setup->session, setup->stream, setup->err);

	finalize_config(setup);

	if (setup->err) {
		g_free(setup->err);
		setup->err = NULL;
	}

	setup_unref(setup);

	return FALSE;
}

static void endpoint_setconf_cb(struct a2dp_setup *setup, gboolean ret)
{
	if (ret == FALSE) {
		setup->err = g_new(struct avdtp_error, 1);
		avdtp_error_init(setup->err, AVDTP_MEDIA_CODEC,
					AVDTP_UNSUPPORTED_CONFIGURATION);
	}

	auto_config(setup);
}

static gboolean endpoint_setconf_ind(struct avdtp *session,
						struct avdtp_local_sep *sep,
						struct avdtp_stream *stream,
						GSList *caps,
						avdtp_set_configuration_cb cb,
						void *user_data)
{
	struct a2dp_sep *a2dp_sep = user_data;
	struct a2dp_setup *setup;

	if (a2dp_sep->type == AVDTP_SEP_TYPE_SINK)
		DBG("Sink %p: Set_Configuration_Ind", sep);
	else
		DBG("Source %p: Set_Configuration_Ind", sep);

	setup = a2dp_setup_get(session);
	if (!session)
		return FALSE;

	a2dp_sep->stream = stream;
	setup->sep = a2dp_sep;
	setup->stream = stream;
	setup->setconf_cb = cb;

	for (; caps != NULL; caps = g_slist_next(caps)) {
		struct avdtp_service_capability *cap = caps->data;
		struct avdtp_media_codec_capability *codec;
		gboolean ret;

		if (cap->category == AVDTP_DELAY_REPORTING &&
					!a2dp_sep->delay_reporting) {
			setup->err = g_new(struct avdtp_error, 1);
			avdtp_error_init(setup->err, AVDTP_DELAY_REPORTING,
					AVDTP_UNSUPPORTED_CONFIGURATION);
			goto done;
		}

		if (cap->category != AVDTP_MEDIA_CODEC)
			continue;

		codec = (struct avdtp_media_codec_capability *) cap->data;

		if (codec->media_codec_type != a2dp_sep->codec) {
			setup->err = g_new(struct avdtp_error, 1);
			avdtp_error_init(setup->err, AVDTP_MEDIA_CODEC,
					AVDTP_UNSUPPORTED_CONFIGURATION);
			goto done;
		}

		ret = a2dp_sep->endpoint->set_configuration(a2dp_sep,
						setup->dev, codec->data,
						cap->length - sizeof(*codec),
						setup,
						endpoint_setconf_cb,
						a2dp_sep->user_data);
		if (ret == 0)
			return TRUE;

		setup->err = g_new(struct avdtp_error, 1);
		avdtp_error_init(setup->err, AVDTP_MEDIA_CODEC,
					AVDTP_UNSUPPORTED_CONFIGURATION);
		break;
	}

done:
	g_idle_add(auto_config, setup);
	return TRUE;
}

static gboolean endpoint_getcap_ind(struct avdtp *session,
					struct avdtp_local_sep *sep,
					gboolean get_all, GSList **caps,
					uint8_t *err, void *user_data)
{
	struct a2dp_sep *a2dp_sep = user_data;
	struct avdtp_service_capability *media_transport, *media_codec;
	struct avdtp_media_codec_capability *codec_caps;
	uint8_t *capabilities;
	size_t length;

	if (a2dp_sep->type == AVDTP_SEP_TYPE_SINK)
		DBG("Sink %p: Get_Capability_Ind", sep);
	else
		DBG("Source %p: Get_Capability_Ind", sep);

	*caps = NULL;

	media_transport = avdtp_service_cap_new(AVDTP_MEDIA_TRANSPORT,
						NULL, 0);

	*caps = g_slist_append(*caps, media_transport);

	length = a2dp_sep->endpoint->get_capabilities(a2dp_sep, &capabilities,
							a2dp_sep->user_data);

	codec_caps = g_malloc0(sizeof(*codec_caps) + length);
	codec_caps->media_type = AVDTP_MEDIA_TYPE_AUDIO;
	codec_caps->media_codec_type = a2dp_sep->codec;
	memcpy(codec_caps->data, capabilities, length);

	media_codec = avdtp_service_cap_new(AVDTP_MEDIA_CODEC, codec_caps,
						sizeof(*codec_caps) + length);

	*caps = g_slist_append(*caps, media_codec);
	g_free(codec_caps);

	if (get_all) {
		struct avdtp_service_capability *delay_reporting;
		delay_reporting = avdtp_service_cap_new(AVDTP_DELAY_REPORTING,
								NULL, 0);
		*caps = g_slist_append(*caps, delay_reporting);
	}

	return TRUE;
}

static void endpoint_open_cb(struct a2dp_setup *setup, gboolean ret)
{
	int err;

	if (ret == FALSE) {
		setup->stream = NULL;
		finalize_setup_errno(setup, -EPERM, finalize_config, NULL);
		return;
	}

	err = avdtp_open(setup->session, setup->stream);
	if (err == 0)
		return;

	error("Error on avdtp_open %s (%d)", strerror(-err), -err);
	setup->stream = NULL;
	finalize_setup_errno(setup, err, finalize_config, NULL);
}

static void setconf_cfm(struct avdtp *session, struct avdtp_local_sep *sep,
				struct avdtp_stream *stream,
				struct avdtp_error *err, void *user_data)
{
	struct a2dp_sep *a2dp_sep = user_data;
	struct a2dp_setup *setup;
	struct audio_device *dev;
	int ret;

	if (a2dp_sep->type == AVDTP_SEP_TYPE_SINK)
		DBG("Sink %p: Set_Configuration_Cfm", sep);
	else
		DBG("Source %p: Set_Configuration_Cfm", sep);

	setup = find_setup_by_session(session);

	if (err) {
		if (setup) {
			setup_ref(setup);
			setup->err = err;
			finalize_config(setup);
			setup->err = NULL;
			setup_unref(setup);
		}
		return;
	}

	avdtp_stream_add_cb(session, stream, stream_state_changed, a2dp_sep);
	a2dp_sep->stream = stream;

	if (!setup)
		return;

	dev = a2dp_get_dev(session);

	/* Notify D-Bus interface of the new stream */
	if (a2dp_sep->type == AVDTP_SEP_TYPE_SOURCE)
		sink_new_stream(dev, session, setup->stream);
	else
		source_new_stream(dev, session, setup->stream);

	/* Notify Endpoint */
	if (a2dp_sep->endpoint) {
		struct avdtp_service_capability *service;
		struct avdtp_media_codec_capability *codec;
		int err;

		service = avdtp_stream_get_codec(stream);
		codec = (struct avdtp_media_codec_capability *) service->data;

		err = a2dp_sep->endpoint->set_configuration(a2dp_sep, dev,
						codec->data, service->length -
						sizeof(*codec),
						setup,
						endpoint_open_cb,
						a2dp_sep->user_data);
		if (err == 0)
			return;

		setup->stream = NULL;
		finalize_setup_errno(setup, -EPERM, finalize_config, NULL);
		return;
	}

	ret = avdtp_open(session, stream);
	if (ret < 0) {
		error("Error on avdtp_open %s (%d)", strerror(-ret), -ret);
		setup->stream = NULL;
		finalize_setup_errno(setup, ret, finalize_config, NULL);
	}
}

static gboolean getconf_ind(struct avdtp *session, struct avdtp_local_sep *sep,
				uint8_t *err, void *user_data)
{
	struct a2dp_sep *a2dp_sep = user_data;

	if (a2dp_sep->type == AVDTP_SEP_TYPE_SINK)
		DBG("Sink %p: Get_Configuration_Ind", sep);
	else
		DBG("Source %p: Get_Configuration_Ind", sep);
	return TRUE;
}

static void getconf_cfm(struct avdtp *session, struct avdtp_local_sep *sep,
			struct avdtp_stream *stream, struct avdtp_error *err,
			void *user_data)
{
	struct a2dp_sep *a2dp_sep = user_data;

	if (a2dp_sep->type == AVDTP_SEP_TYPE_SINK)
		DBG("Sink %p: Set_Configuration_Cfm", sep);
	else
		DBG("Source %p: Set_Configuration_Cfm", sep);
}

static gboolean open_ind(struct avdtp *session, struct avdtp_local_sep *sep,
				struct avdtp_stream *stream, uint8_t *err,
				void *user_data)
{
	struct a2dp_sep *a2dp_sep = user_data;
	struct a2dp_setup *setup;

	if (a2dp_sep->type == AVDTP_SEP_TYPE_SINK)
		DBG("Sink %p: Open_Ind", sep);
	else
		DBG("Source %p: Open_Ind", sep);

	setup = find_setup_by_session(session);
	if (!setup)
		return TRUE;

	if (setup->reconfigure)
		setup->reconfigure = FALSE;

	finalize_config(setup);

	return TRUE;
}

static void open_cfm(struct avdtp *session, struct avdtp_local_sep *sep,
			struct avdtp_stream *stream, struct avdtp_error *err,
			void *user_data)
{
	struct a2dp_sep *a2dp_sep = user_data;
	struct a2dp_setup *setup;

	if (a2dp_sep->type == AVDTP_SEP_TYPE_SINK)
		DBG("Sink %p: Open_Cfm", sep);
	else
		DBG("Source %p: Open_Cfm", sep);

	setup = find_setup_by_session(session);
	if (!setup)
		return;

	if (setup->reconfigure)
		setup->reconfigure = FALSE;

	if (err) {
		setup->stream = NULL;
		setup->err = err;
	}

	finalize_config(setup);

	if (!setup->start || !err)
		return;

	setup->start = FALSE;
	finalize_resume(setup);

	return;
}

static gboolean suspend_timeout(struct a2dp_sep *sep)
{
	if (avdtp_suspend(sep->session, sep->stream) == 0)
		sep->suspending = TRUE;

	sep->suspend_timer = 0;

	avdtp_unref(sep->session);
	sep->session = NULL;

	return FALSE;
}

static gboolean start_ind(struct avdtp *session, struct avdtp_local_sep *sep,
				struct avdtp_stream *stream, uint8_t *err,
				void *user_data)
{
	struct a2dp_sep *a2dp_sep = user_data;
	struct a2dp_setup *setup;

	if (a2dp_sep->type == AVDTP_SEP_TYPE_SINK)
		DBG("Sink %p: Start_Ind", sep);
	else
		DBG("Source %p: Start_Ind", sep);

	if (!a2dp_sep->locked) {
		a2dp_sep->session = avdtp_ref(session);
		a2dp_sep->suspend_timer = g_timeout_add_seconds(SUSPEND_TIMEOUT,
						(GSourceFunc) suspend_timeout,
						a2dp_sep);
	}

	if (!a2dp_sep->starting)
		return TRUE;

	a2dp_sep->starting = FALSE;

	setup = find_setup_by_session(session);
	if (setup)
		finalize_resume(setup);

	return TRUE;
}

static void start_cfm(struct avdtp *session, struct avdtp_local_sep *sep,
			struct avdtp_stream *stream, struct avdtp_error *err,
			void *user_data)
{
	struct a2dp_sep *a2dp_sep = user_data;
	struct a2dp_setup *setup;

	if (a2dp_sep->type == AVDTP_SEP_TYPE_SINK)
		DBG("Sink %p: Start_Cfm", sep);
	else
		DBG("Source %p: Start_Cfm", sep);

	a2dp_sep->starting = FALSE;

	setup = find_setup_by_session(session);
	if (!setup)
		return;

	if (err) {
		setup->stream = NULL;
		setup->err = err;
	}

	finalize_resume(setup);
}

static gboolean suspend_ind(struct avdtp *session, struct avdtp_local_sep *sep,
				struct avdtp_stream *stream, uint8_t *err,
				void *user_data)
{
	struct a2dp_sep *a2dp_sep = user_data;
	struct a2dp_setup *setup;
	gboolean start;
	int start_err;

	if (a2dp_sep->type == AVDTP_SEP_TYPE_SINK)
		DBG("Sink %p: Suspend_Ind", sep);
	else
		DBG("Source %p: Suspend_Ind", sep);

	if (a2dp_sep->suspend_timer) {
		g_source_remove(a2dp_sep->suspend_timer);
		a2dp_sep->suspend_timer = 0;
		avdtp_unref(a2dp_sep->session);
		a2dp_sep->session = NULL;
	}

	if (!a2dp_sep->suspending)
		return TRUE;

	a2dp_sep->suspending = FALSE;

	setup = find_setup_by_session(session);
	if (!setup)
		return TRUE;

	start = setup->start;
	setup->start = FALSE;

	finalize_suspend(setup);

	if (!start)
		return TRUE;

	start_err = avdtp_start(session, a2dp_sep->stream);
	if (start_err < 0 && start_err != -EINPROGRESS) {
		error("avdtp_start: %s (%d)", strerror(-start_err),
								-start_err);
		finalize_setup_errno(setup, start_err, finalize_resume, NULL);
	}

	return TRUE;
}

static void suspend_cfm(struct avdtp *session, struct avdtp_local_sep *sep,
			struct avdtp_stream *stream, struct avdtp_error *err,
			void *user_data)
{
	struct a2dp_sep *a2dp_sep = user_data;
	struct a2dp_setup *setup;
	gboolean start;
	int start_err;

	if (a2dp_sep->type == AVDTP_SEP_TYPE_SINK)
		DBG("Sink %p: Suspend_Cfm", sep);
	else
		DBG("Source %p: Suspend_Cfm", sep);

	a2dp_sep->suspending = FALSE;

	setup = find_setup_by_session(session);
	if (!setup)
		return;

	start = setup->start;
	setup->start = FALSE;

	if (err) {
		setup->stream = NULL;
		setup->err = err;
	}

	finalize_suspend(setup);

	if (!start)
		return;

	if (err) {
		finalize_resume(setup);
		return;
	}

	start_err = avdtp_start(session, a2dp_sep->stream);
	if (start_err < 0 && start_err != -EINPROGRESS) {
		error("avdtp_start: %s (%d)", strerror(-start_err),
								-start_err);
		finalize_setup_errno(setup, start_err, finalize_suspend, NULL);
	}
}

static gboolean close_ind(struct avdtp *session, struct avdtp_local_sep *sep,
				struct avdtp_stream *stream, uint8_t *err,
				void *user_data)
{
	struct a2dp_sep *a2dp_sep = user_data;
	struct a2dp_setup *setup;

	if (a2dp_sep->type == AVDTP_SEP_TYPE_SINK)
		DBG("Sink %p: Close_Ind", sep);
	else
		DBG("Source %p: Close_Ind", sep);

	setup = find_setup_by_session(session);
	if (!setup)
		return TRUE;

	finalize_setup_errno(setup, -ECONNRESET, finalize_suspend,
							finalize_resume, NULL);

	return TRUE;
}

static gboolean a2dp_reconfigure(gpointer data)
{
	struct a2dp_setup *setup = data;
	struct a2dp_sep *sep = setup->sep;
	int posix_err;
	struct avdtp_media_codec_capability *rsep_codec;
	struct avdtp_service_capability *cap;

	if (setup->rsep) {
		cap = avdtp_get_codec(setup->rsep);
		rsep_codec = (struct avdtp_media_codec_capability *) cap->data;
	}

	if (!setup->rsep || sep->codec != rsep_codec->media_codec_type)
		setup->rsep = avdtp_find_remote_sep(setup->session, sep->lsep);

	posix_err = avdtp_set_configuration(setup->session, setup->rsep,
						sep->lsep,
						setup->caps,
						&setup->stream);
	if (posix_err < 0) {
		error("avdtp_set_configuration: %s", strerror(-posix_err));
		goto failed;
	}

	return FALSE;

failed:
	finalize_setup_errno(setup, posix_err, finalize_config, NULL);
	return FALSE;
}

static void close_cfm(struct avdtp *session, struct avdtp_local_sep *sep,
			struct avdtp_stream *stream, struct avdtp_error *err,
			void *user_data)
{
	struct a2dp_sep *a2dp_sep = user_data;
	struct a2dp_setup *setup;

	if (a2dp_sep->type == AVDTP_SEP_TYPE_SINK)
		DBG("Sink %p: Close_Cfm", sep);
	else
		DBG("Source %p: Close_Cfm", sep);

	setup = find_setup_by_session(session);
	if (!setup)
		return;

	if (err) {
		setup->stream = NULL;
		setup->err = err;
		finalize_config(setup);
		return;
	}

	if (!setup->rsep)
		setup->rsep = avdtp_stream_get_remote_sep(stream);

	if (setup->reconfigure)
		g_timeout_add(RECONFIGURE_TIMEOUT, a2dp_reconfigure, setup);
}

static void abort_ind(struct avdtp *session, struct avdtp_local_sep *sep,
				struct avdtp_stream *stream, uint8_t *err,
				void *user_data)
{
	struct a2dp_sep *a2dp_sep = user_data;
	struct a2dp_setup *setup;

	if (a2dp_sep->type == AVDTP_SEP_TYPE_SINK)
		DBG("Sink %p: Abort_Ind", sep);
	else
		DBG("Source %p: Abort_Ind", sep);

	a2dp_sep->stream = NULL;

	setup = find_setup_by_session(session);
	if (!setup)
		return;

	finalize_setup_errno(setup, -ECONNRESET, finalize_suspend,
							finalize_resume,
							finalize_config, NULL);

	return;
}

static void abort_cfm(struct avdtp *session, struct avdtp_local_sep *sep,
			struct avdtp_stream *stream, struct avdtp_error *err,
			void *user_data)
{
	struct a2dp_sep *a2dp_sep = user_data;
	struct a2dp_setup *setup;

	if (a2dp_sep->type == AVDTP_SEP_TYPE_SINK)
		DBG("Sink %p: Abort_Cfm", sep);
	else
		DBG("Source %p: Abort_Cfm", sep);

	setup = find_setup_by_session(session);
	if (!setup)
		return;

	setup_unref(setup);
}

static gboolean reconf_ind(struct avdtp *session, struct avdtp_local_sep *sep,
				uint8_t *err, void *user_data)
{
	struct a2dp_sep *a2dp_sep = user_data;

	if (a2dp_sep->type == AVDTP_SEP_TYPE_SINK)
		DBG("Sink %p: ReConfigure_Ind", sep);
	else
		DBG("Source %p: ReConfigure_Ind", sep);

	return TRUE;
}

static gboolean endpoint_delayreport_ind(struct avdtp *session,
						struct avdtp_local_sep *sep,
						uint8_t rseid, uint16_t delay,
						uint8_t *err, void *user_data)
{
	struct a2dp_sep *a2dp_sep = user_data;

	if (a2dp_sep->type == AVDTP_SEP_TYPE_SINK)
		DBG("Sink %p: DelayReport_Ind", sep);
	else
		DBG("Source %p: DelayReport_Ind", sep);

	if (a2dp_sep->endpoint == NULL ||
				a2dp_sep->endpoint->set_delay == NULL)
		return FALSE;

	a2dp_sep->endpoint->set_delay(a2dp_sep, delay, a2dp_sep->user_data);

	return TRUE;
}

static void reconf_cfm(struct avdtp *session, struct avdtp_local_sep *sep,
			struct avdtp_stream *stream, struct avdtp_error *err,
			void *user_data)
{
	struct a2dp_sep *a2dp_sep = user_data;
	struct a2dp_setup *setup;

	if (a2dp_sep->type == AVDTP_SEP_TYPE_SINK)
		DBG("Sink %p: ReConfigure_Cfm", sep);
	else
		DBG("Source %p: ReConfigure_Cfm", sep);

	setup = find_setup_by_session(session);
	if (!setup)
		return;

	if (err) {
		setup->stream = NULL;
		setup->err = err;
	}

	finalize_config(setup);
}

static void delay_report_cfm(struct avdtp *session, struct avdtp_local_sep *sep,
				struct avdtp_stream *stream,
				struct avdtp_error *err, void *user_data)
{
	struct a2dp_sep *a2dp_sep = user_data;

	if (a2dp_sep->type == AVDTP_SEP_TYPE_SINK)
		DBG("Sink %p: DelayReport_Cfm", sep);
	else
		DBG("Source %p: DelayReport_Cfm", sep);
}

static struct avdtp_sep_cfm cfm = {
	.set_configuration	= setconf_cfm,
	.get_configuration	= getconf_cfm,
	.open			= open_cfm,
	.start			= start_cfm,
	.suspend		= suspend_cfm,
	.close			= close_cfm,
	.abort			= abort_cfm,
	.reconfigure		= reconf_cfm,
	.delay_report		= delay_report_cfm,
};

static struct avdtp_sep_ind endpoint_ind = {
	.get_capability		= endpoint_getcap_ind,
	.set_configuration	= endpoint_setconf_ind,
	.get_configuration	= getconf_ind,
	.open			= open_ind,
	.start			= start_ind,
	.suspend		= suspend_ind,
	.close			= close_ind,
	.abort			= abort_ind,
	.reconfigure		= reconf_ind,
	.delayreport		= endpoint_delayreport_ind,
};

static sdp_record_t *a2dp_record(uint8_t type)
{
	sdp_list_t *svclass_id, *pfseq, *apseq, *root;
	uuid_t root_uuid, l2cap_uuid, avdtp_uuid, a2dp_uuid;
	sdp_profile_desc_t profile[1];
	sdp_list_t *aproto, *proto[2];
	sdp_record_t *record;
	sdp_data_t *psm, *version, *features;
	uint16_t lp = AVDTP_UUID;
	uint16_t a2dp_ver = 0x0103, avdtp_ver = 0x0103, feat = 0x000f;

	record = sdp_record_alloc();
	if (!record)
		return NULL;

	sdp_uuid16_create(&root_uuid, PUBLIC_BROWSE_GROUP);
	root = sdp_list_append(0, &root_uuid);
	sdp_set_browse_groups(record, root);

	if (type == AVDTP_SEP_TYPE_SOURCE)
		sdp_uuid16_create(&a2dp_uuid, AUDIO_SOURCE_SVCLASS_ID);
	else
		sdp_uuid16_create(&a2dp_uuid, AUDIO_SINK_SVCLASS_ID);
	svclass_id = sdp_list_append(0, &a2dp_uuid);
	sdp_set_service_classes(record, svclass_id);

	sdp_uuid16_create(&profile[0].uuid, ADVANCED_AUDIO_PROFILE_ID);
	profile[0].version = a2dp_ver;
	pfseq = sdp_list_append(0, &profile[0]);
	sdp_set_profile_descs(record, pfseq);

	sdp_uuid16_create(&l2cap_uuid, L2CAP_UUID);
	proto[0] = sdp_list_append(0, &l2cap_uuid);
	psm = sdp_data_alloc(SDP_UINT16, &lp);
	proto[0] = sdp_list_append(proto[0], psm);
	apseq = sdp_list_append(0, proto[0]);

	sdp_uuid16_create(&avdtp_uuid, AVDTP_UUID);
	proto[1] = sdp_list_append(0, &avdtp_uuid);
	version = sdp_data_alloc(SDP_UINT16, &avdtp_ver);
	proto[1] = sdp_list_append(proto[1], version);
	apseq = sdp_list_append(apseq, proto[1]);

	aproto = sdp_list_append(0, apseq);
	sdp_set_access_protos(record, aproto);

	features = sdp_data_alloc(SDP_UINT16, &feat);
	sdp_attr_add(record, SDP_ATTR_SUPPORTED_FEATURES, features);

	if (type == AVDTP_SEP_TYPE_SOURCE)
		sdp_set_info_attr(record, "Audio Source", 0, 0);
	else
		sdp_set_info_attr(record, "Audio Sink", 0, 0);

	free(psm);
	free(version);
	sdp_list_free(proto[0], 0);
	sdp_list_free(proto[1], 0);
	sdp_list_free(apseq, 0);
	sdp_list_free(pfseq, 0);
	sdp_list_free(aproto, 0);
	sdp_list_free(root, 0);
	sdp_list_free(svclass_id, 0);

	return record;
}

static struct a2dp_server *find_server(GSList *list, struct btd_adapter *a)
{

	for (; list; list = list->next) {
		struct a2dp_server *server = list->data;

		if (server->adapter == a)
			return server;
	}

	return NULL;
}

static struct a2dp_server *a2dp_server_register(struct btd_adapter *adapter,
							GKeyFile *config)
{
	struct a2dp_server *server;
	int av_err;

	server = g_new0(struct a2dp_server, 1);

	av_err = avdtp_init(adapter, config);
	if (av_err < 0) {
		DBG("AVDTP not registered");
		g_free(server);
		return NULL;
	}

	server->adapter = btd_adapter_ref(adapter);
	servers = g_slist_append(servers, server);

	return server;
}

int a2dp_source_register(struct btd_adapter *adapter, GKeyFile *config)
{
	struct a2dp_server *server;

	server = find_server(servers, adapter);
	if (server != NULL)
		goto done;

	server = a2dp_server_register(adapter, config);
	if (server == NULL)
		return -EPROTONOSUPPORT;

done:
	server->source_enabled = TRUE;

	return 0;
}

int a2dp_sink_register(struct btd_adapter *adapter, GKeyFile *config)
{
	struct a2dp_server *server;

	server = find_server(servers, adapter);
	if (server != NULL)
		goto done;

	server = a2dp_server_register(adapter, config);
	if (server == NULL)
		return -EPROTONOSUPPORT;

done:
	server->sink_enabled = TRUE;

	return 0;
}

static void a2dp_unregister_sep(struct a2dp_sep *sep)
{
	if (sep->destroy) {
		sep->destroy(sep->user_data);
		sep->endpoint = NULL;
	}

	avdtp_unregister_sep(sep->lsep);
	g_free(sep);
}

static void a2dp_server_unregister(struct a2dp_server *server)
{
	avdtp_exit(server->adapter);

	servers = g_slist_remove(servers, server);
	btd_adapter_unref(server->adapter);
	g_free(server);
}

void a2dp_sink_unregister(struct btd_adapter *adapter)
{
	struct a2dp_server *server;

	server = find_server(servers, adapter);
	if (!server)
		return;

	g_slist_free_full(server->sinks, (GDestroyNotify) a2dp_unregister_sep);

	if (server->sink_record_id) {
		remove_record_from_server(server->sink_record_id);
		server->sink_record_id = 0;
	}

	if (server->source_record_id)
		return;

	a2dp_server_unregister(server);
}

void a2dp_source_unregister(struct btd_adapter *adapter)
{
	struct a2dp_server *server;

	server = find_server(servers, adapter);
	if (!server)
		return;

	g_slist_free_full(server->sources,
					(GDestroyNotify) a2dp_unregister_sep);

	if (server->source_record_id) {
		remove_record_from_server(server->source_record_id);
		server->source_record_id = 0;
	}

	if (server->sink_record_id)
		return;

	a2dp_server_unregister(server);
}

struct a2dp_sep *a2dp_add_sep(struct btd_adapter *adapter, uint8_t type,
				uint8_t codec, gboolean delay_reporting,
				struct a2dp_endpoint *endpoint,
				void *user_data, GDestroyNotify destroy,
				int *err)
{
	struct a2dp_server *server;
	struct a2dp_sep *sep;
	GSList **l;
	uint32_t *record_id;
	sdp_record_t *record;

	server = find_server(servers, adapter);
	if (server == NULL) {
		if (err)
			*err = -EPROTONOSUPPORT;
		return NULL;
	}

	if (type == AVDTP_SEP_TYPE_SINK && !server->sink_enabled) {
		if (err)
			*err = -EPROTONOSUPPORT;
		return NULL;
	}

	if (type == AVDTP_SEP_TYPE_SOURCE && !server->source_enabled) {
		if (err)
			*err = -EPROTONOSUPPORT;
		return NULL;
	}

	sep = g_new0(struct a2dp_sep, 1);

	sep->lsep = avdtp_register_sep(adapter, type,
					AVDTP_MEDIA_TYPE_AUDIO, codec,
					delay_reporting, &endpoint_ind,
					&cfm, sep);

	if (sep->lsep == NULL) {
		g_free(sep);
		if (err)
			*err = -EINVAL;
		return NULL;
	}

	sep->server = server;
	sep->endpoint = endpoint;
	sep->codec = codec;
	sep->type = type;
	sep->delay_reporting = delay_reporting;
	sep->user_data = user_data;
	sep->destroy = destroy;

	if (type == AVDTP_SEP_TYPE_SOURCE) {
		l = &server->sources;
		record_id = &server->source_record_id;
	} else {
		l = &server->sinks;
		record_id = &server->sink_record_id;
	}

	if (*record_id != 0)
		goto add;

	record = a2dp_record(type);
	if (!record) {
		error("Unable to allocate new service record");
		avdtp_unregister_sep(sep->lsep);
		g_free(sep);
		if (err)
			*err = -EINVAL;
		return NULL;
	}

	if (add_record_to_server(adapter_get_address(server->adapter),
								record) < 0) {
		error("Unable to register A2DP service record");
		sdp_record_free(record);
		avdtp_unregister_sep(sep->lsep);
		g_free(sep);
		if (err)
			*err = -EINVAL;
		return NULL;
	}
	*record_id = record->handle;

add:
	*l = g_slist_append(*l, sep);

	if (err)
		*err = 0;
	return sep;
}

void a2dp_remove_sep(struct a2dp_sep *sep)
{
	struct a2dp_server *server = sep->server;

	if (sep->type == AVDTP_SEP_TYPE_SOURCE) {
		if (g_slist_find(server->sources, sep) == NULL)
			return;
		server->sources = g_slist_remove(server->sources, sep);
		if (server->sources == NULL && server->source_record_id) {
			remove_record_from_server(server->source_record_id);
			server->source_record_id = 0;
		}
	} else {
		if (g_slist_find(server->sinks, sep) == NULL)
			return;
		server->sinks = g_slist_remove(server->sinks, sep);
		if (server->sinks == NULL && server->sink_record_id) {
			remove_record_from_server(server->sink_record_id);
			server->sink_record_id = 0;
		}
	}

	if (sep->locked)
		return;

	a2dp_unregister_sep(sep);
}

static void select_cb(struct a2dp_setup *setup, void *ret, int size)
{
	struct avdtp_service_capability *media_transport, *media_codec;
	struct avdtp_media_codec_capability *cap;

	if (size < 0) {
		DBG("Endpoint replied an invalid configuration");
		goto done;
	}

	media_transport = avdtp_service_cap_new(AVDTP_MEDIA_TRANSPORT,
						NULL, 0);

	setup->caps = g_slist_append(setup->caps, media_transport);

	cap = g_malloc0(sizeof(*cap) + size);
	cap->media_type = AVDTP_MEDIA_TYPE_AUDIO;
	cap->media_codec_type = setup->sep->codec;
	memcpy(cap->data, ret, size);

	media_codec = avdtp_service_cap_new(AVDTP_MEDIA_CODEC, cap,
						sizeof(*cap) + size);

	setup->caps = g_slist_append(setup->caps, media_codec);
	g_free(cap);

done:
	finalize_select(setup);
}

static gboolean check_vendor_codec(struct a2dp_sep *sep, uint8_t *cap,
								size_t len)
{
	uint8_t *capabilities;
	size_t length;
	a2dp_vendor_codec_t *local_codec;
	a2dp_vendor_codec_t *remote_codec;

	if (len < sizeof(a2dp_vendor_codec_t))
		return FALSE;

	remote_codec = (a2dp_vendor_codec_t *) cap;

	if (sep->endpoint == NULL)
		return FALSE;

	length = sep->endpoint->get_capabilities(sep,
				&capabilities, sep->user_data);

	if (length < sizeof(a2dp_vendor_codec_t))
		return FALSE;

	local_codec = (a2dp_vendor_codec_t *) capabilities;

	if (memcmp(remote_codec->vendor_id, local_codec->vendor_id,
					sizeof(local_codec->vendor_id)))
		return FALSE;

	if (memcmp(remote_codec->codec_id, local_codec->codec_id,
					sizeof(local_codec->codec_id)))
		return FALSE;

	DBG("vendor 0x%02x%02x%02x%02x codec 0x%02x%02x",
			remote_codec->vendor_id[0], remote_codec->vendor_id[1],
			remote_codec->vendor_id[2], remote_codec->vendor_id[3],
			remote_codec->codec_id[0], remote_codec->codec_id[1]);

	return TRUE;
}

static struct a2dp_sep *a2dp_find_sep(struct avdtp *session, GSList *list,
					const char *sender)
{
	for (; list; list = list->next) {
		struct a2dp_sep *sep = list->data;
		struct avdtp_remote_sep *rsep;
		struct avdtp_media_codec_capability *cap;
		struct avdtp_service_capability *service;

		/* Use sender's endpoint if available */
		if (sender) {
			const char *name;

			if (sep->endpoint == NULL)
				continue;

			name = sep->endpoint->get_name(sep, sep->user_data);
			if (g_strcmp0(sender, name) != 0)
				continue;
		}

		rsep = avdtp_find_remote_sep(session, sep->lsep);
		if (rsep == NULL)
			continue;

		service = avdtp_get_codec(rsep);
		cap = (struct avdtp_media_codec_capability *) service->data;

		if (cap->media_codec_type != A2DP_CODEC_VENDOR)
			return sep;

		if (check_vendor_codec(sep, cap->data,
					service->length - sizeof(*cap)))
			return sep;
	}

	return NULL;
}

static struct a2dp_sep *a2dp_select_sep(struct avdtp *session, uint8_t type,
					const char *sender)
{
	struct a2dp_server *server;
	struct a2dp_sep *sep;
	GSList *l;

	server = find_server(servers, avdtp_get_adapter(session));
	if (!server)
		return NULL;

	l = type == AVDTP_SEP_TYPE_SINK ? server->sources : server->sinks;

	/* Check sender's seps first */
	sep = a2dp_find_sep(session, l, sender);
	if (sep != NULL)
		return sep;

	return a2dp_find_sep(session, l, NULL);
}

unsigned int a2dp_select_capabilities(struct avdtp *session,
					uint8_t type, const char *sender,
					a2dp_select_cb_t cb,
					void *user_data)
{
	struct a2dp_setup *setup;
	struct a2dp_setup_cb *cb_data;
	struct a2dp_sep *sep;
	struct avdtp_service_capability *service;
	struct avdtp_media_codec_capability *codec;
	int err;

	sep = a2dp_select_sep(session, type, sender);
	if (!sep) {
		error("Unable to select SEP");
		return 0;
	}

	setup = a2dp_setup_get(session);
	if (!setup)
		return 0;

	cb_data = setup_cb_new(setup);
	cb_data->select_cb = cb;
	cb_data->user_data = user_data;

	setup->sep = sep;
	setup->rsep = avdtp_find_remote_sep(session, sep->lsep);

	if (setup->rsep == NULL) {
		error("Could not find remote sep");
		goto fail;
	}

	service = avdtp_get_codec(setup->rsep);
	codec = (struct avdtp_media_codec_capability *) service->data;

	err = sep->endpoint->select_configuration(sep, codec->data,
					service->length - sizeof(*codec),
					setup,
					select_cb, sep->user_data);
	if (err == 0)
		return cb_data->id;

fail:
	setup_cb_free(cb_data);
	return 0;

}

unsigned int a2dp_config(struct avdtp *session, struct a2dp_sep *sep,
				a2dp_config_cb_t cb, GSList *caps,
				void *user_data)
{
	struct a2dp_setup_cb *cb_data;
	GSList *l;
	struct a2dp_server *server;
	struct a2dp_setup *setup;
	struct a2dp_sep *tmp;
	struct avdtp_service_capability *cap;
	struct avdtp_media_codec_capability *codec_cap = NULL;
	int posix_err;

	server = find_server(servers, avdtp_get_adapter(session));
	if (!server)
		return 0;

	for (l = caps; l != NULL; l = l->next) {
		cap = l->data;

		if (cap->category != AVDTP_MEDIA_CODEC)
			continue;

		codec_cap = (void *) cap->data;
		break;
	}

	if (!codec_cap)
		return 0;

	if (sep->codec != codec_cap->media_codec_type)
		return 0;

	DBG("a2dp_config: selected SEP %p", sep->lsep);

	setup = a2dp_setup_get(session);
	if (!setup)
		return 0;

	cb_data = setup_cb_new(setup);
	cb_data->config_cb = cb;
	cb_data->user_data = user_data;

	setup->sep = sep;
	setup->stream = sep->stream;

	/* Copy given caps if they are different than current caps */
	if (setup->caps != caps) {
		g_slist_free_full(setup->caps, g_free);
		setup->caps = g_slist_copy(caps);
	}

	switch (avdtp_sep_get_state(sep->lsep)) {
	case AVDTP_STATE_IDLE:
		if (sep->type == AVDTP_SEP_TYPE_SOURCE)
			l = server->sources;
		else
			l = server->sinks;

		for (; l != NULL; l = l->next) {
			tmp = l->data;
			if (avdtp_has_stream(session, tmp->stream))
				break;
		}

		if (l != NULL) {
			if (tmp->locked)
				goto failed;
			setup->reconfigure = TRUE;
			if (avdtp_close(session, tmp->stream, FALSE) < 0) {
				error("avdtp_close failed");
				goto failed;
			}
			break;
		}

		setup->rsep = avdtp_find_remote_sep(session, sep->lsep);
		if (setup->rsep == NULL) {
			error("No matching ACP and INT SEPs found");
			goto failed;
		}

		posix_err = avdtp_set_configuration(session, setup->rsep,
							sep->lsep, caps,
							&setup->stream);
		if (posix_err < 0) {
			error("avdtp_set_configuration: %s",
				strerror(-posix_err));
			goto failed;
		}
		break;
	case AVDTP_STATE_OPEN:
	case AVDTP_STATE_STREAMING:
		if (avdtp_stream_has_capabilities(setup->stream, caps)) {
			DBG("Configuration match: resuming");
			cb_data->source_id = g_idle_add(finalize_config,
								setup);
		} else if (!setup->reconfigure) {
			setup->reconfigure = TRUE;
			if (avdtp_close(session, sep->stream, FALSE) < 0) {
				error("avdtp_close failed");
				goto failed;
			}
		}
		break;
	default:
		error("SEP in bad state for requesting a new stream");
		goto failed;
	}

	return cb_data->id;

failed:
	setup_cb_free(cb_data);
	return 0;
}

unsigned int a2dp_resume(struct avdtp *session, struct a2dp_sep *sep,
				a2dp_stream_cb_t cb, void *user_data)
{
	struct a2dp_setup_cb *cb_data;
	struct a2dp_setup *setup;

	setup = a2dp_setup_get(session);
	if (!setup)
		return 0;

	cb_data = setup_cb_new(setup);
	cb_data->resume_cb = cb;
	cb_data->user_data = user_data;

	setup->sep = sep;
	setup->stream = sep->stream;

	switch (avdtp_sep_get_state(sep->lsep)) {
	case AVDTP_STATE_IDLE:
		goto failed;
		break;
	case AVDTP_STATE_CONFIGURED:
		setup->start = TRUE;
		break;
	case AVDTP_STATE_OPEN:
		if (avdtp_start(session, sep->stream) < 0) {
			error("avdtp_start failed");
			goto failed;
		}
		sep->starting = TRUE;
		break;
	case AVDTP_STATE_STREAMING:
		if (!sep->suspending && sep->suspend_timer) {
			g_source_remove(sep->suspend_timer);
			sep->suspend_timer = 0;
			avdtp_unref(sep->session);
			sep->session = NULL;
		}
		if (sep->suspending)
			setup->start = TRUE;
		else
			cb_data->source_id = g_idle_add(finalize_resume,
								setup);
		break;
	default:
		error("SEP in bad state for resume");
		goto failed;
	}

	return cb_data->id;

failed:
	setup_cb_free(cb_data);
	return 0;
}

unsigned int a2dp_suspend(struct avdtp *session, struct a2dp_sep *sep,
				a2dp_stream_cb_t cb, void *user_data)
{
	struct a2dp_setup_cb *cb_data;
	struct a2dp_setup *setup;

	setup = a2dp_setup_get(session);
	if (!setup)
		return 0;

	cb_data = setup_cb_new(setup);
	cb_data->suspend_cb = cb;
	cb_data->user_data = user_data;

	setup->sep = sep;
	setup->stream = sep->stream;

	switch (avdtp_sep_get_state(sep->lsep)) {
	case AVDTP_STATE_IDLE:
		error("a2dp_suspend: no stream to suspend");
		goto failed;
		break;
	case AVDTP_STATE_OPEN:
		cb_data->source_id = g_idle_add(finalize_suspend, setup);
		break;
	case AVDTP_STATE_STREAMING:
		if (avdtp_suspend(session, sep->stream) < 0) {
			error("avdtp_suspend failed");
			goto failed;
		}
		sep->suspending = TRUE;
		break;
	default:
		error("SEP in bad state for suspend");
		goto failed;
	}

	return cb_data->id;

failed:
	setup_cb_free(cb_data);
	return 0;
}

gboolean a2dp_cancel(struct audio_device *dev, unsigned int id)
{
	struct a2dp_setup *setup;
	GSList *l;

	setup = find_setup_by_dev(dev);
	if (!setup)
		return FALSE;

	for (l = setup->cb; l != NULL; l = g_slist_next(l)) {
		struct a2dp_setup_cb *cb = l->data;

		if (cb->id != id)
			continue;

		setup_ref(setup);
		setup_cb_free(cb);

		if (!setup->cb) {
			DBG("aborting setup %p", setup);
			avdtp_abort(setup->session, setup->stream);
			return TRUE;
		}

		setup_unref(setup);
		return TRUE;
	}

	return FALSE;
}

gboolean a2dp_sep_lock(struct a2dp_sep *sep, struct avdtp *session)
{
	if (sep->locked)
		return FALSE;

	DBG("SEP %p locked", sep->lsep);
	sep->locked = TRUE;

	return TRUE;
}

gboolean a2dp_sep_unlock(struct a2dp_sep *sep, struct avdtp *session)
{
	struct a2dp_server *server = sep->server;
	avdtp_state_t state;
	GSList *l;

	state = avdtp_sep_get_state(sep->lsep);

	sep->locked = FALSE;

	DBG("SEP %p unlocked", sep->lsep);

	if (sep->type == AVDTP_SEP_TYPE_SOURCE)
		l = server->sources;
	else
		l = server->sinks;

	/* Unregister sep if it was removed */
	if (g_slist_find(l, sep) == NULL) {
		a2dp_unregister_sep(sep);
		return TRUE;
	}

	if (!sep->stream || state == AVDTP_STATE_IDLE)
		return TRUE;

	switch (state) {
	case AVDTP_STATE_OPEN:
		/* Set timer here */
		break;
	case AVDTP_STATE_STREAMING:
		if (avdtp_suspend(session, sep->stream) == 0)
			sep->suspending = TRUE;
		break;
	default:
		break;
	}

	return TRUE;
}

struct avdtp_stream *a2dp_sep_get_stream(struct a2dp_sep *sep)
{
	return sep->stream;
}

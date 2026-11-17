/*
 * mod_amvd - Answering Machine / Voicemail Detection for FreeSWITCH
 *
 * Dialplan applications
 *   amvd       [profile] [key=value ...]   blocking: returns when the verdict (and, with
 *                                          wait-message-end, the end of the greeting) is known
 *   amvd_start [profile] [key=value ...]   background: detection runs on a media bug while the
 *                                          channel bridges / parks / plays; results arrive as events
 *   amvd_stop                              stop a background detection
 *
 * API
 *   uuid_amvd <uuid> start [profile] [key=value ...]
 *   uuid_amvd <uuid> stop
 *   amvd status | reload | profiles
 *
 * Channel variables read : amvd_profile, amvd_params ("k=v,k=v", applied after the profile)
 * Channel variables set  : amvd_result, amvd_reason, amvd_decision_ms, amvd_p_machine,
 *                          amvd_first_speech_ms, amvd_features, amvd_end_type, amvd_end_ms,
 *                          amvd_beep_hz, amvd_no_audio_ms
 *                          AMDSTATUS / AMDCAUSE (VICIdial / mod_amd compatible)
 * Hooks (background)     : api_on_amvd_result*, api_on_amvd_message_end* run as API commands
 *                          on a pool thread, never under the media bug lock
 * Events                 : CUSTOM amvd::result, amvd::message_end, amvd::beep
 *
 * Integration notes learned from other FreeSWITCH AMD modules:
 *  - exactly one switch_core_media_bug_read() per READ callback; on an empty buffer the core
 *    returns a synthetic frame rather than failing
 *  - frames with every sample == -1 (0xFFFF) are the core's fill for missing RTP; they are fed
 *    as digital silence so timing stays right, and counted in amvd_no_audio_ms
 *  - SMBF_ONE_ONLY refuses a second detector on the same channel
 *  - every detection holds a read lock on the module so unload cannot unmap live code
 *  - only mod_amvd_module_interface is exported (see mod_amvd.map)
 */
#include <switch.h>
#include "amvd_engine.h"

#define AMVD_EVENT_RESULT  "amvd::result"
#define AMVD_EVENT_MSGEND  "amvd::message_end"
#define AMVD_EVENT_BEEP    "amvd::beep"
#define AMVD_PRIVATE       "_amvd_call_"
#define AMVD_BUG_NAME      "amvd"

SWITCH_MODULE_LOAD_FUNCTION(mod_amvd_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_amvd_shutdown);
SWITCH_MODULE_DEFINITION(mod_amvd, mod_amvd_load, mod_amvd_shutdown, NULL);

/* ------------------------------------------------------------------ */
/* Profiles / models                                                   */
/* ------------------------------------------------------------------ */

typedef struct amvd_model_ref {
	amvd_model_t *model;
	char *path;
	int refs;
} amvd_model_ref_t;

typedef struct amvd_profile {
	char *name;
	amvd_config_t cfg;
	amvd_model_ref_t *mref;
	struct amvd_profile *next;
} amvd_profile_t;

static struct {
	switch_memory_pool_t *pool;
	switch_mutex_t *mutex;
	switch_loadable_module_interface_t *mi;
	amvd_profile_t *profiles;          /* malloc'd list, replaced on reload */
	char default_profile[64];
	int fire_beep_events;              /* amvd::beep per beep (off: one less event per VM call) */
	int event_channel_data;            /* full channel vars on events (off: slim headers, cheaper ESL) */
	/* counters */
	uint64_t started, human, machine, fax, unavailable, unknown, hangup, msg_end;
	int active;
} globals;

static void model_unref(amvd_model_ref_t *r)
{
	int free_it = 0;
	if (!r) return;
	switch_mutex_lock(globals.mutex);
	if (--r->refs <= 0) free_it = 1;
	switch_mutex_unlock(globals.mutex);
	if (free_it) {
		amvd_model_free(r->model);
		free(r->path);
		free(r);
	}
}

static void free_profiles(amvd_profile_t *p)
{
	while (p) {
		amvd_profile_t *n = p->next;
		model_unref(p->mref);
		free(p->name);
		free(p);
		p = n;
	}
}

static amvd_profile_t *find_profile(amvd_profile_t *list, const char *name)
{
	for (; list; list = list->next)
		if (!strcasecmp(list->name, name)) return list;
	return NULL;
}

static switch_status_t load_config(void)
{
	switch_xml_t cfg, xml, settings, profiles, x_profile, param;
	amvd_profile_t *list = NULL, *old;
	char def[64] = "default";
	int beep_ev = 0, chan_data = 1;

	if (!(xml = switch_xml_open_cfg("amvd.conf", &cfg, NULL))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "amvd.conf not found, using built-in defaults\n");
	} else {
		if ((settings = switch_xml_child(cfg, "settings"))) {
			for (param = switch_xml_child(settings, "param"); param; param = param->next) {
				const char *n = switch_xml_attr_soft(param, "name"), *v = switch_xml_attr_soft(param, "value");
				if (!strcasecmp(n, "default-profile") && !zstr(v)) switch_copy_string(def, v, sizeof(def));
				else if (!strcasecmp(n, "fire-beep-events")) beep_ev = switch_true(v);
				else if (!strcasecmp(n, "event-channel-data")) chan_data = switch_true(v);
			}
		}
		if ((profiles = switch_xml_child(cfg, "profiles"))) {
			for (x_profile = switch_xml_child(profiles, "profile"); x_profile; x_profile = x_profile->next) {
				const char *pname = switch_xml_attr_soft(x_profile, "name");
				const char *inherit = switch_xml_attr(x_profile, "extends");
				amvd_profile_t *p;
				if (zstr(pname)) continue;
				p = calloc(1, sizeof(*p));
				p->name = strdup(pname);
				amvd_config_defaults(&p->cfg);
				if (inherit) {
					amvd_profile_t *base = find_profile(list, inherit);
					if (base) {
						p->cfg = base->cfg;
						if (base->mref) {
							switch_mutex_lock(globals.mutex);
							base->mref->refs++;
							switch_mutex_unlock(globals.mutex);
							p->mref = base->mref;
						}
					} else {
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "amvd: profile %s extends unknown %s (define it first)\n", pname, inherit);
					}
				}
				for (param = switch_xml_child(x_profile, "param"); param; param = param->next) {
					const char *n = switch_xml_attr_soft(param, "name"), *v = switch_xml_attr_soft(param, "value");
					if (!strcasecmp(n, "model-file")) {
						model_unref(p->mref);
						p->mref = NULL;
						if (!zstr(v)) {
							char err[512];
							amvd_model_t *m = amvd_model_load(v, err, sizeof(err));
							if (m) {
								p->mref = calloc(1, sizeof(*p->mref));
								p->mref->model = m;
								p->mref->path = strdup(v);
								p->mref->refs = 1;
							} else {
								switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "amvd: profile %s: model not loaded: %s (rules only)\n", pname, err);
							}
						}
						continue;
					}
					switch (amvd_config_set(&p->cfg, n, v)) {
					case -1: switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "amvd: profile %s: unknown param %s\n", pname, n); break;
					case -2: switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "amvd: profile %s: bad value %s=%s\n", pname, n, v); break;
					default: break;
					}
				}
				p->next = list;
				list = p;
			}
		}
		switch_xml_free(xml);
	}
	if (!find_profile(list, "default")) {
		amvd_profile_t *p = calloc(1, sizeof(*p));
		p->name = strdup("default");
		amvd_config_defaults(&p->cfg);
		p->next = list;
		list = p;
	}

	switch_mutex_lock(globals.mutex);
	old = globals.profiles;
	globals.profiles = list;
	switch_copy_string(globals.default_profile, def, sizeof(globals.default_profile));
	globals.fire_beep_events = beep_ev;
	globals.event_channel_data = chan_data;
	switch_mutex_unlock(globals.mutex);
	free_profiles(old); /* models still used by live calls survive through their refs */
	return SWITCH_STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Per-call state                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
	switch_core_session_t *session;
	switch_channel_t *channel;
	switch_media_bug_t *bug;
	amvd_engine_t *engine;
	amvd_model_ref_t *mref;
	char profile[64];
	int background;
	int wait_end;
	int rate, channels;
	int result_applied, end_applied, finished;
	int stopped;
	volatile int done;           /* blocking app polls this */
	uint32_t no_audio_ms;
	switch_time_t t_start;
} amvd_call_t;

static const char *amd_status(amvd_result_t r)
{
	switch (r) {
	case AMVD_RES_HUMAN: return "HUMAN";
	case AMVD_RES_MACHINE: case AMVD_RES_FAX: return "MACHINE";
	default: return "NOTSURE";
	}
}

static const char *amd_cause(const amvd_report_t *r)
{
	if (r->result == AMVD_RES_FAX) return "FAX";
	if (r->result == AMVD_RES_UNAVAILABLE) return "SIT";
	if (!strcmp(r->reason, "long_greeting")) return "LONGGREETING";
	if (!strcmp(r->reason, "max_words")) return "MAXWORDS";
	if (!strcmp(r->reason, "initial_silence")) return "INITIALSILENCE";
	if (!strcmp(r->reason, "beep")) return "BEEP";
	if (!strcmp(r->reason, "timeout") || !strcmp(r->reason, "speech_at_timeout") || !strcmp(r->reason, "no_speech")) return "TOOLONG";
	if (r->result == AMVD_RES_HUMAN) return "HUMAN";
	return "MODEL";
}

/* Full channel data is ~100+ headers per event. At thousands of concurrent calls the slim form
 * keeps ESL traffic small: identity + the variables a controller routes on. */
static void set_channel_headers(amvd_call_t *call, switch_event_t *ev)
{
	static const char *vars[] = { "amvd_policy", "amvd_campaign", "amvd_lead_id", "origination_caller_id_number", NULL };
	int i;
	if (globals.event_channel_data) {
		switch_channel_event_set_data(call->channel, ev);
		return;
	}
	switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, "Unique-ID", switch_core_session_get_uuid(call->session));
	switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, "Channel-Name", switch_channel_get_name(call->channel));
	switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, "Caller-Destination-Number",
								   switch_str_nil(switch_channel_get_variable(call->channel, "destination_number")));
	for (i = 0; vars[i]; i++) {
		const char *v = switch_channel_get_variable(call->channel, vars[i]);
		if (v) {
			char name[96];
			switch_snprintf(name, sizeof(name), "variable_%s", vars[i]);
			switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, name, v);
		}
	}
}

static void add_common_headers(amvd_call_t *call, switch_event_t *ev, const amvd_report_t *r)
{
	char feats[1024];
	set_channel_headers(call, ev);
	switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, "AMVD-Version", AMVD_VERSION);
	switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, "AMVD-Profile", call->profile);
	switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, "AMVD-Background", call->background ? "true" : "false");
	switch_event_add_header(ev, SWITCH_STACK_BOTTOM, "AMVD-Elapsed-Wall-MS", "%lld", (long long)((switch_micro_time_now() - call->t_start) / 1000));
	switch_event_add_header(ev, SWITCH_STACK_BOTTOM, "AMVD-Audio-MS", "%d", amvd_engine_elapsed_ms(call->engine));
	switch_event_add_header(ev, SWITCH_STACK_BOTTOM, "AMVD-No-Audio-MS", "%u", call->no_audio_ms);
	switch_event_add_header(ev, SWITCH_STACK_BOTTOM, "AMVD-Model", "%s", call->mref ? call->mref->path : "rules");
	amvd_features_csv(r->features, feats, sizeof(feats));
	switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, "AMVD-Features", feats);
	switch_event_add_header(ev, SWITCH_STACK_BOTTOM, "AMVD-Features-MS", "%d", r->features_ms);
}

/* Run api_on_<prefix>* on a pool thread: commands may add/remove media bugs (uuid_record,
 * uuid_transfer, uuid_amvd) and must never run under the bug lock. */
typedef struct { switch_core_session_t *session; char prefix[64]; } api_job_t;

static void *SWITCH_THREAD_FUNC api_on_thread(switch_thread_t *thread, void *obj)
{
	api_job_t *job = obj;
	switch_channel_t *channel = switch_core_session_get_channel(job->session);
	if (switch_channel_up(channel)) switch_channel_api_on(channel, job->prefix);
	switch_core_session_rwunlock(job->session);
	free(job);
	return NULL;
}

static void run_api_on(amvd_call_t *call, const char *prefix)
{
	switch_thread_data_t *td;
	api_job_t *job;

	/* O(1) check: no cloning of the channel's variables per call. Set the hook as
	 * api_on_amvd_result (further commands: api_on_amvd_result_1, _2 ... alongside it). */
	if (!switch_channel_get_variable(call->channel, prefix)) return;
	if (switch_core_session_read_lock(call->session) != SWITCH_STATUS_SUCCESS) return;
	job = calloc(1, sizeof(*job));
	td = calloc(1, sizeof(*td));
	job->session = call->session;
	switch_copy_string(job->prefix, prefix, sizeof(job->prefix));
	td->func = api_on_thread;
	td->obj = job;
	td->alloc = 1;
	if (switch_thread_pool_launch_thread(&td) != SWITCH_STATUS_SUCCESS) {
		switch_core_session_rwunlock(call->session);
		free(job);
		free(td);
	}
}

static void apply_result(amvd_call_t *call, const char *override_result)
{
	const amvd_report_t *r = amvd_engine_report(call->engine);
	const char *res = override_result ? override_result : amvd_result_str(r->result);
	char feats[1024];
	switch_event_t *ev;

	if (call->result_applied) return;
	call->result_applied = 1;

	amvd_features_csv(r->features, feats, sizeof(feats));
	switch_channel_set_variable(call->channel, "amvd_result", res);
	switch_channel_set_variable(call->channel, "amvd_reason", override_result ? override_result : r->reason);
	switch_channel_set_variable_printf(call->channel, "amvd_decision_ms", "%d", r->decision_ms);
	switch_channel_set_variable_printf(call->channel, "amvd_p_machine", "%.4f", r->p_machine);
	switch_channel_set_variable_printf(call->channel, "amvd_first_speech_ms", "%d", r->first_speech_ms);
	switch_channel_set_variable(call->channel, "amvd_features", feats);
	switch_channel_set_variable_printf(call->channel, "amvd_no_audio_ms", "%u", call->no_audio_ms);
	if (override_result && !strcmp(override_result, "hangup")) {
		switch_channel_set_variable(call->channel, "AMDSTATUS", "HANGUP");
		switch_channel_set_variable(call->channel, "AMDCAUSE", "HANGUP");
	} else {
		switch_channel_set_variable(call->channel, "AMDSTATUS", amd_status(r->result));
		switch_channel_set_variable(call->channel, "AMDCAUSE", amd_cause(r));
	}

	switch_mutex_lock(globals.mutex);
	if (override_result) globals.hangup++;
	else switch (r->result) {
		case AMVD_RES_HUMAN: globals.human++; break;
		case AMVD_RES_MACHINE: globals.machine++; break;
		case AMVD_RES_FAX: globals.fax++; break;
		case AMVD_RES_UNAVAILABLE: globals.unavailable++; break;
		default: globals.unknown++; break;
	}
	switch_mutex_unlock(globals.mutex);

	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(call->session), SWITCH_LOG_INFO, "AMVD: %s result=%s reason=%s decision=%dms\n",
					  switch_channel_get_name(call->channel), res, override_result ? override_result : r->reason, r->decision_ms);
	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(call->session), SWITCH_LOG_DEBUG, "AMVD: p=%.3f first_speech=%dms profile=%s features=[%s]\n",
					  r->p_machine, r->first_speech_ms, call->profile, feats);

	if (switch_event_create_subclass(&ev, SWITCH_EVENT_CUSTOM, AMVD_EVENT_RESULT) == SWITCH_STATUS_SUCCESS) {
		add_common_headers(call, ev, r);
		switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, "AMVD-Result", res);
		switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, "AMVD-Reason", override_result ? override_result : r->reason);
		switch_event_add_header(ev, SWITCH_STACK_BOTTOM, "AMVD-Decision-MS", "%d", r->decision_ms);
		switch_event_add_header(ev, SWITCH_STACK_BOTTOM, "AMVD-P-Machine", "%.4f", r->p_machine);
		switch_event_add_header(ev, SWITCH_STACK_BOTTOM, "AMVD-First-Speech-MS", "%d", r->first_speech_ms);
		switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, "AMVD-Wait-Message-End", call->wait_end ? "true" : "false");
		switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, "AMD-Status", switch_channel_get_variable(call->channel, "AMDSTATUS"));
		switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, "AMD-Cause", switch_channel_get_variable(call->channel, "AMDCAUSE"));
		switch_event_fire(&ev);
	}
	if (call->background) run_api_on(call, "api_on_amvd_result");
}

static void apply_msg_end(amvd_call_t *call)
{
	const amvd_report_t *r = amvd_engine_report(call->engine);
	switch_event_t *ev;

	if (call->end_applied) return;
	call->end_applied = 1;
	switch_channel_set_variable(call->channel, "amvd_end_type", amvd_end_str(r->end_type));
	switch_channel_set_variable_printf(call->channel, "amvd_end_ms", "%d", r->end_ms);
	switch_channel_set_variable_printf(call->channel, "amvd_beep_hz", "%.0f", r->beep_hz);
	switch_mutex_lock(globals.mutex);
	globals.msg_end++;
	switch_mutex_unlock(globals.mutex);
	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(call->session), SWITCH_LOG_INFO, "AMVD: %s message_end=%s at %dms beep=%.0fHz\n",
					  switch_channel_get_name(call->channel), amvd_end_str(r->end_type), r->end_ms, r->beep_hz);
	if (switch_event_create_subclass(&ev, SWITCH_EVENT_CUSTOM, AMVD_EVENT_MSGEND) == SWITCH_STATUS_SUCCESS) {
		add_common_headers(call, ev, r);
		switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, "AMVD-End-Type", amvd_end_str(r->end_type));
		switch_event_add_header(ev, SWITCH_STACK_BOTTOM, "AMVD-End-MS", "%d", r->end_ms);
		switch_event_add_header(ev, SWITCH_STACK_BOTTOM, "AMVD-Beep-Hz", "%.0f", r->beep_hz);
		switch_event_add_header(ev, SWITCH_STACK_BOTTOM, "AMVD-Beep-Count", "%d", r->beep_count);
		switch_event_fire(&ev);
	}
	if (call->background) run_api_on(call, "api_on_amvd_message_end");
}

static void fire_beep(amvd_call_t *call)
{
	const amvd_report_t *r = amvd_engine_report(call->engine);
	switch_event_t *ev;
	if (!globals.fire_beep_events) return;
	if (switch_event_create_subclass(&ev, SWITCH_EVENT_CUSTOM, AMVD_EVENT_BEEP) == SWITCH_STATUS_SUCCESS) {
		set_channel_headers(call, ev);
		switch_event_add_header(ev, SWITCH_STACK_BOTTOM, "AMVD-Beep-Hz", "%.0f", r->beep_hz);
		switch_event_add_header(ev, SWITCH_STACK_BOTTOM, "AMVD-Audio-MS", "%d", amvd_engine_elapsed_ms(call->engine));
		switch_event_fire(&ev);
	}
}

/* returns 1 when the engine is done */
static int handle_events(amvd_call_t *call, int ev)
{
	if (ev & AMVD_EV_BEEP) fire_beep(call);
	if (ev & AMVD_EV_RESULT) apply_result(call, NULL);
	if (ev & AMVD_EV_MSG_END) apply_msg_end(call);
	if (ev & AMVD_EV_DONE) {
		call->finished = 1;
		return 1;
	}
	return 0;
}

static void call_release(amvd_call_t *call)
{
	switch_channel_set_private(call->channel, AMVD_PRIVATE, NULL);
	if (call->engine) {
		amvd_engine_destroy(call->engine);
		call->engine = NULL;
	}
	model_unref(call->mref);
	call->mref = NULL;
	switch_mutex_lock(globals.mutex);
	globals.active--;
	switch_mutex_unlock(globals.mutex);
	switch_thread_rwlock_unlock(globals.mi->rwlock);
	call->done = 1;
}

static switch_bool_t amvd_bug_callback(switch_media_bug_t *bug, void *user_data, switch_abc_type_t type)
{
	amvd_call_t *call = user_data;

	switch (type) {
	case SWITCH_ABC_TYPE_INIT:
		break;

	case SWITCH_ABC_TYPE_READ: {
		uint8_t data[SWITCH_RECOMMENDED_BUFFER_SIZE];
		int16_t mono[SWITCH_RECOMMENDED_BUFFER_SIZE / 2];
		switch_frame_t frame = { 0 };
		int ev = 0;

		if (!call->engine || call->finished) return SWITCH_FALSE;
		frame.data = data;
		frame.buflen = sizeof(data);
		/* exactly one read per callback */
		if (switch_core_media_bug_read(bug, &frame, SWITCH_FALSE) == SWITCH_STATUS_SUCCESS && frame.datalen) {
			int16_t *s = (int16_t *) frame.data;
			uint32_t n = frame.datalen / 2 / (call->channels ? call->channels : 1), i;
			int fill = 1;
			if (n > sizeof(mono) / sizeof(mono[0])) n = sizeof(mono) / sizeof(mono[0]);
			for (i = 0; i < n; i++) {
				int16_t v = s[i * call->channels];
				if (v != -1) fill = 0;
				mono[i] = v;
			}
			if (fill) { /* no RTP: core fill pattern -> digital silence */
				memset(mono, 0, n * sizeof(int16_t));
				call->no_audio_ms += n * 1000 / call->rate;
			}
			ev = amvd_engine_feed(call->engine, mono, n);
		}
		if (ev && handle_events(call, ev)) return SWITCH_FALSE; /* prune: CLOSE follows */
		break;
	}

	case SWITCH_ABC_TYPE_CLOSE:
		if (call->engine) {
			if (!call->finished) {
				if (call->stopped || switch_channel_up(call->channel)) {
					handle_events(call, amvd_engine_finish(call->engine));
				} else if (!call->result_applied) {
					amvd_engine_finish(call->engine);
					apply_result(call, "hangup");
				} else {
					amvd_engine_finish(call->engine); /* hung up while waiting for message end */
				}
			}
			call_release(call);
		}
		break;

	default:
		break;
	}
	return SWITCH_TRUE;
}

/* ------------------------------------------------------------------ */
/* Start / stop                                                        */
/* ------------------------------------------------------------------ */

static switch_status_t amvd_start(switch_core_session_t *session, const char *data, int background, amvd_call_t **out, const char **err)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	switch_codec_implementation_t impl = { 0 };
	amvd_call_t *call;
	amvd_config_t cfg;
	amvd_profile_t *prof;
	const char *pname = NULL, *v;
	char *argv[32] = { 0 }, *mydata = NULL;
	int argc = 0, i;
	char errbuf[512];

	*err = NULL;
	if (switch_channel_get_private(channel, AMVD_PRIVATE)) { *err = "already running on this channel"; return SWITCH_STATUS_FALSE; }
	if (!switch_channel_media_ready(channel)) {
		if (switch_channel_pre_answer(channel) != SWITCH_STATUS_SUCCESS) { *err = "no media"; return SWITCH_STATUS_FALSE; }
	}
	switch_core_session_get_read_impl(session, &impl);
	if (!impl.actual_samples_per_second) { *err = "no read codec"; return SWITCH_STATUS_FALSE; }

	if (!zstr(data)) {
		mydata = switch_core_session_strdup(session, data);
		argc = switch_separate_string(mydata, ' ', argv, sizeof(argv) / sizeof(argv[0]));
	}
	for (i = 0; i < argc; i++) if (argv[i] && !strchr(argv[i], '=')) { pname = argv[i]; break; }
	if (!pname) pname = switch_channel_get_variable(channel, "amvd_profile");

	call = switch_core_session_alloc(session, sizeof(*call));
	call->session = session;
	call->channel = channel;
	call->background = background;
	call->rate = impl.actual_samples_per_second;
	call->channels = impl.number_of_channels ? impl.number_of_channels : 1;

	switch_mutex_lock(globals.mutex);
	if (zstr(pname)) pname = globals.default_profile;
	prof = find_profile(globals.profiles, pname);
	if (!prof) prof = find_profile(globals.profiles, "default");
	cfg = prof->cfg;
	switch_copy_string(call->profile, prof->name, sizeof(call->profile));
	if (prof->mref) { prof->mref->refs++; call->mref = prof->mref; }
	switch_mutex_unlock(globals.mutex);

	if ((v = switch_channel_get_variable(channel, "amvd_params")) && amvd_config_apply_string(&cfg, v, errbuf, sizeof(errbuf)))
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, "AMVD: amvd_params: %s\n", errbuf);
	for (i = 0; i < argc; i++) {
		char *eq;
		if (!argv[i] || !(eq = strchr(argv[i], '='))) continue;
		*eq = '\0';
		if (amvd_config_set(&cfg, argv[i], eq + 1))
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, "AMVD: ignoring bad arg %s=%s\n", argv[i], eq + 1);
	}
	call->wait_end = cfg.wait_message_end;

	call->engine = amvd_engine_create(&cfg, call->rate, call->mref ? call->mref->model : NULL);
	if (!call->engine) {
		model_unref(call->mref);
		*err = "unsupported sample rate";
		return SWITCH_STATUS_FALSE;
	}

	switch_thread_rwlock_rdlock(globals.mi->rwlock); /* keep module mapped while in use */
	switch_mutex_lock(globals.mutex);
	globals.active++;
	globals.started++;
	switch_mutex_unlock(globals.mutex);

	call->t_start = switch_micro_time_now();
	switch_channel_set_variable(channel, "amvd_active", "true"); /* ESL: filter variable_amvd_active true */
	switch_channel_set_private(channel, AMVD_PRIVATE, call);
	if (switch_core_media_bug_add(session, AMVD_BUG_NAME, NULL, amvd_bug_callback, call, 0,
								  SMBF_READ_STREAM | SMBF_NO_PAUSE | SMBF_ONE_ONLY, &call->bug) != SWITCH_STATUS_SUCCESS) {
		call_release(call);
		*err = "cannot attach media bug";
		return SWITCH_STATUS_FALSE;
	}
	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "AMVD: started on %s profile=%s rate=%d %s%s\n",
					  switch_channel_get_name(channel), call->profile, call->rate, background ? "background" : "blocking",
					  call->wait_end ? " +message-end" : "");
	if (out) *out = call;
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t amvd_stop(switch_core_session_t *session)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	amvd_call_t *call = switch_channel_get_private(channel, AMVD_PRIVATE);
	if (!call || !call->bug) return SWITCH_STATUS_FALSE;
	call->stopped = 1;
	return switch_core_media_bug_remove(session, &call->bug);
}

#define AMVD_APP_SYNTAX "[profile] [key=value ...]"

SWITCH_STANDARD_APP(amvd_app)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	amvd_call_t *call = NULL;
	const char *err = NULL;

	if (amvd_start(session, data, 0, &call, &err) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "AMVD: %s\n", err);
		switch_channel_set_variable(channel, "amvd_result", "error");
		switch_channel_set_variable(channel, "amvd_reason", err);
		switch_channel_set_variable(channel, "AMDSTATUS", "NOTSURE");
		switch_channel_set_variable(channel, "AMDCAUSE", "FATAL_ERROR");
		return;
	}
	/* Reading frames drives the media bug; switch_ivr_sleep also keeps outbound media flowing. */
	while (!call->done && switch_channel_ready(channel)) {
		if (call->result_applied && !call->wait_end) break;
		switch_ivr_sleep(session, 20, SWITCH_FALSE, NULL);
	}
	if (!call->done && call->bug) {
		switch_core_media_bug_remove(session, &call->bug); /* CLOSE: finalise + release */
	}
}

SWITCH_STANDARD_APP(amvd_start_app)
{
	const char *err = NULL;
	if (amvd_start(session, data, 1, NULL, &err) != SWITCH_STATUS_SUCCESS) {
		switch_channel_t *channel = switch_core_session_get_channel(session);
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "AMVD: %s\n", err);
		switch_channel_set_variable(channel, "amvd_result", "error");
		switch_channel_set_variable(channel, "amvd_reason", err);
	}
}

SWITCH_STANDARD_APP(amvd_stop_app)
{
	amvd_stop(session);
}

#define UUID_AMVD_SYNTAX "<uuid> start [profile] [key=value ...] | <uuid> stop"
SWITCH_STANDARD_API(uuid_amvd_api)
{
	char *mydata = NULL, *argv[3] = { 0 };
	int argc = 0;
	switch_core_session_t *s;
	const char *err = NULL;

	if (zstr(cmd) || !(mydata = strdup(cmd)) || (argc = switch_separate_string(mydata, ' ', argv, 3)) < 2) {
		stream->write_function(stream, "-USAGE: %s\n", UUID_AMVD_SYNTAX);
		goto done;
	}
	if (!(s = switch_core_session_locate(argv[0]))) {
		stream->write_function(stream, "-ERR no such channel %s\n", argv[0]);
		goto done;
	}
	if (!strcasecmp(argv[1], "start")) {
		if (amvd_start(s, argc > 2 ? argv[2] : NULL, 1, NULL, &err) == SWITCH_STATUS_SUCCESS) stream->write_function(stream, "+OK\n");
		else stream->write_function(stream, "-ERR %s\n", err);
	} else if (!strcasecmp(argv[1], "stop")) {
		if (amvd_stop(s) == SWITCH_STATUS_SUCCESS) stream->write_function(stream, "+OK\n");
		else stream->write_function(stream, "-ERR not running\n");
	} else {
		stream->write_function(stream, "-USAGE: %s\n", UUID_AMVD_SYNTAX);
	}
	switch_core_session_rwunlock(s);
done:
	switch_safe_free(mydata);
	return SWITCH_STATUS_SUCCESS;
}

#define AMVD_API_SYNTAX "status | reload | profiles"
SWITCH_STANDARD_API(amvd_api)
{
	if (zstr(cmd) || !strcasecmp(cmd, "status")) {
		switch_mutex_lock(globals.mutex);
		stream->write_function(stream,
							   "mod_amvd %s\nactive: %d\nstarted: %llu\nhuman: %llu\nmachine: %llu\nfax: %llu\n"
							   "unavailable: %llu\nunknown: %llu\nhangup: %llu\nmessage_end: %llu\n",
							   AMVD_VERSION, globals.active, (unsigned long long)globals.started, (unsigned long long)globals.human,
							   (unsigned long long)globals.machine, (unsigned long long)globals.fax, (unsigned long long)globals.unavailable,
							   (unsigned long long)globals.unknown, (unsigned long long)globals.hangup, (unsigned long long)globals.msg_end);
		switch_mutex_unlock(globals.mutex);
	} else if (!strcasecmp(cmd, "reload")) {
		load_config();
		stream->write_function(stream, "+OK reloaded (live calls keep their settings)\n");
	} else if (!strcasecmp(cmd, "profiles")) {
		amvd_profile_t *p;
		switch_mutex_lock(globals.mutex);
		for (p = globals.profiles; p; p = p->next) {
			stream->write_function(stream, "%s%s model=%s window=%d total=%d machine_speech=%d human_max=%d wait_end=%d\n", p->name,
								   !strcasecmp(p->name, globals.default_profile) ? " (default)" : "", p->mref ? p->mref->path : "rules",
								   p->cfg.window_ms, p->cfg.total_analysis_ms, p->cfg.machine_speech_ms, p->cfg.human_max_greeting_ms,
								   p->cfg.wait_message_end);
		}
		switch_mutex_unlock(globals.mutex);
	} else {
		stream->write_function(stream, "-USAGE: amvd %s\n", AMVD_API_SYNTAX);
	}
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_LOAD_FUNCTION(mod_amvd_load)
{
	switch_application_interface_t *app;
	switch_api_interface_t *api;

	memset(&globals, 0, sizeof(globals));
	globals.pool = pool;
	switch_mutex_init(&globals.mutex, SWITCH_MUTEX_NESTED, pool);

	if (switch_event_reserve_subclass(AMVD_EVENT_RESULT) != SWITCH_STATUS_SUCCESS ||
		switch_event_reserve_subclass(AMVD_EVENT_MSGEND) != SWITCH_STATUS_SUCCESS ||
		switch_event_reserve_subclass(AMVD_EVENT_BEEP) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "amvd: cannot reserve event subclasses\n");
		return SWITCH_STATUS_TERM;
	}

	*module_interface = switch_loadable_module_create_module_interface(pool, modname);
	globals.mi = *module_interface;
	load_config();

	SWITCH_ADD_APP(app, "amvd", "Answering machine detection (blocking)", "Detect human/machine/fax/SIT; returns with ${amvd_result}",
				   amvd_app, AMVD_APP_SYNTAX, SAF_NONE);
	SWITCH_ADD_APP(app, "amvd_start", "Answering machine detection (background)", "Detect on a media bug; results as events/hooks",
				   amvd_start_app, AMVD_APP_SYNTAX, SAF_MEDIA_TAP);
	SWITCH_ADD_APP(app, "amvd_stop", "Stop background answering machine detection", "", amvd_stop_app, "", SAF_NONE);
	SWITCH_ADD_API(api, "uuid_amvd", "Answering machine detection on a channel", uuid_amvd_api, UUID_AMVD_SYNTAX);
	SWITCH_ADD_API(api, "amvd", "mod_amvd control", amvd_api, AMVD_API_SYNTAX);
	switch_console_set_complete("add uuid_amvd ::console::list_uuid start");
	switch_console_set_complete("add uuid_amvd ::console::list_uuid stop");
	switch_console_set_complete("add amvd status");
	switch_console_set_complete("add amvd reload");
	switch_console_set_complete("add amvd profiles");

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_amvd %s loaded (default profile %s)\n", AMVD_VERSION, globals.default_profile);
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_amvd_shutdown)
{
	switch_console_set_complete("del uuid_amvd");
	switch_console_set_complete("del amvd");
	switch_event_free_subclass(AMVD_EVENT_RESULT);
	switch_event_free_subclass(AMVD_EVENT_MSGEND);
	switch_event_free_subclass(AMVD_EVENT_BEEP);
	free_profiles(globals.profiles);
	globals.profiles = NULL;
	return SWITCH_STATUS_SUCCESS;
}

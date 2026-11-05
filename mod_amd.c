#include <switch.h>

#define AMD_PARAMS (2)
#define AMD_SYNTAX "<uuid> <command>"
#define AMD_BACKGROUND_SYNTAX "[start|stop]"
#define AMD_VOICE_START_SYNTAX "[key=value,key=value,...]"

/* Media bug name and the channel private slot used to keep track of it */
#define AMD_BUG_FUNCTION "amd"
#define AMD_BUG_PRIVATE "_amd_background_"

/* mod_com_amd accepts at most this many key=value overrides on voice_start */
#define AMD_MAX_OVERRIDES 20

/* There is no licence server: report an unlimited number of AMD channels */
#define AMD_UNLIMITED_CHANNELS 65535

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_amd_shutdown);
SWITCH_MODULE_LOAD_FUNCTION(mod_amd_load);
SWITCH_MODULE_DEFINITION(mod_amd, mod_amd_load, mod_amd_shutdown, NULL);
SWITCH_STANDARD_APP(amd_start_function);
SWITCH_STANDARD_APP(amd_background_function);
SWITCH_STANDARD_APP(voice_start_function);
SWITCH_STANDARD_APP(voice_stop_function);
SWITCH_STANDARD_APP(waitforresult_function);
SWITCH_STANDARD_API(amd_available_api);
SWITCH_STANDARD_API(amd_count_api);
SWITCH_STANDARD_API(amd_info_api);

/* Which interface a detection was started through. */
typedef enum {
	/* amd / amd_background: HUMAN / MACHINE / NOTSURE + CUSTOM amd::result */
	AMD_MODE_NATIVE,
	/* voice_start / waitforresult: mod_com_amd compatible person / machine /
	 * unsure + DETECTED_SPEECH */
	AMD_MODE_COMPAT
} amd_mode_t;

/* A rule matched by the VAD. The status/result strings it maps to depend on
 * the mode (see amd_publish()). */
typedef enum {
	AMD_DECISION_NONE = 0,
	AMD_DECISION_INITIAL_SILENCE,
	AMD_DECISION_AFTER_INTRO_SILENCE,
	AMD_DECISION_LONG_GREETING,
	AMD_DECISION_MAX_WORD_LENGTH,
	AMD_DECISION_MAX_WORDS,
	AMD_DECISION_TIMEOUT
} amd_decision_t;

typedef enum {
	SILENCE,
	VOICED
} amd_frame_classifier;

typedef enum {
	VAD_STATE_IN_WORD,
	VAD_STATE_IN_SILENCE,
} amd_vad_state_t;

typedef struct {
	/* Shared by both interfaces */
	uint32_t silence_threshold;
	uint32_t initial_silence;
	uint32_t after_greeting_silence;
	uint32_t greeting;
	uint32_t minimum_word_length;
	uint32_t between_words_silence;
	uint32_t maximum_number_of_words;
	uint32_t maximum_word_length;
	uint32_t total_analysis_time;	/* milliseconds, 0 = unlimited */

	/* mod_com_amd compatible options (native defaults leave them unused) */
	switch_bool_t hangup_on_bug_fail;
	switch_bool_t event_fire;
	switch_bool_t debug;
	switch_log_level_t debug_loglevel;
} amd_settings_t;

/*
 * Two independent default sets:
 *
 * - native is what the original amd / amd_background applications have always
 *   used (Asterisk style).
 * - compat mirrors the values packaged in mod_com_amd's amd.conf.xml so that
 *   voice_start behaves like the commercial module out of the box.
 *
 * The configuration file only overrides the interface whose parameter names it
 * uses (see amd_apply_config_value()), plus silence_threshold and
 * total_analysis_time which have the same name in both.
 */
static struct {
	amd_settings_t native;
	amd_settings_t compat;
} globals;

static void amd_settings_defaults(amd_settings_t *settings, amd_mode_t mode)
{
	memset(settings, 0, sizeof(*settings));

	settings->silence_threshold = 256;
	settings->total_analysis_time = 5000;
	settings->debug_loglevel = SWITCH_LOG_DEBUG;

	if (mode == AMD_MODE_COMPAT) {
		settings->initial_silence = 4500;
		settings->after_greeting_silence = 500;
		settings->greeting = 1250;
		settings->minimum_word_length = 120;
		settings->between_words_silence = 50;
		settings->maximum_number_of_words = 3;
		/* mod_com_amd has no single word length rule */
		settings->maximum_word_length = 0;
		settings->hangup_on_bug_fail = SWITCH_TRUE;
		settings->event_fire = SWITCH_FALSE;
	} else {
		settings->initial_silence = 2500;
		settings->after_greeting_silence = 800;
		settings->greeting = 1500;
		settings->minimum_word_length = 100;
		settings->between_words_silence = 50;
		settings->maximum_number_of_words = 3;
		settings->maximum_word_length = 5000;
	}
}

static uint32_t amd_strtoul(const char *value)
{
	return (uint32_t) strtoul(value, NULL, 10);
}

/*
 * Set one parameter on a settings struct. Both the native names and the
 * mod_com_amd names are accepted, which lets voice_start overrides use either
 * family. Returns SWITCH_FALSE for names that are neither known nor explicitly
 * ignored.
 */
static switch_bool_t amd_settings_set(amd_settings_t *settings, const char *name, const char *value)
{
	if (!strcasecmp(name, "silence_threshold") || !strcasecmp(name, "silent_threshold")) {
		settings->silence_threshold = amd_strtoul(value);
	} else if (!strcasecmp(name, "total_analysis_time")) {
		settings->total_analysis_time = amd_strtoul(value);
	} else if (!strcasecmp(name, "initial_silence") || !strcasecmp(name, "silent_initial")) {
		settings->initial_silence = amd_strtoul(value);
	} else if (!strcasecmp(name, "greeting") || !strcasecmp(name, "noise_max_intro")) {
		settings->greeting = amd_strtoul(value);
	} else if (!strcasecmp(name, "after_greeting_silence") || !strcasecmp(name, "silent_after_intro")) {
		settings->after_greeting_silence = amd_strtoul(value);
	} else if (!strcasecmp(name, "min_word_length") || !strcasecmp(name, "noise_min_length")) {
		settings->minimum_word_length = amd_strtoul(value);
	} else if (!strcasecmp(name, "between_words_silence") || !strcasecmp(name, "noise_inter_silence")) {
		settings->between_words_silence = amd_strtoul(value);
	} else if (!strcasecmp(name, "maximum_number_of_words") || !strcasecmp(name, "noise_max_count")) {
		settings->maximum_number_of_words = amd_strtoul(value);
	} else if (!strcasecmp(name, "maximum_word_length")) {
		settings->maximum_word_length = amd_strtoul(value);
	} else if (!strcasecmp(name, "hangup_on_bug_fail")) {
		settings->hangup_on_bug_fail = switch_true(value);
	} else if (!strcasecmp(name, "event_fire")) {
		settings->event_fire = switch_true(value);
	} else if (!strcasecmp(name, "debug")) {
		settings->debug = switch_true(value);
	} else if (!strcasecmp(name, "debug_loglevel")) {
		int level = atoi(value);

		if (level >= SWITCH_LOG_ALERT && level <= SWITCH_LOG_DEBUG) {
			settings->debug_loglevel = (switch_log_level_t) level;
		}
	} else if (!strcasecmp(name, "silent_max_session") ||
			   !strcasecmp(name, "ring_timeout") ||
			   !strcasecmp(name, "break_digit")) {
		/* Accepted for mod_com_amd compatibility, not used by the module */
	} else {
		return SWITCH_FALSE;
	}

	return SWITCH_TRUE;
}

/*
 * Apply one <param name=... value=.../> entry. Native and mod_com_amd use
 * different names for the same concept; a name only updates the settings of the
 * interface it belongs to, so a native amd.conf does not silently change the
 * defaults of voice_start (and vice versa). silence_threshold and
 * total_analysis_time have the same name in both and update both.
 *
 * Returns SWITCH_FALSE for names that are neither known nor explicitly ignored.
 */
static switch_bool_t amd_apply_config_value(const char *name, const char *value)
{
	if (!strcasecmp(name, "silence_threshold") ||
		!strcasecmp(name, "silent_threshold") ||
		!strcasecmp(name, "total_analysis_time")) {
		return amd_settings_set(&globals.native, name, value) &&
			amd_settings_set(&globals.compat, name, value);
	}

	if (!strcasecmp(name, "initial_silence") ||
		!strcasecmp(name, "greeting") ||
		!strcasecmp(name, "after_greeting_silence") ||
		!strcasecmp(name, "min_word_length") ||
		!strcasecmp(name, "between_words_silence") ||
		!strcasecmp(name, "maximum_number_of_words") ||
		!strcasecmp(name, "maximum_word_length")) {
		return amd_settings_set(&globals.native, name, value);
	}

	if (!strcasecmp(name, "silent_initial") ||
		!strcasecmp(name, "noise_max_intro") ||
		!strcasecmp(name, "silent_after_intro") ||
		!strcasecmp(name, "noise_min_length") ||
		!strcasecmp(name, "noise_inter_silence") ||
		!strcasecmp(name, "noise_max_count") ||
		!strcasecmp(name, "hangup_on_bug_fail") ||
		!strcasecmp(name, "event_fire") ||
		!strcasecmp(name, "debug") ||
		!strcasecmp(name, "debug_loglevel")) {
		return amd_settings_set(&globals.compat, name, value);
	}

	if (!strcasecmp(name, "silent_max_session") ||
		!strcasecmp(name, "ring_timeout") ||
		!strcasecmp(name, "break_digit")) {
		return SWITCH_TRUE;
	}

	return SWITCH_FALSE;
}

static switch_status_t do_config(void)
{
	switch_xml_t cfg, xml, settings, param;

	amd_settings_defaults(&globals.native, AMD_MODE_NATIVE);
	amd_settings_defaults(&globals.compat, AMD_MODE_COMPAT);

	if (!(xml = switch_xml_open_cfg("amd.conf", &cfg, NULL))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
						  "Could not open amd.conf, using built-in defaults\n");
		return SWITCH_STATUS_FALSE;
	}

	if ((settings = switch_xml_child(cfg, "settings"))) {
		for (param = switch_xml_child(settings, "param"); param; param = param->next) {
			const char *name = switch_xml_attr_soft(param, "name");
			const char *value = switch_xml_attr_soft(param, "value");

			if (zstr(name) || zstr(value)) {
				continue;
			}

			if (!amd_apply_config_value(name, value)) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
								  "Configuration parameter [%s] is unfortunately not valid, "
								  "you might want to double-check that.\n", name);
			}
		}
	}

	switch_xml_free(xml);

	return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_LOAD_FUNCTION(mod_amd_load)
{
	switch_application_interface_t *app_interface;
	switch_api_interface_t *api_interface;

	*module_interface = switch_loadable_module_create_module_interface(pool, modname);

	do_config();

	SWITCH_ADD_APP(
		app_interface,
		"amd",
		"Voice activity detection (blocking)",
		"Asterisk's AMD (Blocking)",
		amd_start_function,
		NULL,
		SAF_NONE);

	SWITCH_ADD_APP(
		app_interface,
		"amd_background",
		"Voice activity detection (non blocking)",
		"Start or stop voice activity detection in the background. The result is\n"
		"published as the channel variables amd_result/amd_cause and as a\n"
		"CUSTOM amd::result event. Detection keeps running while the channel\n"
		"plays audio or waits for input.",
		amd_background_function,
		AMD_BACKGROUND_SYNTAX,
		SAF_NONE);

	/*
	 * mod_com_amd compatible interface: voice_start returns immediately, the
	 * verdict is published as amd_status/amd_result and as a DETECTED_SPEECH
	 * event, and waitforresult pumps the read leg until the verdict arrives.
	 */
	SWITCH_ADD_APP(
		app_interface,
		"voice_start",
		"Answering machine detection (mod_com_amd compatible, non blocking)",
		"Start answering machine detection in the background. The arguments are\n"
		"optional comma separated key=value overrides of the amd.conf settings.\n"
		"The verdict is published as amd_status/amd_result and as a DETECTED_SPEECH\n"
		"event.",
		voice_start_function,
		AMD_VOICE_START_SYNTAX,
		SAF_NONE);

	SWITCH_ADD_APP(
		app_interface,
		"voice_stop",
		"Stop answering machine detection (mod_com_amd compatible)",
		"Stop a detection started with voice_start. A stopped detection has no\n"
		"verdict to report.",
		voice_stop_function,
		NULL,
		SAF_NONE);

	SWITCH_ADD_APP(
		app_interface,
		"waitforresult",
		"Wait for an answering machine detection result",
		"Answer the channel if needed and wait until the detection started with\n"
		"voice_start produces a verdict. The optional argument is a file played\n"
		"while waiting (silence when omitted).",
		waitforresult_function,
		"[file]",
		SAF_NONE);

	SWITCH_ADD_API(api_interface, "amd_available", "Answering machine detection availability",
				   amd_available_api, "");
	SWITCH_ADD_API(api_interface, "amd_count", "Free answering machine detection channels",
				   amd_count_api, "");
	SWITCH_ADD_API(api_interface, "amd_info", "Answering machine detection channel usage",
				   amd_info_api, "");

	return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_amd_shutdown)
{
	return SWITCH_STATUS_SUCCESS;
}

static void amd_fire_native_event(switch_core_session_t *session, switch_channel_t *channel,
								  const char *result, const char *cause)
{
	switch_event_t *event;

	if (switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, "amd::result") != SWITCH_STATUS_SUCCESS) {
		return;
	}

	switch_channel_event_set_data(channel, event);

	switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "AMD-Result", switch_str_nil(result));
	switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "AMD-Cause", switch_str_nil(cause));

	switch_event_fire(&event);
}

/*
 * Deliver the DETECTED_SPEECH completion event the way mod_com_amd does:
 *
 * - event_fire=false queues it to the session without channel headers. With
 *   divert_events enabled (outbound Event Socket) the queued event is drained
 *   straight to the socket.
 * - event_fire=true sends it to the event system with the channel headers.
 */
static void amd_queue_speech_event(switch_core_session_t *session, switch_channel_t *channel,
								   switch_bool_t event_fire, const char *status, const char *result)
{
	switch_event_t *event;

	if (switch_event_create(&event, SWITCH_EVENT_DETECTED_SPEECH) != SWITCH_STATUS_SUCCESS) {
		return;
	}

	switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Speech-Type", "detected-speech");
	switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "amd_status", switch_str_nil(status));
	switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "amd_result", switch_str_nil(result));
	switch_event_add_body(event, "%s", "amd_complete");

	if (event_fire) {
		switch_event_t *dup;

		if (switch_event_dup(&dup, event) == SWITCH_STATUS_SUCCESS) {
			switch_channel_event_set_data(channel, dup);
			switch_event_fire(&dup);
		}

		switch_event_destroy(&event);
		return;
	}

	if (switch_core_session_queue_event(session, &event) != SWITCH_STATUS_SUCCESS) {
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "delivery-failure", "true");
		switch_channel_event_set_data(channel, event);
		switch_event_fire(&event);
	}
}

typedef struct {
	switch_core_session_t *session;
	switch_channel_t *channel;
	const amd_settings_t *settings;
	amd_mode_t mode;

	amd_vad_state_t state;
	uint32_t frame_ms;
	uint32_t silence_duration;
	uint32_t voice_duration;
	uint32_t words;

	uint32_t in_initial_silence:1;
	uint32_t in_greeting:1;
	uint32_t talking:1;
	uint32_t published:1;
} amd_vad_t;

static void amd_emit_talk_event(amd_vad_t *vad, switch_bool_t talking)
{
	switch_event_t *event;

	if (vad->mode != AMD_MODE_COMPAT || vad->talking == talking) {
		return;
	}

	vad->talking = talking;

	if (switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, "AMD::EVENT") != SWITCH_STATUS_SUCCESS) {
		return;
	}

	switch_channel_event_set_data(vad->channel, event);
	switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Action",
								   talking ? "start-talking" : "stop-talking");
	switch_event_fire(&event);
}

/*
 * Check that an application name (optionally written as "app::arg") exists
 * before it is queued on the session.
 */
static switch_bool_t amd_application_exists(const char *app)
{
	char *check = strdup(app);
	char *sep = check ? strstr(check, "::") : NULL;
	int32_t flags = 0;
	switch_bool_t found;

	if (sep) {
		*sep = '\0';
	}

	found = check && switch_core_session_get_app_flags(check, &flags) == SWITCH_STATUS_SUCCESS;

	switch_safe_free(check);

	return found;
}

/*
 * Run the mod_com_amd amd_execute_on_<status>* channel variables. Each
 * variable whose name begins with the prefix holds an application (and an
 * optional argument) that is queued on the session when the detection
 * completes, so it runs while the channel is still in the application it was
 * executing. Variables are expanded when the action runs, not when it is
 * queued.
 */
static void amd_run_execute_on(amd_vad_t *vad, const char *status)
{
	switch_event_t *vars = NULL;
	switch_event_header_t *hp;
	char prefix[64];

	if (zstr(status)) {
		return;
	}

	switch_snprintf(prefix, sizeof(prefix), "amd_execute_on_%s", status);

	switch_channel_get_variables_prefix(vad->channel, prefix, &vars);

	if (vars == NULL) {
		return;
	}

	for (hp = vars->headers; hp; hp = hp->next) {
		char *app, *arg = NULL;
		char *expanded = NULL;

		/*
		 * switch_channel_get_variables_prefix() hands back a channel data
		 * event, so the matching variables sit next to its own Event-Name
		 * header. Only the prefixed headers are actions.
		 */
		if (strncmp(hp->name, prefix, strlen(prefix)) || zstr(hp->value)) {
			continue;
		}

		app = switch_core_session_strdup(vad->session, hp->value);

		if ((arg = strchr(app, ' '))) {
			*arg++ = '\0';

			while (*arg == ' ') {
				arg++;
			}
		}

		if (!zstr(arg)) {
			expanded = switch_channel_expand_variables(vad->channel, arg);

			if (expanded == arg) {
				expanded = NULL;
			}
		} else {
			arg = NULL;
		}

		/*
		 * A queued application that does not exist makes the core hang the
		 * channel up (Invalid Application), so check it first: a typo in a
		 * dialplan variable should not drop a call. The check stops at the
		 * "app::arg" separator, which the core splits itself.
		 */
		if (!amd_application_exists(app)) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(vad->session), SWITCH_LOG_WARNING,
							  "AMD: Ignoring unknown application '%s' for result %s\n", app, status);
			switch_safe_free(expanded);
			continue;
		}

		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(vad->session), SWITCH_LOG_INFO,
						  "AMD: Running '%s' (%s) for result %s\n", app,
						  switch_str_nil(expanded ? expanded : arg), status);

		switch_core_session_execute_application_async(vad->session, app, expanded ? expanded : arg);

		switch_safe_free(expanded);
	}

	switch_event_destroy(&vars);
}

/*
 * Publish a verdict using the conventions of the interface the detection was
 * started through.
 */
static void amd_publish(amd_vad_t *vad, amd_decision_t decision)
{
	const char *result;
	const char *cause;

	if (vad->published) {
		return;
	}

	vad->published = SWITCH_TRUE;

	if (vad->mode == AMD_MODE_COMPAT) {
		const char *status = "unsure";

		result = "too-long";

		switch (decision) {
		case AMD_DECISION_INITIAL_SILENCE:
			status = "person";
			result = "silent-initial";
			break;
		case AMD_DECISION_AFTER_INTRO_SILENCE:
			status = "person";
			result = "silent-after-intro";
			break;
		case AMD_DECISION_LONG_GREETING:
		case AMD_DECISION_MAX_WORD_LENGTH:
		case AMD_DECISION_MAX_WORDS:
			status = "machine";
			result = "max-count";
			break;
		case AMD_DECISION_TIMEOUT:
		case AMD_DECISION_NONE:
			break;
		}

		switch_channel_set_variable(vad->channel, "amd_status", status);
		switch_channel_set_variable(vad->channel, "amd_result", result);

		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(vad->session), SWITCH_LOG_INFO,
						  "AMD Status: %s (%s)\n", status, result);

		if (vad->settings->debug) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(vad->session), vad->settings->debug_loglevel,
							  "AMD: status=%s result=%s silence=%u voice=%u words=%u\n",
							  status, result, vad->silence_duration, vad->voice_duration, vad->words);
		}

		amd_queue_speech_event(vad->session, vad->channel, vad->settings->event_fire, status, result);

		amd_run_execute_on(vad, status);

		return;
	}

	result = "NOTSURE";
	cause = "TOOLONG";

	switch (decision) {
	case AMD_DECISION_INITIAL_SILENCE:
		result = "MACHINE";
		cause = "INITIALSILENCE";
		break;
	case AMD_DECISION_AFTER_INTRO_SILENCE:
		result = "HUMAN";
		cause = "HUMAN";
		break;
	case AMD_DECISION_LONG_GREETING:
		result = "MACHINE";
		cause = "LONGGREETING";
		break;
	case AMD_DECISION_MAX_WORD_LENGTH:
		result = "MACHINE";
		cause = "MAXWORDLENGTH";
		break;
	case AMD_DECISION_MAX_WORDS:
		result = "MACHINE";
		cause = "MAXWORDS";
		break;
	case AMD_DECISION_TIMEOUT:
	case AMD_DECISION_NONE:
		break;
	}

	switch_channel_set_variable(vad->channel, "amd_result", result);
	switch_channel_set_variable(vad->channel, "amd_cause", cause);

	amd_fire_native_event(vad->session, vad->channel, result, cause);
}

static amd_frame_classifier classify_frame(const switch_frame_t *f,
										   const switch_codec_implementation_t *codec,
										   const amd_settings_t *settings)
{
	int16_t *audio = f->data;
	uint32_t score, count, j;
	double energy;
	int divisor;

	if (f->samples == 0 || f->datalen == 0) {
		return SILENCE;
	}

	divisor = codec->actual_samples_per_second / 8000;

	for (energy = 0, j = 0, count = 0; count < f->samples; count++) {
		energy += abs(audio[j++]);
		j += codec->number_of_channels;
	}

	score = (uint32_t) (energy / (f->samples / divisor));

	if (score >= settings->silence_threshold) {
		return VOICED;
	}

	return SILENCE;
}

static amd_decision_t amd_handle_silence_frame(amd_vad_t *vad)
{
	const amd_settings_t *settings = vad->settings;

	vad->silence_duration += vad->frame_ms;

	if (vad->silence_duration >= settings->between_words_silence) {
		if (vad->state != VAD_STATE_IN_SILENCE) {
			if (vad->mode == AMD_MODE_COMPAT) {
				amd_emit_talk_event(vad, SWITCH_FALSE);
			} else if (settings->debug) {
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(vad->session), SWITCH_LOG_DEBUG,
								  "AMD: Changed state to VAD_STATE_IN_SILENCE\n");
			}
		}

		vad->state = VAD_STATE_IN_SILENCE;
		vad->voice_duration = 0;
	}

	if (vad->in_initial_silence && vad->silence_duration >= settings->initial_silence) {
		if (settings->debug) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(vad->session), SWITCH_LOG_DEBUG,
							  "AMD: Initial silence (silence_duration: %u, initial_silence: %u)\n",
							  vad->silence_duration, settings->initial_silence);
		}

		return AMD_DECISION_INITIAL_SILENCE;
	}

	if (vad->in_greeting && vad->silence_duration >= settings->after_greeting_silence) {
		if (settings->debug) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(vad->session), SWITCH_LOG_DEBUG,
							  "AMD: Silence after intro (silence_duration: %u, after_greeting_silence: %u)\n",
							  vad->silence_duration, settings->after_greeting_silence);
		}

		return AMD_DECISION_AFTER_INTRO_SILENCE;
	}

	return AMD_DECISION_NONE;
}

static amd_decision_t amd_handle_voiced_frame(amd_vad_t *vad)
{
	const amd_settings_t *settings = vad->settings;
	switch_bool_t count_word;

	vad->voice_duration += vad->frame_ms;

	if (vad->mode == AMD_MODE_COMPAT) {
		/*
		 * noise_inter_silence is a word-count gate: a burst only counts as a
		 * word once at least that many ms have passed since the last silent
		 * frame. At this point the burst itself is voice_duration long, which
		 * reproduces mod_com_amd's quirk (a value at or below noise_min_length
		 * never blocks a word).
		 */
		count_word = vad->voice_duration >= settings->minimum_word_length &&
			vad->voice_duration >= settings->between_words_silence;
	} else {
		count_word = vad->voice_duration >= settings->minimum_word_length;
	}

	if (count_word && vad->state == VAD_STATE_IN_SILENCE) {
		vad->words++;

		if (vad->mode == AMD_MODE_COMPAT) {
			amd_emit_talk_event(vad, SWITCH_TRUE);
		} else if (settings->debug) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(vad->session), SWITCH_LOG_DEBUG,
							  "AMD: Word detected (words: %u)\n", vad->words);
		}

		vad->state = VAD_STATE_IN_WORD;
	}

	if (settings->maximum_word_length && vad->voice_duration >= settings->maximum_word_length) {
		if (settings->debug) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(vad->session), SWITCH_LOG_DEBUG,
							  "AMD: Long word (voice_duration: %u, maximum_word_length: %u)\n",
							  vad->voice_duration, settings->maximum_word_length);
		}

		return AMD_DECISION_MAX_WORD_LENGTH;
	}

	if (settings->maximum_number_of_words && vad->words >= settings->maximum_number_of_words) {
		if (settings->debug) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(vad->session), SWITCH_LOG_DEBUG,
							  "AMD: Word count reached (words: %u, maximum_number_of_words: %u)\n",
							  vad->words, settings->maximum_number_of_words);
		}

		return AMD_DECISION_MAX_WORDS;
	}

	if (vad->in_greeting && vad->voice_duration >= settings->greeting) {
		if (settings->debug) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(vad->session), SWITCH_LOG_DEBUG,
							  "AMD: Long greeting (voice_duration: %u, greeting: %u)\n",
							  vad->voice_duration, settings->greeting);
		}

		return AMD_DECISION_LONG_GREETING;
	}

	if (vad->voice_duration >= settings->minimum_word_length) {
		if (vad->silence_duration && settings->debug) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(vad->session), SWITCH_LOG_DEBUG,
							  "AMD: Detected Talk, previous silence duration: %ums\n",
							  vad->silence_duration);
		}

		vad->silence_duration = 0;
	}

	if (count_word && !vad->in_greeting) {
		if (vad->silence_duration && settings->debug) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(vad->session), SWITCH_LOG_DEBUG,
							  "AMD: Before Greeting Time (silence_duration: %u, voice_duration: %u)\n",
							  vad->silence_duration, vad->voice_duration);
		}

		vad->in_initial_silence = 0;
		vad->in_greeting = 1;
	}

	return AMD_DECISION_NONE;
}

/*
 * Background (non blocking) detection.
 *
 * Both amd_background (native) and voice_start (mod_com_amd compatible) attach
 * a media bug: the application returns as soon as the bug is in place and the
 * audio keeps flowing through the bug while the channel is being used for
 * something else (playback / waitforresult pumps the read leg once per frame).
 */
typedef struct {
	amd_vad_t vad;
	amd_settings_t settings;
	switch_media_bug_t *bug;
	switch_time_t start_time;
	switch_bool_t finished;
} amd_bug_t;

static void amd_finish(amd_bug_t *abug, amd_decision_t decision)
{
	abug->finished = SWITCH_TRUE;
	amd_publish(&abug->vad, decision);
}

static switch_bool_t amd_bug_callback(switch_media_bug_t *bug, void *user_data, switch_abc_type_t type)
{
	amd_bug_t *abug = (amd_bug_t *) user_data;
	switch_bool_t rval = SWITCH_TRUE;

	if (abug == NULL) {
		return SWITCH_FALSE;
	}

	switch (type) {
	case SWITCH_ABC_TYPE_INIT:
		if (abug->settings.debug) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(abug->vad.session), SWITCH_LOG_DEBUG,
							  "AMD: Background detection initialized\n");
		}
		break;

	case SWITCH_ABC_TYPE_CLOSE:
		if (switch_channel_get_private(abug->vad.channel, AMD_BUG_PRIVATE) == abug) {
			switch_channel_set_private(abug->vad.channel, AMD_BUG_PRIVATE, NULL);
		}

		/* Reached when the bug is pruned before any verdict was made */
		if (!abug->finished && switch_channel_ready(abug->vad.channel)) {
			if (abug->settings.debug) {
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(abug->vad.session), SWITCH_LOG_DEBUG,
								  "AMD: Timeout (elapsed: %u ms)\n",
								  (uint32_t) ((switch_micro_time_now() - abug->start_time) / 1000));
			}

			amd_finish(abug, AMD_DECISION_TIMEOUT);
		}

		return SWITCH_TRUE;

	case SWITCH_ABC_TYPE_READ_REPLACE:
	{
		switch_frame_t *read_frame = switch_core_media_bug_get_read_replace_frame(bug);
		switch_codec_implementation_t read_impl = { 0 };
		uint32_t elapsed_ms;
		amd_decision_t decision = AMD_DECISION_NONE;

		if (abug->finished || read_frame == NULL) {
			break;
		}

		switch_core_session_get_read_impl(abug->vad.session, &read_impl);

		/*
		 * Frames generated by the endpoint while the far end is silent carry
		 * no samples at all, use the negotiated packet time for them so that
		 * silence keeps being accounted for.
		 */
		if (read_frame->samples > 0 && read_frame->samples <= read_impl.actual_samples_per_second) {
			abug->vad.frame_ms = 1000 / (read_impl.actual_samples_per_second / read_frame->samples);
		} else {
			abug->vad.frame_ms = read_impl.microseconds_per_packet / 1000;
		}

		if (abug->vad.frame_ms == 0) {
			abug->vad.frame_ms = 20;
		}

		elapsed_ms = (uint32_t) ((switch_micro_time_now() - abug->start_time) / 1000);

		if (abug->settings.total_analysis_time && elapsed_ms >= abug->settings.total_analysis_time) {
			if (abug->settings.debug) {
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(abug->vad.session), SWITCH_LOG_DEBUG,
								  "AMD: Timeout (elapsed: %u ms)\n", elapsed_ms);
			}

			amd_finish(abug, AMD_DECISION_TIMEOUT);
			rval = SWITCH_FALSE;
			break;
		}

		switch (classify_frame(read_frame, &read_impl, &abug->settings)) {
		case SILENCE:
			decision = amd_handle_silence_frame(&abug->vad);
			break;

		case VOICED:
		default:
			decision = amd_handle_voiced_frame(&abug->vad);
			break;
		}

		if (decision != AMD_DECISION_NONE) {
			amd_finish(abug, decision);
			rval = SWITCH_FALSE;
		}

		break;
	}

	default:
		break;
	}

	return rval;
}

static switch_status_t amd_add_bug(switch_core_session_t *session, switch_channel_t *channel,
								   const amd_settings_t *settings, amd_mode_t mode)
{
	amd_bug_t *abug;
	switch_status_t status;
	time_t stop_time;

	abug = (amd_bug_t *) switch_core_session_alloc(session, sizeof(*abug));

	if (abug == NULL) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
						  "AMD: Unable to allocate memory\n");
		return SWITCH_STATUS_FALSE;
	}

	memset(abug, 0, sizeof(*abug));

	abug->settings = *settings;
	abug->vad.session = session;
	abug->vad.channel = channel;
	abug->vad.settings = &abug->settings;
	abug->vad.mode = mode;
	abug->vad.state = VAD_STATE_IN_WORD;
	abug->vad.in_initial_silence = 1;
	abug->start_time = switch_micro_time_now();

	if (abug->settings.total_analysis_time) {
		/*
		 * stop_time is a seconds based fallback for a read leg that nothing
		 * pumps. The verdict itself is timed in milliseconds by the bug
		 * callback, so round up: pruning the bug one second too early would
		 * cut an analysis short before it could reach a decision.
		 */
		stop_time = switch_epoch_time_now(NULL) +
			((abug->settings.total_analysis_time + 999) / 1000) + 1;
	} else {
		stop_time = 0;
	}

	status = switch_core_media_bug_add(
		session,
		AMD_BUG_FUNCTION,
		NULL,
		amd_bug_callback,
		abug,
		stop_time,
		SMBF_READ_REPLACE | SMBF_ONE_ONLY,
		&abug->bug);

	if (status != SWITCH_STATUS_SUCCESS || abug->bug == NULL) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
						  "AMD: Unable to add media bug (%d)\n", status);
		return status;
	}

	switch_channel_set_private(channel, AMD_BUG_PRIVATE, abug);

	if (abug->settings.debug) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
						  "AMD: Background detection started\n");
	}

	return SWITCH_STATUS_SUCCESS;
}

SWITCH_STANDARD_APP(amd_background_function)
{
	switch_channel_t *channel;
	amd_bug_t *abug = NULL;
	const char *cmd = zstr(data) ? "start" : data;

	if (!session) {
		return;
	}

	channel = switch_core_session_get_channel(session);

	if (!switch_channel_ready(channel)) {
		return;
	}

	abug = (amd_bug_t *) switch_channel_get_private(channel, AMD_BUG_PRIVATE);

	if (!strcasecmp(cmd, "stop")) {
		if (abug == NULL) {
			switch_log_printf(
				SWITCH_CHANNEL_SESSION_LOG(session),
				SWITCH_LOG_DEBUG,
				"AMD: Background detection is not running\n");
			return;
		}

		/* Silence the callback, a stopped detection has no verdict to report */
		abug->finished = SWITCH_TRUE;
		switch_channel_set_private(channel, AMD_BUG_PRIVATE, NULL);
		switch_core_media_bug_remove(session, &abug->bug);

		switch_log_printf(
			SWITCH_CHANNEL_SESSION_LOG(session),
			SWITCH_LOG_DEBUG,
			"AMD: Background detection stopped\n");
		return;
	}

	if (strcasecmp(cmd, "start")) {
		switch_log_printf(
			SWITCH_CHANNEL_SESSION_LOG(session),
			SWITCH_LOG_ERROR,
			"AMD: Unknown command '%s', expected start or stop\n",
			cmd);
		return;
	}

	if (abug != NULL) {
		switch_log_printf(
			SWITCH_CHANNEL_SESSION_LOG(session),
			SWITCH_LOG_ERROR,
			"AMD: Background detection already running\n");
		return;
	}

	amd_add_bug(session, channel, &globals.native, AMD_MODE_NATIVE);
}

static char *amd_trim(char *s)
{
	char *end;

	while (*s == ' ' || *s == '\t') {
		s++;
	}

	end = s + strlen(s);

	while (end > s && (end[-1] == ' ' || end[-1] == '\t')) {
		*--end = '\0';
	}

	return s;
}

/*
 * Parse the optional comma separated key=value overrides of voice_start into a
 * per detection copy of the mod_com_amd settings. Names are case insensitive
 * and at most AMD_MAX_OVERRIDES pairs are read (like mod_com_amd).
 */
static void amd_parse_overrides(amd_settings_t *settings, const char *data)
{
	char *dup, *pair, *next;
	int count = 0;

	if (zstr(data)) {
		return;
	}

	if (!(dup = strdup(data))) {
		return;
	}

	for (pair = dup; pair && count < AMD_MAX_OVERRIDES; pair = next, count++) {
		char *eq;
		char *name;
		char *value;

		if ((next = strchr(pair, ','))) {
			*next++ = '\0';
		}

		if (!(eq = strchr(pair, '='))) {
			continue;
		}

		*eq++ = '\0';
		name = amd_trim(pair);
		value = amd_trim(eq);

		if (zstr(name) || zstr(value)) {
			continue;
		}

		if (!amd_settings_set(settings, name, value)) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "AMD: Ignoring unknown voice_start parameter '%s'\n", name);
		}
	}

	free(dup);
}

/*
 * voice_start is the mod_com_amd compatible, non blocking entry point.
 */
SWITCH_STANDARD_APP(voice_start_function)
{
	switch_channel_t *channel;
	amd_bug_t *abug;
	amd_settings_t settings;
	switch_status_t status;

	if (!session) {
		return;
	}

	channel = switch_core_session_get_channel(session);

	if (!switch_channel_ready(channel)) {
		return;
	}

	abug = (amd_bug_t *) switch_channel_get_private(channel, AMD_BUG_PRIVATE);

	if (abug != NULL) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
						  "AMD: Only 1 AMD per channel please!\n");
		return;
	}

	settings = globals.compat;
	amd_parse_overrides(&settings, data);

	if (switch_true(switch_channel_get_variable(channel, "amd_debug"))) {
		settings.debug = SWITCH_TRUE;
	}

	/* Starting detection clears any previous verdict */
	switch_channel_set_variable(channel, "amd_status", NULL);
	switch_channel_set_variable(channel, "amd_result", NULL);
	switch_channel_set_variable(channel, "amd_wait_status", NULL);
	switch_channel_set_variable(channel, "amd_wait_result", NULL);

	status = amd_add_bug(session, channel, &settings, AMD_MODE_COMPAT);

	if (status != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
						  "AMD: Bug Failure!\n");

		switch_channel_set_variable(channel, "amd_status", "undefined");
		switch_channel_set_variable(channel, "amd_result", "media-bug-failed");
		amd_queue_speech_event(session, channel, settings.event_fire, "undefined", "media-bug-failed");

		if (settings.hangup_on_bug_fail) {
			switch_channel_hangup(channel, SWITCH_CAUSE_DESTINATION_OUT_OF_ORDER);
		}
	}
}

/*
 * voice_stop is the mod_com_amd compatible way to abort a detection. If no
 * verdict was reached amd_status / amd_result are left unset, matching
 * mod_com_amd.
 */
SWITCH_STANDARD_APP(voice_stop_function)
{
	switch_channel_t *channel;
	amd_bug_t *abug;

	if (!session) {
		return;
	}

	channel = switch_core_session_get_channel(session);
	abug = (amd_bug_t *) switch_channel_get_private(channel, AMD_BUG_PRIVATE);

	if (abug == NULL) {
		return;
	}

	abug->finished = SWITCH_TRUE;
	switch_channel_set_private(channel, AMD_BUG_PRIVATE, NULL);
	switch_core_media_bug_remove(session, &abug->bug);
}

static switch_status_t amd_wait_read_frame_callback(switch_core_session_t *session,
													switch_frame_t *frame, void *user_data)
{
	amd_bug_t *abug = (amd_bug_t *) user_data;

	if (abug == NULL || abug->finished || !switch_channel_ready(switch_core_session_get_channel(session))) {
		return SWITCH_STATUS_BREAK;
	}

	return SWITCH_STATUS_SUCCESS;
}

/*
 * waitforresult blocks until the detection started by voice_start produces a
 * verdict. Playing a file (silence when none is given) pumps the read leg,
 * which is what drives the media bug; a plain sleep would starve it.
 */
SWITCH_STANDARD_APP(waitforresult_function)
{
	switch_channel_t *channel;
	amd_bug_t *abug;
	const char *status;
	const char *file = NULL;

	if (!session) {
		return;
	}

	channel = switch_core_session_get_channel(session);

	/* mod_com_amd treats the literal word "silence" like no argument */
	if (!zstr(data) && strcasecmp(data, "silence")) {
		file = data;
	}

	status = switch_channel_get_variable(channel, "amd_status");

	if (!zstr(status)) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
						  "AMD RESULT FOUND: %s\n", switch_str_nil(switch_channel_get_variable(channel, "amd_result")));
		return;
	}

	abug = (amd_bug_t *) switch_channel_get_private(channel, AMD_BUG_PRIVATE);

	if (abug == NULL) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING,
						  "%s Channel is not running AMD\n", switch_channel_get_name(channel));
		return;
	}

	if (!switch_channel_test_flag(channel, CF_ANSWERED)) {
		switch_channel_answer(channel);
	}

	while (switch_channel_ready(channel) && !abug->finished) {
		switch_input_args_t args = { 0 };

		args.read_frame_callback = amd_wait_read_frame_callback;
		args.user_data = abug;

		switch_ivr_play_file(session, NULL, file ? file : "silence_stream://1000", &args);

		/*
		 * A finite file that ended before the verdict is played again. The
		 * read_frame_callback stops each playback as soon as the detection
		 * finishes, so this loop only spins for short files.
		 */
	}

	if (!abug->finished) {
		switch_channel_set_variable(channel, "amd_wait_status", "hangup");
		switch_channel_set_variable(channel, "amd_wait_result", "hangup");
	}
}

SWITCH_STANDARD_API(amd_available_api)
{
	stream->write_function(stream, "true\n");
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_STANDARD_API(amd_count_api)
{
	stream->write_function(stream, "%d\n", AMD_UNLIMITED_CHANNELS);
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_STANDARD_API(amd_info_api)
{
	stream->write_function(stream,
						   "AMD license available => %d\n"
						   "AMD license allocated => 0\n",
						   AMD_UNLIMITED_CHANNELS);
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_STANDARD_APP(amd_start_function)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	switch_codec_t raw_codec = { 0 };
	switch_codec_implementation_t read_impl = { 0 };
	switch_frame_t *read_frame;
	switch_status_t status;
	const amd_settings_t *settings = &globals.native;
	uint32_t timeout_ms = settings->total_analysis_time;
	int32_t sample_count_limit = 0;
	switch_bool_t complete = SWITCH_FALSE;
	amd_decision_t decision = AMD_DECISION_NONE;

	amd_vad_t vad = { 0 };

	if (!session) {
		return;
	}

	vad.channel = channel;
	vad.session = session;
	vad.settings = settings;
	vad.mode = AMD_MODE_NATIVE;
	vad.state = VAD_STATE_IN_WORD;
	vad.silence_duration = 0;
	vad.voice_duration = 0;
	vad.frame_ms = 0;
	vad.in_initial_silence = 1;
	vad.in_greeting = 0;
	vad.words = 0;

	switch_core_session_get_read_impl(session, &read_impl);

	if (timeout_ms) {
		sample_count_limit = (read_impl.actual_samples_per_second / 1000) * timeout_ms;
	}

	/*
	 * We are creating a new L16 (raw 16-bit samples) codec for the read end
	 * of our channel.  We'll use this to process the audio coming off of the
	 * channel so that we always know what we are dealing with.
	 */
	status = switch_core_codec_init(
		&raw_codec,
		"L16",
		NULL,
		NULL,
		read_impl.actual_samples_per_second,
		read_impl.microseconds_per_packet / 1000,
		1,
		SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE,
		NULL,
		switch_core_session_get_pool(session));

	if (status != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(
			SWITCH_CHANNEL_SESSION_LOG(session),
			SWITCH_LOG_ERROR,
			"Unable to initialize L16 (raw) codec.\n");
		return;
	}

	switch_core_session_set_read_codec(session, &raw_codec);

	while (switch_channel_ready(channel)) {
		status = switch_core_session_read_frame(session, &read_frame, SWITCH_IO_FLAG_NONE, 0);

		if (!SWITCH_READ_ACCEPTABLE(status)) {
			break;
		}

		if (read_frame->samples == 0) {
			continue;
		}

		vad.frame_ms = 1000 / (read_impl.actual_samples_per_second / read_frame->samples);

		if (sample_count_limit) {
			sample_count_limit -= raw_codec.implementation->samples_per_packet;
			if (sample_count_limit <= 0) {
				if (settings->debug) {
					switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "AMD: Timeout\n");
				}

				amd_publish(&vad, AMD_DECISION_TIMEOUT);
				break;
			}
		}

		switch (classify_frame(read_frame, &read_impl, settings)) {
		case SILENCE:
			decision = amd_handle_silence_frame(&vad);
			break;
		case VOICED:
		default:
			decision = amd_handle_voiced_frame(&vad);
			break;
		}

		if (decision != AMD_DECISION_NONE) {
			amd_publish(&vad, decision);
			complete = SWITCH_TRUE;
		}

		if (complete) {
			break;
		}
	}

	switch_core_session_reset(session, SWITCH_FALSE, SWITCH_TRUE);
	switch_core_codec_destroy(&raw_codec);
}

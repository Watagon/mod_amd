#include <switch.h>

#define AMD_PARAMS (2)
#define AMD_SYNTAX "<uuid> <command>"
#define AMD_BACKGROUND_SYNTAX "[start|stop]"

/* Media bug name and the channel private slot used to keep track of it */
#define AMD_BUG_FUNCTION "amd"
#define AMD_BUG_PRIVATE "_amd_background_"

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_amd_shutdown);
SWITCH_MODULE_LOAD_FUNCTION(mod_amd_load);
SWITCH_MODULE_DEFINITION(mod_amd, mod_amd_load, mod_amd_shutdown, NULL);
SWITCH_STANDARD_APP(amd_start_function);
SWITCH_STANDARD_APP(amd_background_function);

static struct {
	uint32_t initial_silence;
	uint32_t greeting;
	uint32_t after_greeting_silence;
	uint32_t total_analysis_time;
	uint32_t minimum_word_length;
	uint32_t between_words_silence;
	uint32_t maximum_number_of_words;
	uint32_t silence_threshold;
	uint32_t maximum_word_length;
} globals;

static switch_xml_config_item_t instructions[] = {
	SWITCH_CONFIG_ITEM(
		"initial_silence",
		SWITCH_CONFIG_INT,
		CONFIG_RELOADABLE,
		&globals.initial_silence,
		(void *) 2500,
		NULL, NULL, NULL),

	SWITCH_CONFIG_ITEM(
		"greeting",
		SWITCH_CONFIG_INT,
		CONFIG_RELOADABLE,
		&globals.greeting,
		(void *) 1500,
		NULL, NULL, NULL),

	SWITCH_CONFIG_ITEM(
		"after_greeting_silence",
		SWITCH_CONFIG_INT,
		CONFIG_RELOADABLE,
		&globals.after_greeting_silence,
		(void *) 800,
		NULL, NULL, NULL),

	SWITCH_CONFIG_ITEM(
		"total_analysis_time",
		SWITCH_CONFIG_INT,
		CONFIG_RELOADABLE,
		&globals.total_analysis_time,
		(void *) 5000,
		NULL, NULL, NULL),

	SWITCH_CONFIG_ITEM(
		"min_word_length",
		SWITCH_CONFIG_INT,
		CONFIG_RELOADABLE,
		&globals.minimum_word_length,
		(void *) 100,
		NULL, NULL, NULL),

	SWITCH_CONFIG_ITEM(
		"between_words_silence",
		SWITCH_CONFIG_INT,
		CONFIG_RELOADABLE,
		&globals.between_words_silence,
		(void *) 50,
		NULL, NULL, NULL),

	SWITCH_CONFIG_ITEM(
		"maximum_number_of_words",
		SWITCH_CONFIG_INT,
		CONFIG_RELOADABLE,
		&globals.maximum_number_of_words,
		(void *) 3,
		NULL, NULL, NULL),

	SWITCH_CONFIG_ITEM(
		"maximum_word_length",
		SWITCH_CONFIG_INT,
		CONFIG_RELOADABLE,
		&globals.maximum_word_length,
		(void *)5000,
		NULL, NULL, NULL),

	SWITCH_CONFIG_ITEM(
		"silence_threshold",
		SWITCH_CONFIG_INT,
		CONFIG_RELOADABLE,
		&globals.silence_threshold,
		(void *) 256,
		NULL, NULL, NULL),

	SWITCH_CONFIG_ITEM_END()
};

/*
 * The defaults of the config items are only applied for the parameters that
 * amd.conf actually sets, switch_xml_config_parse_module_settings() returns
 * FAILURE without applying anything at all when the file is missing. Without
 * this every setting would be 0, which classifies every single frame as voiced
 * and reports a machine on the very first frame.
 */
static void set_default_config(void)
{
	memset(&globals, 0, sizeof(globals));

	globals.initial_silence = 2500;
	globals.greeting = 1500;
	globals.after_greeting_silence = 800;
	globals.total_analysis_time = 5000;
	globals.minimum_word_length = 100;
	globals.between_words_silence = 50;
	globals.maximum_number_of_words = 3;
	globals.maximum_word_length = 5000;
	globals.silence_threshold = 256;
}

static switch_status_t do_config(switch_bool_t reload)
{
	set_default_config();

	if (switch_xml_config_parse_module_settings("amd.conf", reload, instructions) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_FALSE;
	}

	return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_LOAD_FUNCTION(mod_amd_load)
{
	switch_application_interface_t *app_interface;

	*module_interface = switch_loadable_module_create_module_interface(pool, modname);

	do_config(SWITCH_FALSE);

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

	return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_amd_shutdown)
{
	switch_xml_config_cleanup(instructions);

	return SWITCH_STATUS_SUCCESS;
}

typedef enum {
	SILENCE,
	VOICED
} amd_frame_classifier;

typedef enum {
	VAD_STATE_IN_WORD,
	VAD_STATE_IN_SILENCE,
} amd_vad_state_t;

typedef struct {
	switch_core_session_t *session;
	switch_channel_t *channel;
	amd_vad_state_t state;
	uint32_t frame_ms;

	uint32_t silence_duration;
	uint32_t voice_duration;
	uint32_t words;

	uint32_t in_initial_silence:1;
	uint32_t in_greeting:1;
} amd_vad_t;

static void amd_fire_event(amd_vad_t *vad)
{
	switch_event_t *event;

	if (switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, "amd::result") != SWITCH_STATUS_SUCCESS) {
		return;
	}

	switch_channel_event_set_data(vad->channel, event);

	switch_event_add_header_string(
		event, SWITCH_STACK_BOTTOM, "AMD-Result",
		switch_channel_get_variable(vad->channel, "amd_result"));

	switch_event_add_header_string(
		event, SWITCH_STACK_BOTTOM, "AMD-Cause",
		switch_channel_get_variable(vad->channel, "amd_cause"));

	switch_event_fire(&event);
}

static amd_frame_classifier classify_frame(const switch_frame_t *f, const switch_codec_implementation_t *codec)
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

	if (score >= globals.silence_threshold) {
		return VOICED;
	}

	return SILENCE;
}

static switch_bool_t amd_handle_silence_frame(amd_vad_t *vad, const switch_frame_t *f)
{
	vad->silence_duration += vad->frame_ms;

	if (vad->silence_duration >= globals.between_words_silence) {
		if (vad->state != VAD_STATE_IN_SILENCE) {
			switch_log_printf(
				SWITCH_CHANNEL_SESSION_LOG(vad->session),
				SWITCH_LOG_DEBUG,
				"AMD: Changed state to VAD_STATE_IN_SILENCE\n");
		}

		vad->state = VAD_STATE_IN_SILENCE;
		vad->voice_duration = 0;
	}

	if (vad->in_initial_silence && vad->silence_duration >= globals.initial_silence) {
		switch_log_printf(
			SWITCH_CHANNEL_SESSION_LOG(vad->session),
			SWITCH_LOG_DEBUG,
			"AMD: MACHINE (silence_duration: %d, initial_silence: %d)\n",
			vad->silence_duration,
			globals.initial_silence);

		switch_channel_set_variable(vad->channel, "amd_result", "MACHINE");
		switch_channel_set_variable(vad->channel, "amd_cause", "INITIALSILENCE");
		return SWITCH_TRUE;
	}

	if (vad->silence_duration >= globals.after_greeting_silence && vad->in_greeting) {
		switch_log_printf(
			SWITCH_CHANNEL_SESSION_LOG(vad->session),
			SWITCH_LOG_DEBUG,
			"AMD: HUMAN (silence_duration: %d, after_greeting_silence: %d)\n",
			vad->silence_duration,
			globals.after_greeting_silence);

		switch_channel_set_variable(vad->channel, "amd_result", "HUMAN");
		switch_channel_set_variable(vad->channel, "amd_cause", "HUMAN");
		return SWITCH_TRUE;
	}

	return SWITCH_FALSE;
}

static switch_bool_t amd_handle_voiced_frame(amd_vad_t *vad, const switch_frame_t *f)
{
	vad->voice_duration += vad->frame_ms;

	if (vad->voice_duration >= globals.minimum_word_length && vad->state == VAD_STATE_IN_SILENCE) {
		vad->words++;

		switch_log_printf(
			SWITCH_CHANNEL_SESSION_LOG(vad->session),
			SWITCH_LOG_DEBUG,
			"AMD: Word detected (words: %d)\n",
			vad->words);

		vad->state = VAD_STATE_IN_WORD;
	}

	if (vad->voice_duration >= globals.maximum_word_length) {
		switch_log_printf(
			SWITCH_CHANNEL_SESSION_LOG(vad->session),
			SWITCH_LOG_DEBUG,
			"AMD: MACHINE (voice_duration: %d, maximum_word_length: %d)\n",
			vad->voice_duration,
			globals.maximum_word_length);

		switch_channel_set_variable(vad->channel, "amd_result", "MACHINE");
		switch_channel_set_variable(vad->channel, "amd_cause", "MAXWORDLENGTH");
		return SWITCH_TRUE;
	}

	if (vad->words >= globals.maximum_number_of_words) {
		switch_log_printf(
			SWITCH_CHANNEL_SESSION_LOG(vad->session),
			SWITCH_LOG_DEBUG,
			"AMD: MACHINE (words: %d, maximum_number_of_words: %d)\n",
			vad->words,
			globals.maximum_number_of_words);

		switch_channel_set_variable(vad->channel, "amd_result", "MACHINE");
		switch_channel_set_variable(vad->channel, "amd_cause", "MAXWORDS");
		return SWITCH_TRUE;
	}

	if (vad->in_greeting && vad->voice_duration >= globals.greeting) {
		switch_log_printf(
			SWITCH_CHANNEL_SESSION_LOG(vad->session),
			SWITCH_LOG_DEBUG,
			"AMD: MACHINE (voice_duration: %d, greeting: %d)\n",
			vad->voice_duration,
			globals.greeting);

		switch_channel_set_variable(vad->channel, "amd_result", "MACHINE");
		switch_channel_set_variable(vad->channel, "amd_cause", "LONGGREETING");
		return SWITCH_TRUE;
	}

	if (vad->voice_duration >= globals.minimum_word_length) {
		if (vad->silence_duration) {
			switch_log_printf(
				SWITCH_CHANNEL_SESSION_LOG(vad->session),
				SWITCH_LOG_DEBUG,
				"AMD: Detected Talk, previous silence duration: %dms\n",
				vad->silence_duration);
		}

		vad->silence_duration = 0;
	}

	if (vad->voice_duration >= globals.minimum_word_length && !vad->in_greeting) {
		if (vad->silence_duration) {
			switch_log_printf(
				SWITCH_CHANNEL_SESSION_LOG(vad->session),
				SWITCH_LOG_DEBUG,
				"AMD: Before Greeting Time (silence_duration: %d, voice_duration: %d)\n",
				vad->silence_duration,
				vad->voice_duration);
		}

		vad->in_initial_silence = 0;
		vad->in_greeting = 1;
	}

	return SWITCH_FALSE;
}

/*
 * Background (non blocking) detection.
 *
 * The blocking "amd" application consumes the read leg of the channel until a
 * verdict is reached, which means the IVR can not play a greeting or wait for
 * input at the same time. The background variant attaches a media bug instead:
 * the application returns as soon as the bug is in place and the audio keeps
 * flowing through the bug while the channel is being used for something else
 * (playback pumps the read leg once per frame written).
 */
typedef struct {
	amd_vad_t vad;
	switch_media_bug_t *bug;
	switch_time_t start_time;
	switch_bool_t finished;
} amd_bug_t;

static void amd_finish(amd_bug_t *abug)
{
	abug->finished = SWITCH_TRUE;
	amd_fire_event(&abug->vad);
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
		switch_log_printf(
			SWITCH_CHANNEL_SESSION_LOG(abug->vad.session),
			SWITCH_LOG_DEBUG,
			"AMD: Background detection initialized\n");
		break;

	case SWITCH_ABC_TYPE_CLOSE:
		if (switch_channel_get_private(abug->vad.channel, AMD_BUG_PRIVATE) == abug) {
			switch_channel_set_private(abug->vad.channel, AMD_BUG_PRIVATE, NULL);
		}

		/* Reached when the bug is pruned before any verdict was made */
		if (!abug->finished && switch_channel_ready(abug->vad.channel)) {
			switch_log_printf(
				SWITCH_CHANNEL_SESSION_LOG(abug->vad.session),
				SWITCH_LOG_DEBUG,
				"AMD: Timeout (elapsed: %u ms)\n",
				(uint32_t) ((switch_micro_time_now() - abug->start_time) / 1000));

			switch_channel_set_variable(abug->vad.channel, "amd_result", "NOTSURE");
			switch_channel_set_variable(abug->vad.channel, "amd_cause", "TOOLONG");
			amd_finish(abug);
		}

		return SWITCH_TRUE;

	case SWITCH_ABC_TYPE_READ_REPLACE:
	{
		switch_frame_t *read_frame = switch_core_media_bug_get_read_replace_frame(bug);
		switch_codec_implementation_t read_impl = { 0 };
		uint32_t elapsed_ms;

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

		if (globals.total_analysis_time && elapsed_ms >= globals.total_analysis_time) {
			switch_log_printf(
				SWITCH_CHANNEL_SESSION_LOG(abug->vad.session),
				SWITCH_LOG_DEBUG,
				"AMD: Timeout (elapsed: %u ms)\n",
				elapsed_ms);

			switch_channel_set_variable(abug->vad.channel, "amd_result", "NOTSURE");
			switch_channel_set_variable(abug->vad.channel, "amd_cause", "TOOLONG");
			amd_finish(abug);
			rval = SWITCH_FALSE;
			break;
		}

		switch (classify_frame(read_frame, &read_impl)) {
		case SILENCE:
			switch_log_printf(
				SWITCH_CHANNEL_SESSION_LOG(abug->vad.session),
				SWITCH_LOG_DEBUG,
				"AMD: Silence\n");

			if (amd_handle_silence_frame(&abug->vad, read_frame)) {
				amd_finish(abug);
				rval = SWITCH_FALSE;
			}
			break;

		case VOICED:
		default:
			switch_log_printf(
				SWITCH_CHANNEL_SESSION_LOG(abug->vad.session),
				SWITCH_LOG_DEBUG,
				"AMD: Voiced\n");

			if (amd_handle_voiced_frame(&abug->vad, read_frame)) {
				amd_finish(abug);
				rval = SWITCH_FALSE;
			}
			break;
		}

		break;
	}

	default:
		break;
	}

	return rval;
}

SWITCH_STANDARD_APP(amd_background_function)
{
	switch_channel_t *channel;
	amd_bug_t *abug = NULL;
	switch_status_t status;
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

	/* The detection lives as long as the session, no extra pool needed */
	abug = (amd_bug_t *) switch_core_session_alloc(session, sizeof(*abug));

	if (abug == NULL) {
		switch_log_printf(
			SWITCH_CHANNEL_SESSION_LOG(session),
			SWITCH_LOG_ERROR,
			"AMD: Unable to allocate memory\n");
		return;
	}

	memset(abug, 0, sizeof(*abug));

	abug->vad.session = session;
	abug->vad.channel = channel;
	abug->vad.state = VAD_STATE_IN_WORD;
	abug->vad.in_initial_silence = 1;
	abug->start_time = switch_micro_time_now();

	status = switch_core_media_bug_add(
		session,
		AMD_BUG_FUNCTION,
		NULL,
		amd_bug_callback,
		abug,
		/* stop_time of a media bug is an absolute epoch time in seconds */
		switch_epoch_time_now(NULL) + (globals.total_analysis_time / 1000),
		SMBF_READ_REPLACE | SMBF_ONE_ONLY,
		&abug->bug);

	if (status != SWITCH_STATUS_SUCCESS || abug->bug == NULL) {
		switch_log_printf(
			SWITCH_CHANNEL_SESSION_LOG(session),
			SWITCH_LOG_ERROR,
			"AMD: Unable to add media bug (%d)\n",
			status);
		return;
	}

	switch_channel_set_private(channel, AMD_BUG_PRIVATE, abug);

	switch_log_printf(
		SWITCH_CHANNEL_SESSION_LOG(session),
		SWITCH_LOG_DEBUG,
		"AMD: Background detection started\n");
}

SWITCH_STANDARD_APP(amd_start_function)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	switch_codec_t raw_codec = { 0 };
	switch_codec_implementation_t read_impl = { 0 };
	switch_frame_t *read_frame;
	switch_status_t status;
	uint32_t timeout_ms = globals.total_analysis_time;
	int32_t sample_count_limit;
	switch_bool_t complete = SWITCH_FALSE;

	amd_vad_t vad = { 0 };

	if (!session) {
		return;
	}

	vad.channel = channel;
	vad.session = session;
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
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "AMD: Timeout\n");

				switch_channel_set_variable(channel, "amd_result", "NOTSURE");
				switch_channel_set_variable(channel, "amd_cause", "TOOLONG");
				amd_fire_event(&vad);
				break;
			}
		}

		switch (classify_frame(read_frame, &read_impl)) {
		case SILENCE:
			switch_log_printf(
				SWITCH_CHANNEL_SESSION_LOG(session),
				SWITCH_LOG_DEBUG,
				"AMD: Silence\n");

			if (amd_handle_silence_frame(&vad, read_frame)) {
				complete = SWITCH_TRUE;
			}
			break;
		case VOICED:
		default:
			switch_log_printf(
				SWITCH_CHANNEL_SESSION_LOG(session),
				SWITCH_LOG_DEBUG,
				"AMD: Voiced\n");

			if (amd_handle_voiced_frame(&vad, read_frame)) {
				complete = SWITCH_TRUE;
			}
			break;
		}

		if (complete) {
			amd_fire_event(&vad);
			break;
		}
	}

	switch_core_session_reset(session, SWITCH_FALSE, SWITCH_TRUE);
	switch_core_codec_destroy(&raw_codec);
}

/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 2009 - 2026, Lefteris Zafiris
 *
 * Lefteris Zafiris <zaf@fastmail.com>
 *
 * See http://www.asterisk.org for more information about
 * the Asterisk project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the COPYING file
 * at the top of the source tree.
 */

/*! \file
 *
 * \brief Say text to the user, using Flite TTS engine.
 *
 * \author\verbatim Lefteris Zafiris <zaf@fastmail.com> \endverbatim
 *
 * \extref Flite text to speech Synthesis System - http://www.speech.cs.cmu.edu/flite/
 *
 * \ingroup applications
 */

/*** MODULEINFO
	<defaultenabled>no</defaultenabled>
 ***/

#include "asterisk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <flite/flite.h>
#include "asterisk/app.h"
#include "asterisk/channel.h"
#include "asterisk/module.h"
#include "asterisk/config.h"
#include "asterisk/utils.h"
#include "asterisk/lock.h"

#define AST_MODULE "Flite"
#define FLITE_CONFIG "flite.conf"
#define MAXLEN 2048
#define MAXTEXT 32768
#define DEF_MAXTEXT 4096
#define DEF_RATE 8000
#define DEF_VOICE "kal"
#define DEF_DIR "/var/lib/asterisk/flitecache"

/*** DOCUMENTATION
	<application name="Flite" language="en_US">
		<synopsis>
			Say text to the user, using flite speech synthesizer.
		</synopsis>
		<syntax>
			<parameter name="text" required="true" />
			<parameter name="intkeys" />
		</syntax>
		<description>
			<para>Flite(text[,intkeys]):  This will invoke the flite TTS engine,
			send a text string, get back the resulting waveform and play it to the user,
			allowing any given interrupt keys to immediately terminate and return.</para>
		</description>
	</application>
 ***/

cst_voice *register_cmu_us_awb(const char *voxdir);
void unregister_cmu_us_awb(cst_voice *v);

cst_voice *register_cmu_us_kal(const char *voxdir);
void unregister_cmu_us_kal(cst_voice *v);

cst_voice *register_cmu_us_kal16(const char *voxdir);
void unregister_cmu_us_kal16(cst_voice *v);

cst_voice *register_cmu_us_rms(const char *voxdir);
void unregister_cmu_us_rms(cst_voice *v);

cst_voice *register_cmu_us_slt(const char *voxdir);
void unregister_cmu_us_slt(cst_voice *v);

/* Voices are registered once at load and shared read-only between
 * channel threads; flite synthesis itself is thread safe. */
static cst_voice *v_kal, *v_kal16, *v_awb, *v_rms, *v_slt;

static const char *app = "Flite";

static int target_sample_rate;
static int usecache;
static int maxtext;
static char cachedir[MAXLEN];
static char voice_name[16];

/* Protects the config values above. */
AST_RWLOCK_DEFINE_STATIC(cfg_lock);

static void unregister_voices(void)
{
	if (v_kal)
		unregister_cmu_us_kal(v_kal);
	if (v_kal16)
		unregister_cmu_us_kal16(v_kal16);
	if (v_awb)
		unregister_cmu_us_awb(v_awb);
	if (v_rms)
		unregister_cmu_us_rms(v_rms);
	if (v_slt)
		unregister_cmu_us_slt(v_slt);
	v_kal = v_kal16 = v_awb = v_rms = v_slt = NULL;
}

static int parse_int(const char *val, int def, int min, int max, const char *name)
{
	char *end;
	long n;

	errno = 0;
	n = strtol(val, &end, 10);
	if (errno || end == val || *end || n < min || n > max) {
		ast_log(LOG_WARNING,
				"Flite: Invalid value '%s' for %s (valid: %d-%d), using default %d\n",
				val, name, min, max, def);
		return def;
	}
	return (int) n;
}

static int read_config(const char *flite_conf)
{
	const char *temp;
	struct ast_config *cfg;
	struct ast_flags config_flags = { 0 };

	cfg = ast_config_load(flite_conf, config_flags);
	if (!cfg || cfg == CONFIG_STATUS_FILEINVALID) {
		ast_log(LOG_WARNING,
				"Flite: Unable to read config file %s. Using default settings\n", flite_conf);
		cfg = NULL;
	}

	ast_rwlock_wrlock(&cfg_lock);
	/* set default values */
	target_sample_rate = DEF_RATE;
	usecache = 0;
	maxtext = DEF_MAXTEXT;
	ast_copy_string(cachedir, DEF_DIR, sizeof(cachedir));
	ast_copy_string(voice_name, DEF_VOICE, sizeof(voice_name));

	if (cfg) {
		if ((temp = ast_variable_retrieve(cfg, "general", "usecache")))
			usecache = ast_true(temp);

		temp = ast_variable_retrieve(cfg, "general", "cachedir");
		if (!ast_strlen_zero(temp))
			ast_copy_string(cachedir, temp, sizeof(cachedir));

		temp = ast_variable_retrieve(cfg, "general", "voice");
		if (!ast_strlen_zero(temp))
			ast_copy_string(voice_name, temp, sizeof(voice_name));

		if ((temp = ast_variable_retrieve(cfg, "general", "samplerate")))
			target_sample_rate = parse_int(temp, DEF_RATE, 8000, 16000, "samplerate");
		if ((temp = ast_variable_retrieve(cfg, "general", "maxtext")))
			maxtext = parse_int(temp, DEF_MAXTEXT, 1, MAXTEXT, "maxtext");
	}

	if (target_sample_rate != 8000 && target_sample_rate != 16000) {
		ast_log(LOG_WARNING, "Flite: Unsupported sample rate: %d. Falling back to %d\n",
				target_sample_rate, DEF_RATE);
		target_sample_rate = DEF_RATE;
	}
	if (usecache) {
		struct stat st;
		if (stat(cachedir, &st) && (ast_mkdir(cachedir, 0700) || stat(cachedir, &st))) {
			ast_log(LOG_ERROR,
					"Flite: Failed to create cache directory %s, caching disabled\n", cachedir);
			usecache = 0;
		} else if (!S_ISDIR(st.st_mode) || st.st_uid != geteuid()
				|| (st.st_mode & (S_IWGRP | S_IWOTH))) {
			ast_log(LOG_ERROR,
					"Flite: Cache directory %s must be owned by the Asterisk user "
					"and not group/world writable, caching disabled\n", cachedir);
			usecache = 0;
		}
	}
	ast_rwlock_unlock(&cfg_lock);
	if (cfg)
		ast_config_destroy(cfg);
	return 0;
}

static int flite_exec(struct ast_channel *chan, const char *data)
{
	int res = 0;
	int raw_fd;
	int writecache = 0;
	FILE *fl;
	char *mydata, *format;
	char cachefile[MAXLEN];
	char tmp_name[MAXLEN + 16];
	char raw_tmp_name[MAXLEN + 24];
	int use_cache, t_rate, l_maxtext;
	char l_cachedir[MAXLEN];
	char l_voice[16];
	cst_wave *raw_data;
	cst_voice *voice;
	AST_DECLARE_APP_ARGS(args,
		AST_APP_ARG(text);
		AST_APP_ARG(interrupt);
	);

	if (ast_strlen_zero(data)) {
		ast_log(LOG_ERROR, "Flite requires an argument (text)\n");
		return -1;
	}

	ast_rwlock_rdlock(&cfg_lock);
	use_cache = usecache;
	t_rate = target_sample_rate;
	l_maxtext = maxtext;
	ast_copy_string(l_cachedir, cachedir, sizeof(l_cachedir));
	ast_copy_string(l_voice, voice_name, sizeof(l_voice));
	ast_rwlock_unlock(&cfg_lock);

	/* Check before ast_strdupa() copies the data onto the stack. */
	if (strlen(data) > (size_t) l_maxtext) {
		ast_log(LOG_WARNING, "Flite: Text too long (max %d bytes).\n", l_maxtext);
		return -1;
	}
	mydata = ast_strdupa(data);
	AST_STANDARD_APP_ARGS(args, mydata);

	if (args.interrupt && !strcasecmp(args.interrupt, "any"))
		args.interrupt = AST_DIGIT_ANY;

	args.text = ast_strip_quoted(args.text, "\"", "\"");
	if (ast_strlen_zero(args.text)) {
		ast_log(LOG_WARNING, "Flite: No text passed for synthesis.\n");
		return res;
	}

	ast_debug(1, "Flite:\nText passed: %s\nInterrupt key(s): %s\nVoice: %s\nRate: %d\n",
			args.text, S_OR(args.interrupt, "none"), l_voice, t_rate);

	if (t_rate == 16000)
		format = "sln16";
	else
		format = "sln";

	/*Cache mechanism */
	if (use_cache) {
		char text_hash[41], key[128], hash[41];
		struct stat st;

		ast_sha1_hash(text_hash, args.text);
		snprintf(key, sizeof(key), "%s|%s|%d", text_hash, l_voice, t_rate);
		if (strlen(l_cachedir) + sizeof(hash) + 6 <= MAXLEN) {
			char cachepath[MAXLEN + 8];

			ast_sha1_hash(hash, key);
			snprintf(cachefile, sizeof(cachefile), "%s/%s", l_cachedir, hash);
			snprintf(cachepath, sizeof(cachepath), "%s.%s", cachefile, format);
			writecache = 1;
			if (lstat(cachepath, &st) == 0 && S_ISREG(st.st_mode)
					&& st.st_uid == geteuid() && st.st_size > 0) {
				ast_debug(1, "Flite: Serving from cache file %s\n", cachepath);
				if (ast_channel_state(chan) != AST_STATE_UP)
					ast_answer(chan);
				res = ast_streamfile(chan, cachefile, ast_channel_language(chan));
				if (!res) {
					res = ast_waitstream(chan, args.interrupt);
					ast_stopstream(chan);
					return res;
				}
				ast_log(LOG_WARNING, "Flite: Bad cache entry %s, regenerating\n", cachepath);
			}
		}
	}

	/* Create the temp file in the cache dir when a cache write is pending,
	 * so the final rename is atomic and on the same filesystem. */
	if (writecache)
		snprintf(tmp_name, sizeof(tmp_name), "%s/flite_XXXXXX", l_cachedir);
	else
		ast_copy_string(tmp_name, "/tmp/flite_XXXXXX", sizeof(tmp_name));
	if ((raw_fd = mkstemp(tmp_name)) == -1 && writecache) {
		ast_copy_string(tmp_name, "/tmp/flite_XXXXXX", sizeof(tmp_name));
		raw_fd = mkstemp(tmp_name);
	}
	if (raw_fd == -1) {
		ast_log(LOG_ERROR, "Flite: Failed to create audio file.\n");
		return -1;
	}
	if ((fl = fdopen(raw_fd, "w+")) == NULL) {
		ast_log(LOG_ERROR, "Flite: Failed to open audio file '%s'\n", tmp_name);
		close(raw_fd);
		unlink(tmp_name);
		return -1;
	}

	/* Invoke Flite */
	if (strcmp(l_voice, "kal") == 0)
		voice = v_kal;
	else if (strcmp(l_voice, "kal16") == 0)
		voice = v_kal16;
	else if (strcmp(l_voice, "awb") == 0)
		voice = v_awb;
	else if (strcmp(l_voice, "rms") == 0)
		voice = v_rms;
	else if (strcmp(l_voice, "slt") == 0)
		voice = v_slt;
	else {
		ast_log(LOG_WARNING, "Flite: Unsupported voice %s. Using default male voice.\n",
				l_voice);
		voice = v_kal;
	}

	raw_data = flite_text_to_wave(args.text, voice);
	if (raw_data) {
		/* Resample if needed */
		if (raw_data->sample_rate != t_rate)
			cst_wave_resample(raw_data, t_rate);
		res = cst_wave_save_raw_fd(raw_data, fl);
		delete_wave(raw_data);
	} else {
		res = -1;
	}
	if (fclose(fl))
		res = -1;

	if (res) {
		ast_log(LOG_ERROR, "Flite: Failed to synthesize or write audio file %s\n", tmp_name);
		unlink(tmp_name);
		return -1;
	}

	snprintf(raw_tmp_name, sizeof(raw_tmp_name), "%s.%s", tmp_name, format);
	if (rename(tmp_name, raw_tmp_name)) {
		char ebuf[128];
		ast_log(LOG_ERROR, "Flite: Failed to rename audio file: %s\n",
				strerror_r(errno, ebuf, sizeof(ebuf)));
		unlink(tmp_name);
		return -1;
	}

	if (ast_channel_state(chan) != AST_STATE_UP)
		ast_answer(chan);
	res = ast_streamfile(chan, tmp_name, ast_channel_language(chan));
	if (res) {
		ast_log(LOG_ERROR, "Flite: ast_streamfile failed on %s\n", ast_channel_name(chan));
	} else {
		res = ast_waitstream(chan, args.interrupt);
		ast_stopstream(chan);
	}

	/* Save file to cache if set */
	if (writecache) {
		int cfd;
		ast_debug(1, "Flite: Saving cache file %s\n", cachefile);
		if ((cfd = open(raw_tmp_name, O_RDONLY)) != -1) {
			fsync(cfd);
			close(cfd);
		}
		if (ast_filerename(tmp_name, cachefile, format)) {
			ast_log(LOG_WARNING, "Flite: Failed to save cache file %s\n", cachefile);
			unlink(raw_tmp_name);
		}
	} else {
		unlink(raw_tmp_name);
	}
	return res;
}

static int reload_module(void)
{
	return read_config(FLITE_CONFIG);
}

static int unload_module(void)
{
	int res = ast_unregister_application(app);

	unregister_voices();
	return res;
}

static int load_module(void)
{
	read_config(FLITE_CONFIG);
	flite_init();
	v_kal = register_cmu_us_kal(NULL);
	v_kal16 = register_cmu_us_kal16(NULL);
	v_awb = register_cmu_us_awb(NULL);
	v_rms = register_cmu_us_rms(NULL);
	v_slt = register_cmu_us_slt(NULL);
	if (!v_kal || !v_kal16 || !v_awb || !v_rms || !v_slt) {
		ast_log(LOG_ERROR, "Flite: Failed to register voices.\n");
		unregister_voices();
		return AST_MODULE_LOAD_DECLINE;
	}
	if (ast_register_application_xml(app, flite_exec)) {
		unregister_voices();
		return AST_MODULE_LOAD_DECLINE;
	}
	return AST_MODULE_LOAD_SUCCESS;
}

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_DEFAULT, "Flite TTS Interface",
	.load = load_module,
	.unload = unload_module,
	.reload = reload_module,
);

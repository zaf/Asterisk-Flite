/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 2009 - 2011, Lefteris Zafiris
 *
 * Lefteris Zafiris <zaf.000@gmail.com>
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
 * \author\verbatim Lefteris Zafiris <zaf.000@gmail.com> \endverbatim
 *
 * \extref Flite text to speech Synthesis System - http://www.speech.cs.cmu.edu/flite/
 *
 * \ingroup applications
 */

/*** MODULEINFO
	<defaultenabled>no</defaultenabled>
 ***/

#include "asterisk.h"

ASTERISK_FILE_VERSION(__FILE__, "$Revision: 00 $")
#include <stdio.h>
#include <string.h>
#include <flite/flite.h>
#include "asterisk/file.h"
#include "asterisk/channel.h"
#include "asterisk/module.h"
#include "asterisk/config.h"
#include "asterisk/app.h"
#include "asterisk/utils.h"
#include "asterisk/strings.h"
#define AST_MODULE "Flite"
#define FLITE_CONFIG "flite.conf"
#define MAXLEN 2048

cst_voice *register_cmu_us_awb(void);
cst_voice *register_cmu_us_kal16(void);
cst_voice *register_cmu_us_rms(void);
cst_voice *register_cmu_us_slt(void);

static char *app = "Flite";
static char *synopsis = "Say text to the user, using Flite TTS engine";
static char *descrip =
	" Flite(text[,intkeys]): This will invoke the Flite TTS engine, send a text string,\n"
	"get back the resulting waveform and play it to the user, allowing any given interrupt\n"
	"keys to immediately terminate and return the value, or 'any' to allow any number back.\n";

static int app_exec(struct ast_channel *chan, const char *data)
{
	int res = 0;
	char *mydata;
	const char *cachedir;
	const char *temp;
	const char *voice_name;
	int usecache = 0;
	int writecache = 0;
	char MD5_name[33] = "";
	int sample_rate;
	int target_sample_rate;
	char cachefile[MAXLEN] = "";
	char tmp_name[20];
	char raw_tmp_name[26];
	cst_wave *raw_data;
	cst_voice *voice;
	struct ast_config *cfg;
	struct ast_flags config_flags = { 0 };
	AST_DECLARE_APP_ARGS(args,
		AST_APP_ARG(text);
		AST_APP_ARG(interrupt);
	);

	if (ast_strlen_zero(data)) {
		ast_log(LOG_ERROR, "Flite requires an argument (text)\n");
		return -1;
	}

	cfg = ast_config_load(FLITE_CONFIG, config_flags);
	if (!cfg) {
		ast_log(LOG_WARNING, "Flite: No such configuration file %s\n", FLITE_CONFIG);
		cachedir = "/tmp";
		voice_name = "kal";
		target_sample_rate = 16000;
	} else {
		if ((temp = ast_variable_retrieve(cfg, "general", "usecache")))
			usecache = ast_true(temp);
		if (!(cachedir = ast_variable_retrieve(cfg, "general", "cachedir")))
			cachedir = "/tmp";
		if (!(voice_name = ast_variable_retrieve(cfg, "general", "voice")))
			voice_name = "kal";
		if (!(temp = ast_variable_retrieve(cfg, "general", "samplerate"))) {
			target_sample_rate = 16000;
		} else {
			target_sample_rate = atoi(temp);
		}
	}

	mydata = ast_strdupa(data);
	AST_STANDARD_APP_ARGS(args, mydata);

	if (args.interrupt && !strcasecmp(args.interrupt, "any"))
		args.interrupt = AST_DIGIT_ANY;
	
	args.text = ast_strip_quoted(args.text, "\"", "\"");
	if (ast_strlen_zero(args.text)) {
		ast_log(LOG_WARNING, "Flite: No text passed for synthesis.\n");
		ast_config_destroy(cfg);
		return res;
	}
	
	if (target_sample_rate != 8000 && target_sample_rate != 16000) {
		ast_log(LOG_WARNING, "Flite: Unsupported sample rate: %d. Falling back to 16000Hz\n", target_sample_rate);
		target_sample_rate = 16000;
	}

	ast_debug(1, "Flite:\nText passed: %s\nInterrupt key(s): %s\nVoice: %s\n", args.text,
		args.interrupt, voice_name);

	/*Cache mechanism */
	if (usecache) {
		ast_md5_hash(MD5_name, args.text);
		if (strlen(cachedir) + strlen(MD5_name) + 6 <= MAXLEN) {
			ast_debug(1, "Flite: Activating cache mechanism...\n");
			snprintf(cachefile, sizeof(cachefile), "%s/%s", cachedir, MD5_name);
			if (ast_fileexists(cachefile, NULL, NULL) <= 0) {
				ast_debug(1, "Flite: Cache file does not yet exist.\n");
				writecache = 1;
			} else {
				ast_debug(1, "Flite: Cache file exists.\n");
				if (chan->_state != AST_STATE_UP)
					ast_answer(chan);
				res = ast_streamfile(chan, cachefile, chan->language);
				if (res) {
					ast_log(LOG_ERROR, "Flite: ast_streamfile from cache failed on %s\n",
						chan->name);
				} else {
					res = ast_waitstream(chan, args.interrupt);
					ast_stopstream(chan);
					ast_config_destroy(cfg);
					return res;
				}
			}
		}
	}

	/* Create temp filenames */
	snprintf(tmp_name, sizeof(tmp_name), "/tmp/flite_%li", ast_random() %99999999);
	if (target_sample_rate == 16000)
		snprintf(raw_tmp_name, sizeof(raw_tmp_name), "%s.sln16", tmp_name);
	if (target_sample_rate == 8000)
		snprintf(raw_tmp_name, sizeof(raw_tmp_name), "%s.sln", tmp_name);

	/* Invoke Flite */
	flite_init();
	if (strcmp(voice_name,"kal") == 0)
		voice = register_cmu_us_kal16();
	else if (strcmp(voice_name,"awb") == 0)
		voice = register_cmu_us_awb();
	else if (strcmp(voice_name,"rms") == 0)
		voice = register_cmu_us_rms();
	else if (strcmp(voice_name,"slt") == 0)
		voice = register_cmu_us_slt();
	else
		voice = register_cmu_us_kal16();
	
	raw_data = flite_text_to_wave(args.text, voice);
	sample_rate = raw_data->sample_rate;

	/* Resample if needed */
	if (sample_rate != target_sample_rate)
		cst_wave_resample(raw_data, target_sample_rate);

	res = cst_wave_save_raw(raw_data, raw_tmp_name);
	delete_wave(raw_data);
	if (res) {
		ast_log(LOG_ERROR, "Flite: failed to write file %s\n", raw_tmp_name);
		ast_config_destroy(cfg);
		return res;
	}
	
	/* Save file to cache if set */
	if (writecache) {
		ast_debug(1, "Flite: Saving cache file %s\n", cachefile);
		ast_filecopy(tmp_name, cachefile, NULL);
	}

	if (chan->_state != AST_STATE_UP)
		ast_answer(chan);
	res = ast_streamfile(chan, tmp_name, chan->language);
	if (res) {
		ast_log(LOG_ERROR, "Flite: ast_streamfile failed on %s\n", chan->name);
	} else {
		res = ast_waitstream(chan, args.interrupt);
		ast_stopstream(chan);
	}

	ast_filedelete(tmp_name, NULL);
	ast_config_destroy(cfg);
	return res;
}

static int unload_module(void)
{
	return ast_unregister_application(app);
}

static int load_module(void)
{
	return ast_register_application(app, app_exec, synopsis, descrip) ?
		AST_MODULE_LOAD_DECLINE : AST_MODULE_LOAD_SUCCESS;
}

AST_MODULE_INFO_STANDARD(ASTERISK_GPL_KEY, "Flite TTS Interface");

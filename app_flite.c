/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 2009 - 2015, Lefteris Zafiris
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

ASTERISK_REGISTER_FILE()
#include <stdio.h>
#include <string.h>
#include <flite/flite.h>
#include "asterisk/app.h"
#include "asterisk/channel.h"
#include "asterisk/module.h"
#include "asterisk/config.h"
#include "asterisk/utils.h"

#define AST_MODULE "Flite"
#define FLITE_CONFIG "flite.conf"
#define MAXLEN 2048
#define DEF_DIR "/tmp"

cst_voice *register_cmu_us_kal(void);
void unregister_cmu_us_kal(cst_voice *v);

static const char *app = "Flite";
static const char *synopsis = "Say text to the user, using Flite TTS engine";
static const char *descrip =
	" Flite(text[,intkeys]): This will invoke the Flite TTS engine, send a text string,\n"
	"get back the resulting waveform and play it to the user, allowing any given interrupt\n"
	"keys to immediately terminate and return the value, or 'any' to allow any number back.\n";

static const char *cachedir;
static int usecache;
static struct ast_config *cfg;
static struct ast_flags config_flags = { 0 };

static int read_config(const char *flite_conf)
{
	const char *temp;
	/* Setting default values */
	usecache = 0;
	cachedir = DEF_DIR;

	cfg = ast_config_load(flite_conf, config_flags);
	if (!cfg || cfg == CONFIG_STATUS_FILEINVALID) {
		ast_log(LOG_WARNING,
				"Flite: Unable to read config file %s. Using default settings\n", flite_conf);
	} else {
		if ((temp = ast_variable_retrieve(cfg, "general", "usecache")))
			usecache = ast_true(temp);
		if ((temp = ast_variable_retrieve(cfg, "general", "cachedir")))
			cachedir = temp;
	}
	return 0;
}

static int flite_exec(struct ast_channel *chan, const char *data)
{
	int res = 0;
	int raw_fd;
	int writecache = 0;
	FILE *fl;
	char *mydata;
	char *format = "sln";
	char cachefile[MAXLEN];
	char tmp_name[18] = "/tmp/flite_XXXXXX";
	char raw_tmp_name[24];
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

	mydata = ast_strdupa(data);
	AST_STANDARD_APP_ARGS(args, mydata);

	if (args.interrupt && !strcasecmp(args.interrupt, "any"))
		args.interrupt = AST_DIGIT_ANY;

	args.text = ast_strip_quoted(args.text, "\"", "\"");
	if (ast_strlen_zero(args.text)) {
		ast_log(LOG_WARNING, "Flite: No text passed for synthesis.\n");
		return res;
	}

	ast_debug(1, "Flite:\nText passed: %s\nInterrupt key(s): %s\n", args.text, args.interrupt);

	/*Cache mechanism */
	if (usecache) {
		char MD5_name[33];
		ast_md5_hash(MD5_name, args.text);
		if (strlen(cachedir) + strlen(MD5_name) + 6 <= MAXLEN) {
			ast_debug(1, "Flite: Activating cache mechanism...\n");
			snprintf(cachefile, sizeof(cachefile), "%s/%s", cachedir, MD5_name);
			if (ast_fileexists(cachefile, NULL, NULL) <= 0) {
				ast_debug(1, "Flite: Cache file does not yet exist.\n");
				writecache = 1;
			} else {
				ast_debug(1, "Flite: Cache file exists.\n");
				if (ast_channel_state(chan) != AST_STATE_UP)
					ast_answer(chan);
				res = ast_streamfile(chan, cachefile, ast_channel_language(chan));
				if (res) {
					ast_log(LOG_ERROR, "Flite: ast_streamfile from cache failed on %s\n",
							ast_channel_name(chan));
				} else {
					res = ast_waitstream(chan, args.interrupt);
					ast_stopstream(chan);
					return res;
				}
			}
		}
	}

	/* Create temp filenames */
	if ((raw_fd = mkstemp(tmp_name)) == -1) {
		ast_log(LOG_ERROR, "Flite: Failed to create audio file.\n");
		return -1;
	}
	if ((fl = fdopen(raw_fd, "w+")) == NULL) {
		ast_log(LOG_ERROR, "Flite: Failed to open audio file '%s'\n", tmp_name);
		return -1;
	}

	/* Invoke Flite */
	flite_init();
	voice = register_cmu_us_kal();
	raw_data = flite_text_to_wave(args.text, voice);
	res = cst_wave_save_raw_fd(raw_data, fl);
	fclose(fl);
	delete_wave(raw_data);
	unregister_cmu_us_kal(voice);
	if (res) {
		ast_log(LOG_ERROR, "Flite: failed to write file %s\n", raw_tmp_name);
		return res;
	}

	snprintf(raw_tmp_name, sizeof(raw_tmp_name), "%s.%s", tmp_name, format);
	rename(tmp_name, raw_tmp_name);
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
		ast_debug(1, "Flite: Saving cache file %s\n", cachefile);
		ast_filerename(tmp_name, cachefile, format);
	} else {
		unlink(raw_tmp_name);
	}
	return res;
}

static int reload_module(void)
{
	ast_config_destroy(cfg);
	read_config(FLITE_CONFIG);
	return 0;
}

static int unload_module(void)
{
	ast_config_destroy(cfg);
	return ast_unregister_application(app);
}

static int load_module(void)
{
	read_config(FLITE_CONFIG);
	return ast_register_application(app, flite_exec, synopsis, descrip) ?
		AST_MODULE_LOAD_DECLINE : AST_MODULE_LOAD_SUCCESS;
}

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_DEFAULT, "Flite TTS Interface",
	.load = load_module,
	.unload = unload_module,
	.reload = reload_module,
);

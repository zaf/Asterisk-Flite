/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 2009, Lefteris Zafiris
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
 * the GNU General Public License Version 2. See the LICENSE file
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
#include <flite/flite.h>
#include "asterisk/file.h"
#include "asterisk/channel.h"
#include "asterisk/module.h"
#include "asterisk/config.h"
#include "asterisk/app.h"
#include "asterisk/utils.h"

#define AST_MODULE "Flite"
#define FLITE_CONFIG "flite.conf"
#define MAXLEN 2048

cst_voice *register_cmu_us_kal(void);

static char *app = "Flite";

static char *synopsis = "Say text to the user, using Flite TTS engine";

static char *descrip =
"  Flite(text[,intkeys]):  This will invoke the Flite TTS engine, send a text string,\n"
"get back the resulting waveform and play it to the user, allowing any given interrupt\n"
"keys to immediately terminate and return the value, or 'any' to allow any number back.\n";

static int app_exec(struct ast_channel *chan, void *data)
{
	int res = 0;
	const char *mydata;
	const char *cachedir = "";
	const char *temp;
	int usecache = 0;
	int writecache = 0;
	char MD5_name[33] = "";
	char cachefile[MAXLEN] = "";
	char tmp_name[22];
	char wav_tmp_name[26];
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
	} else {
		if ((temp = ast_variable_retrieve(cfg, "general", "usecache")))
			usecache = ast_true(temp);
		if (!(cachedir = ast_variable_retrieve(cfg, "general", "cachedir")))
			cachedir = "/tmp";
	}

	mydata = ast_strdupa(data);
	AST_STANDARD_APP_ARGS(args, mydata);

	if (args.interrupt && !strcasecmp(args.interrupt, "any"))
		args.interrupt = AST_DIGIT_ANY;

	ast_debug(1, "Flite: Text passed: %s\nInterrupt key(s): %s\n", args.text,
				args.interrupt);

	/*Cache mechanism */
	if (usecache) {
		ast_md5_hash(MD5_name, args.text);
		if (strlen(cachedir) + strlen(MD5_name) + 5 <= MAXLEN) {
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
					ast_log(LOG_ERROR, "Flite: ast_streamfile failed on %s\n", 
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
	snprintf(tmp_name, sizeof(tmp_name), "/tmp/Flite_%li", ast_random());
	snprintf(wav_tmp_name, sizeof(wav_tmp_name), "%s.wav", tmp_name);

	/* Invoke Flite */
	flite_init();
	voice = register_cmu_us_kal();
	flite_text_to_speech(args.text, voice, wav_tmp_name);

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

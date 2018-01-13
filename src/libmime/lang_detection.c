/*-
 * Copyright 2017 Vsevolod Stakhov
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "lang_detection.h"
#include "libutil/logger.h"
#include "libcryptobox/cryptobox.h"
#include "ucl.h"
#include <glob.h>
#include <unicode/utf8.h>
#include <unicode/ucnv.h>

static const gsize default_short_text_limit = 200;
static const gsize default_words = 20;
static const gchar *default_languages_path = RSPAMD_PLUGINSDIR "/languages";

struct rspamd_language_elt {
	const gchar *name; /* e.g. "en" or "ru" */
	guint unigramms_total; /* total frequencies for unigramms */
	GHashTable *unigramms; /* unigramms frequencies */
	guint bigramms_total; /* total frequencies for bigramms */
	GHashTable *bigramms; /* bigramms frequencies */
	guint trigramms_total; /* total frequencies for trigramms */
	GHashTable *trigramms; /* trigramms frequencies */
};

struct rspamd_lang_detector {
	GPtrArray *languages;
	UConverter *uchar_converter;
	gsize short_text_limit;
};

static guint
rspamd_unigram_hash (gconstpointer key)
{
	return rspamd_cryptobox_fast_hash (key, sizeof (UChar), rspamd_hash_seed ());
}

static gboolean
rspamd_unigram_equal (gconstpointer v, gconstpointer v2)
{
	return memcmp (v, v2, sizeof (UChar)) == 0;
}

static guint
rspamd_bigram_hash (gconstpointer key)
{
	return rspamd_cryptobox_fast_hash (key, 2 * sizeof (UChar), rspamd_hash_seed ());
}

static gboolean
rspamd_bigram_equal (gconstpointer v, gconstpointer v2)
{
	return memcmp (v, v2, 2 * sizeof (UChar)) == 0;
}

static guint
rspamd_trigram_hash (gconstpointer key)
{
	return rspamd_cryptobox_fast_hash (key, 3 * sizeof (UChar), rspamd_hash_seed ());
}

static gboolean
rspamd_trigram_equal (gconstpointer v, gconstpointer v2)
{
	return memcmp (v, v2, 3 * sizeof (UChar)) == 0;
}

static void
rspamd_language_detector_read_file (struct rspamd_config *cfg,
		struct rspamd_lang_detector *d,
		const gchar *path)
{
	struct ucl_parser *parser;
	ucl_object_t *top;
	const ucl_object_t *freqs, *cur;
	ucl_object_iter_t it = NULL;
	UErrorCode uc_err = U_ZERO_ERROR;
	struct rspamd_language_elt *nelt;
	gchar *pos;

	parser = ucl_parser_new (UCL_PARSER_NO_FILEVARS);
	if (!ucl_parser_add_file (parser, path)) {
		msg_warn_config ("cannot parse file %s: %s", path,
				ucl_parser_get_error (parser));
		ucl_parser_free (parser);

		return;
	}

	top = ucl_parser_get_object (parser);
	ucl_parser_free (parser);

	freqs = ucl_object_lookup (top, "freq");

	if (freqs == NULL) {
		msg_warn_config ("file %s has no 'freq' key", path);
		ucl_object_unref (top);

		return;
	}

	pos = strrchr (path, '/');
	g_assert (pos != NULL);
	nelt = rspamd_mempool_alloc0 (cfg->cfg_pool, sizeof (*nelt));
	nelt->name = rspamd_mempool_strdup (cfg->cfg_pool, pos + 1);
	/* Remove extension */
	pos = strchr (nelt->name, '.');
	g_assert (pos != NULL);
	*pos = '\0';
	nelt->unigramms = g_hash_table_new (rspamd_unigram_hash, rspamd_unigram_equal);
	nelt->bigramms = g_hash_table_new (rspamd_bigram_hash, rspamd_bigram_equal);
	nelt->trigramms = g_hash_table_new (rspamd_trigram_hash, rspamd_trigram_equal);

	while ((cur = ucl_object_iterate (freqs, &it, true)) != NULL) {
		const gchar *key;
		gsize keylen;
		guint freq, nsym;
		UChar *ucs_key;

		key = ucl_object_keyl (cur, &keylen);
		freq = ucl_object_toint (cur);

		if (key != NULL) {
			ucs_key = rspamd_mempool_alloc (cfg->cfg_pool,
					(keylen + 1) * sizeof (UChar));

			nsym = ucnv_toUChars (d->uchar_converter, ucs_key, keylen + 1, key,
					keylen, &uc_err);

			if (uc_err != U_ZERO_ERROR) {
				msg_warn_config ("cannot convert key to unicode: %s",
						u_errorName (uc_err));

				continue;
			}

			if (nsym == 2) {
				/* We have a digraph */
				g_hash_table_insert (nelt->bigramms, ucs_key,
						GUINT_TO_POINTER (freq));
				nelt->bigramms_total += freq;
			}
			else if (nsym == 3) {
				g_hash_table_insert (nelt->trigramms, ucs_key,
						GUINT_TO_POINTER (freq));
				nelt->trigramms_total += freq;
			}
			else if (nsym == 1) {
				g_hash_table_insert (nelt->unigramms, ucs_key,
						GUINT_TO_POINTER (freq));
				nelt->unigramms_total += freq;
			}
			else if (nsym > 3) {
				msg_warn_config ("have more than 3 characters in key: %d", nsym);
			}
		}
	}

	msg_info_config ("loaded %s language, %d unigramms, %d digramms, %d trigramms",
			nelt->name,
			(gint)g_hash_table_size (nelt->unigramms),
			(gint)g_hash_table_size (nelt->bigramms),
			(gint)g_hash_table_size (nelt->trigramms));

	g_ptr_array_add (d->languages, nelt);
	ucl_object_unref (top);
}

struct rspamd_lang_detector*
rspamd_language_detector_init (struct rspamd_config *cfg)
{
	const ucl_object_t *section, *elt;
	const gchar *languages_path = default_languages_path;
	glob_t gl;
	size_t i, short_text_limit = default_short_text_limit;
	UErrorCode uc_err = U_ZERO_ERROR;
	GString *languages_pattern;
	struct rspamd_lang_detector *ret = NULL;

	section = ucl_object_lookup (cfg->rcl_obj, "lang_detection");

	if (section != NULL) {
		elt = ucl_object_lookup (section, "languages");

		if (elt) {
			languages_path = ucl_object_tostring (elt);
		}

		elt = ucl_object_lookup (section, "short_text_limit");

		if (elt) {
			short_text_limit = ucl_object_toint (elt);
		}
	}

	languages_pattern = g_string_sized_new (PATH_MAX);
	rspamd_printf_gstring (languages_pattern, "%s/*.json", languages_path);
	memset (&gl, 0, sizeof (gl));

	if (glob (languages_pattern->str, 0, NULL, &gl) != 0) {
		msg_err_config ("cannot read any files matching %v", languages_pattern);
		goto end;
	}

	ret = rspamd_mempool_alloc (cfg->cfg_pool, sizeof (*ret));
	ret->languages = g_ptr_array_sized_new (gl.gl_pathc);
	ret->uchar_converter = ucnv_open ("UTF-8", &uc_err);
	ret->short_text_limit = short_text_limit;

	g_assert (uc_err == U_ZERO_ERROR);

	for (i = 0; i < gl.gl_pathc; i ++) {
		rspamd_language_detector_read_file (cfg, ret, gl.gl_pathv[i]);
	}

	msg_info_config ("loaded %d languages", (gint)ret->languages->len);
end:
	if (gl.gl_pathc > 0) {
		globfree (&gl);
	}

	g_string_free (languages_pattern, TRUE);

	return ret;
}


void
rspamd_language_detector_to_ucs (struct rspamd_lang_detector *d,
		rspamd_mempool_t *pool,
		rspamd_stat_token_t *utf_token, rspamd_stat_token_t *ucs_token)
{
	UChar *out;
	int32_t nsym;
	UErrorCode uc_err = U_ZERO_ERROR;

	ucs_token->flags = utf_token->flags;
	out = rspamd_mempool_alloc (pool, sizeof (*out) * (utf_token->len + 1));
	nsym = ucnv_toUChars (d->uchar_converter, out, (utf_token->len + 1),
			utf_token->begin, utf_token->len, &uc_err);

	if (nsym >= 0) {
		ucs_token->begin = (const gchar *) out;
		ucs_token->len = nsym;
	}
	else {
		ucs_token->len = 0;
	}
}

static void
rspamd_language_detector_random_select (GPtrArray *ucs_tokens, guint nwords,
		goffset *offsets_out)
{
	guint step_len, remainder, i, out_idx;
	guint64 coin, sel;

	g_assert (nwords != 0);
	g_assert (offsets_out != NULL);
	g_assert (ucs_tokens->len >= nwords);
	/*
	 * We split input array into `nwords` parts. For each part we randomly select
	 * an element from this particular split. Here is an example:
	 *
	 * nwords=2, input_len=5
	 *
	 * w1 w2 w3   w4 w5
	 * ^          ^
	 * part1      part2
	 *  vv         vv
	 *  w2         w5
	 *
	 * So we have 2 output words from 5 input words selected randomly within
	 * their splits. It is not uniform distribution but it seems to be better
	 * to include words from different text parts
	 */
	step_len = ucs_tokens->len / nwords;
	remainder = ucs_tokens->len % nwords;

	out_idx = 0;
	coin = rspamd_random_uint64_fast ();
	sel = coin % (step_len + remainder);
	offsets_out[out_idx] = sel;

	for (i = step_len + remainder; i < ucs_tokens->len;
			i += step_len, out_idx ++) {
		coin = rspamd_random_uint64_fast ();
		sel = (coin % step_len) + i;
		offsets_out[out_idx] = sel;
	}
}

enum rspamd_language_gramm_type {
	rs_unigramm = 0,
	rs_bigramm,
	rs_trigramm
};

static goffset
rspamd_language_detector_next_ngramm (rspamd_stat_token_t *tok, UChar *window,
		guint wlen, goffset cur_off)
{
	guint i;

	if (wlen > 1) {
		/* Deal with spaces at the beginning and ending */

		if (cur_off == 0) {
			window[0] = (UChar)' ';

			for (i = 0; i < wlen - 1; i ++) {
				window[i + 1] = *(((UChar *)tok->begin) + i);
			}
		}
		else if (cur_off + wlen == tok->len + 1) {
			/* Add trailing space */
			for (i = 0; i < wlen - 1; i ++) {
				window[i] = *(((UChar *)tok->begin) + cur_off + i);
			}
			window[wlen - 1] = (UChar)' ';
		}
		else if (cur_off + wlen > tok->len + 1) {
			/* No more fun */
			return -1;
		}

		/* Normal case */
		for (i = 0; i < wlen; i ++) {
			window[i] = *(((UChar *)tok->begin) + cur_off + i);
		}
	}
	else {
		if (tok->len >= cur_off) {
			return -1;
		}

		window[0] = *(((UChar *)tok->begin) + cur_off);
	}

	return cur_off + 1;
}

/*
 * Do full guess for a specific ngramm, checking all languages defined
 */
static void
rspamd_language_detector_process_ngramm_full (struct rspamd_lang_detector *d,
		UChar *window, enum rspamd_language_gramm_type type,
		GHashTable *candidates)
{
	guint i, freq;
	struct rspamd_language_elt *elt;
	struct rspamd_lang_detector_res *cand;
	GHashTable *ngramms;

	for (i = 0; i < d->languages->len; i ++) {
		elt = g_ptr_array_index (d->languages, i);

		switch (type) {
		case rs_unigramm:
			ngramms = elt->unigramms;
			break;
		case rs_bigramm:
			ngramms = elt->bigramms;
			break;
		case rs_trigramm:
			ngramms = elt->trigramms;
			break;
		}

		freq = GPOINTER_TO_UINT (g_hash_table_lookup (ngramms, window));
		cand = g_hash_table_lookup (candidates, elt->name);

		if (cand == NULL) {
			cand = g_malloc (sizeof (*cand));
			cand->elt = elt;
			cand->lang = elt->name;
			cand->prob = freq;
		}
		else {
			/* Update guess */
			cand->prob += freq;
		}
	}
}

/*
 * Check only candidates, if none found, switch to full version
 */
static void
rspamd_language_detector_process_ngramm_update (struct rspamd_lang_detector *d,
		UChar *window, enum rspamd_language_gramm_type type,
		GHashTable *candidates)
{
	guint freq, total_freq = 0;
	struct rspamd_language_elt *elt;
	struct rspamd_lang_detector_res *cand;
	GHashTableIter it;
	gpointer k, v;
	GHashTable *ngramms;

	g_hash_table_iter_init (&it, candidates);

	while (g_hash_table_iter_next (&it, &k, &v)) {
		cand = (struct rspamd_lang_detector_res *)v;
		elt = cand->elt;

		switch (type) {
		case rs_unigramm:
			ngramms = elt->unigramms;
			break;
		case rs_bigramm:
			ngramms = elt->bigramms;
			break;
		case rs_trigramm:
			ngramms = elt->trigramms;
			break;
		}

		freq = GPOINTER_TO_UINT (g_hash_table_lookup (ngramms, window));

		cand->prob += freq;
		total_freq += freq;
	}

	if (total_freq == 0) {
		/* Nothing found , do full scan which will also update candidates */
		rspamd_language_detector_process_ngramm_full (d, window, type, candidates);
	}
}

static gboolean
rspamd_language_detector_update_guess (struct rspamd_lang_detector *d,
		rspamd_stat_token_t *tok, GHashTable *candidates,
		enum rspamd_language_gramm_type type)
{
	guint wlen;
	UChar window[3];
	goffset cur = 0;

	switch (type) {
	case rs_unigramm:
		wlen = 1;
		break;
	case rs_bigramm:
		wlen = 2;
		break;
	case rs_trigramm:
		wlen = 3;
		break;
	}

	/* Split words */
	while ((cur = rspamd_language_detector_next_ngramm (tok, window, wlen, cur))
			!= -1) {

	}
}

static void
rspamd_language_detector_detect_word (struct rspamd_lang_detector *d,
		rspamd_stat_token_t *tok, GHashTable *candidates,
		enum rspamd_language_gramm_type type)
{
	guint wlen;
	UChar window[3];
	goffset cur = 0;

	switch (type) {
	case rs_unigramm:
		wlen = 1;
		break;
	case rs_bigramm:
		wlen = 2;
		break;
	case rs_trigramm:
		wlen = 3;
		break;
	}

	/* Split words */
	while ((cur = rspamd_language_detector_next_ngramm (tok, window, wlen, cur))
			!= -1) {

	}
}

const gchar *
rspamd_language_detector_detect (struct rspamd_lang_detector *d,
		GPtrArray *ucs_tokens, gsize words_len)
{
	if (words_len < d->short_text_limit) {
		/* For short text, start directly from trigramms */
	}

	/* Start with unigramms */

}
/*
 * obs-fsr1-filter
 *
 * Videofilter für OBS Studio zum Hochskalieren von Webcams und Capture-Karten
 * mit niedriger Auflösung. Fünf Algorithmen stehen zur Wahl:
 *
 *   - AMD FidelityFX Super Resolution 1.0 (EASU + RCAS), data/effects/fsr1.effect
 *   - NVIDIA Image Scaling (NVScaler),                  data/effects/nis.effect
 *   - RAVU-Zoom-AR (r3, aus mpv-prescalers),            data/effects/ravu.effect
 *                                                       + Gewichtstabellen in data/ravu/
 *   - Snapdragon Game Super Resolution 1 (SGSR),        data/effects/sgsr.effect
 *   - FSRCNNX (neuronales Netz, 8 oder 16 Kanäle),      data/effects/fsrcnnx_{8,16}.effect
 *                                                       + Pass-Tabellen src/fsrcnnx_*_passes.h
 *
 * Jeder Algorithmus liegt in einer eigenen Effect-Datei. Schlägt das Laden eines anderen
 * Algorithmus fehl, bleibt FSR 1 voll nutzbar (der Filter fällt dann darauf zurück).
 * FSRCNNX wird erst geladen, wenn es gewählt ist (große Effects, viele Zwischenbilder).
 *
 * Lizenz: GPL-2.0-or-later (wie libobs). Die Shader basieren auf AMDs ffx_fsr1.h (MIT),
 * NVIDIAs NIS_Scaler.h (MIT), RAVU von Bin Jin (LGPL-3.0-or-later), SGSR von Qualcomm
 * (BSD-3-Clause) und FSRCNNX von igv (LGPL-3.0-or-later).
 * Siehe THIRD_PARTY_NOTICES.txt.
 */

#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>
#include <util/platform.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "nis_coef.h"

/* Ein Rechen-Pass von FSRCNNX (Tabellen aus tools/gen_fsrcnnx.py) */
#define FSRCNNX_MAX_INPUTS 5
struct fsrcnnx_pass {
	const char *tech;
	int out;    /* Platz des Ergebnisses */
	int inputs; /* Anzahl Eingaben */
	int in[FSRCNNX_MAX_INPUTS]; /* Plätze der Eingaben, -1 = Quellbild, -2 = unbenutzt */
};
#include "fsrcnnx_8_passes.h"
#include "fsrcnnx_16_passes.h"
#define FSRCNNX_MAX_SLOTS 12

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-fsr1-filter", "en-US")

/* Die ID bleibt bewusst "fsr1_upscale_filter", damit bestehende Szenen weiter funktionieren. */
#define FILTER_ID "fsr1_upscale_filter"

#define S_ALGO "algorithm"
#define S_FACTOR "factor"
#define S_CUSTOM "custom_size"
#define S_WIDTH "width"
#define S_HEIGHT "height"
#define S_SHARPNESS "sharpness"
#define S_DENOISE "denoise"
#define S_NIS_SHARPNESS "nis_sharpness"
#define S_SGSR_SHARPNESS "sgsr_sharpness"

#define V_ALGO_FSR1 "fsr1"
#define V_ALGO_NIS "nis"
#define V_ALGO_RAVU "ravu"
#define V_ALGO_SGSR "sgsr"
#define V_ALGO_FSRCNNX "fsrcnnx"
#define V_ALGO_FSRCNNX_HQ "fsrcnnx_hq"

#define T_ALGO obs_module_text("Algorithm")
#define T_ALGO_FSR1 obs_module_text("AlgoFSR1")
#define T_ALGO_NIS obs_module_text("AlgoNIS")
#define T_ALGO_RAVU obs_module_text("AlgoRAVU")
#define T_ALGO_SGSR obs_module_text("AlgoSGSR")
#define T_ALGO_FSRCNNX obs_module_text("AlgoFSRCNNX")
#define T_ALGO_FSRCNNX_HQ obs_module_text("AlgoFSRCNNXHQ")
#define T_FACTOR obs_module_text("Factor")
#define T_CUSTOM obs_module_text("CustomSize")
#define T_WIDTH obs_module_text("Width")
#define T_HEIGHT obs_module_text("Height")
#define T_SHARPNESS obs_module_text("Sharpness")
#define T_DENOISE obs_module_text("Denoise")
#define T_NIS_SHARPNESS obs_module_text("NISSharpness")
#define T_SGSR_SHARPNESS obs_module_text("SGSRSharpness")

#define MAX_OUTPUT_DIMENSION 8192

/* Gewichtstabellen von RAVU-Zoom-AR r3 (RGBA16F), erzeugt von tools/gen_ravu.py */
#define RAVU_LUT_FILE "ravu/lut_45x2592.bin"
#define RAVU_LUT_W 45
#define RAVU_LUT_H 2592
#define RAVU_LUT_AR_FILE "ravu/lut_ar_18x2592.bin"
#define RAVU_LUT_AR_W 18
#define RAVU_LUT_AR_H 2592

/* SGSR: Kantenschwelle wie im Original für Mobilgeräte (8/255); Webcams rauschen, die
 * strengere VR-Schwelle 4/255 würde Rauschen mitschärfen. */
#define SGSR_EDGE_THRESHOLD (8.0f / 255.0f)

enum algorithm {
	ALGO_FSR1,
	ALGO_NIS,
	ALGO_RAVU,
	ALGO_SGSR,
	ALGO_FSRCNNX,
	ALGO_FSRCNNX_HQ,
};

/* FSRCNNX: ein Netz (8 oder 16 Kanäle) mit seinen Zwischenbildern */
struct fsrcnnx_net {
	const char *file;
	const struct fsrcnnx_pass *passes;
	int npasses;
	int nslots;
	int agg_in;

	bool tried; /* Laden versucht (bei Fehler nicht jedes Bild erneut) */
	gs_effect_t *effect;
	gs_eparam_t *p_image;
	gs_eparam_t *p_t[FSRCNNX_MAX_INPUTS];
	gs_eparam_t *p_y2x;
	gs_eparam_t *p_texel;
	gs_eparam_t *p_input_size;
	gs_eparam_t *p_y2x_size;
	gs_texrender_t *src;                      /* Quellbild (Eingabegröße) */
	gs_texrender_t *slot[FSRCNNX_MAX_SLOTS];  /* Zwischenbilder RGBA16F (Eingabegröße) */
	gs_texrender_t *y2x;                      /* Y' in doppelter Größe */
};

struct fsr1_filter {
	obs_source_t *context;

	/* FSR 1 */
	gs_effect_t *effect;
	gs_eparam_t *p_input_size;
	gs_eparam_t *p_output_size;
	gs_eparam_t *p_image;
	gs_eparam_t *p_rcas_sharp;
	gs_eparam_t *p_rcas_denoise;
	gs_texrender_t *easu_target; /* Zwischenziel für das EASU-Ergebnis */

	/* NIS (optional) */
	gs_effect_t *nis_effect;
	gs_eparam_t *n_input_size;
	gs_eparam_t *n_output_size;
	gs_eparam_t *n_coef;
	gs_eparam_t *n_detect;
	gs_eparam_t *n_sharp1;
	gs_eparam_t *n_sharp2;
	gs_texture_t *nis_coef_tex;

	/* RAVU-Zoom-AR (optional) */
	gs_effect_t *ravu_effect;
	gs_eparam_t *r_input_size;
	gs_eparam_t *r_lut;
	gs_eparam_t *r_lut_ar;
	gs_texture_t *ravu_lut_tex;
	gs_texture_t *ravu_lut_ar_tex;

	/* SGSR (optional) */
	gs_effect_t *sgsr_effect;
	gs_eparam_t *g_input_size;
	gs_eparam_t *g_texel;
	gs_eparam_t *g_edge_threshold;
	gs_eparam_t *g_edge_sharpness;

	/* FSRCNNX (optional, erst bei Bedarf geladen) */
	struct fsrcnnx_net fsrcnnx;
	struct fsrcnnx_net fsrcnnx_hq;

	/* Einstellungen */
	enum algorithm algo;
	double factor;
	bool custom_size;
	uint32_t custom_w;
	uint32_t custom_h;
	double sharpness;     /* FSR 1: 0..100 % */
	bool denoise;         /* FSR 1 */
	double nis_sharpness; /* NIS: 0..100 % */
	double sgsr_sharpness; /* SGSR: 0..100 % (Kantenschärfe 1,0..2,0) */

	/* Größen und Wahl des Algorithmus (in video_tick berechnet) */
	uint32_t in_w;
	uint32_t in_h;
	uint32_t out_w;
	uint32_t out_h;
	enum algorithm active; /* tatsächlich verwendeter Algorithmus (nach Rückfall) */
	bool valid;

	/* Hinweise nur einmal ins Log schreiben */
	bool warned_no_nis;
	bool warned_nis_range;
	bool warned_no_ravu;
	bool warned_ravu_range;
	bool warned_no_sgsr;
	bool warned_sgsr_range;
	bool warned_no_fsrcnnx;
	bool warned_fsrcnnx_range;

	/* Zuletzt ins Log geschriebener Zustand (nur bei Änderung neu loggen) */
	bool logged;
	enum algorithm logged_active;
	uint32_t logged_in_w;
	uint32_t logged_in_h;
	uint32_t logged_out_w;
	uint32_t logged_out_h;
};

static const char *algo_name(enum algorithm a)
{
	switch (a) {
	case ALGO_NIS:
		return "NIS";
	case ALGO_RAVU:
		return "RAVU";
	case ALGO_SGSR:
		return "SGSR";
	case ALGO_FSRCNNX:
		return "FSRCNNX 8";
	case ALGO_FSRCNNX_HQ:
		return "FSRCNNX 16";
	case ALGO_FSR1:
	default:
		return "FSR 1";
	}
}

static const char *fsr1_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("UpscaleFilter");
}

static void fsr1_update(void *data, obs_data_t *settings)
{
	struct fsr1_filter *f = data;

	const char *algo = obs_data_get_string(settings, S_ALGO);
	if (algo && strcmp(algo, V_ALGO_NIS) == 0)
		f->algo = ALGO_NIS;
	else if (algo && strcmp(algo, V_ALGO_RAVU) == 0)
		f->algo = ALGO_RAVU;
	else if (algo && strcmp(algo, V_ALGO_SGSR) == 0)
		f->algo = ALGO_SGSR;
	else if (algo && strcmp(algo, V_ALGO_FSRCNNX) == 0)
		f->algo = ALGO_FSRCNNX;
	else if (algo && strcmp(algo, V_ALGO_FSRCNNX_HQ) == 0)
		f->algo = ALGO_FSRCNNX_HQ;
	else
		f->algo = ALGO_FSR1;

	f->factor = obs_data_get_double(settings, S_FACTOR);
	f->custom_size = obs_data_get_bool(settings, S_CUSTOM);
	f->custom_w = (uint32_t)obs_data_get_int(settings, S_WIDTH);
	f->custom_h = (uint32_t)obs_data_get_int(settings, S_HEIGHT);
	f->sharpness = obs_data_get_double(settings, S_SHARPNESS);
	f->denoise = obs_data_get_bool(settings, S_DENOISE);
	f->nis_sharpness = obs_data_get_double(settings, S_NIS_SHARPNESS);
	f->sgsr_sharpness = obs_data_get_double(settings, S_SGSR_SHARPNESS);
}

/* Gibt alles eines FSRCNNX-Netzes frei (im Grafikkontext); tried bleibt erhalten. */
static void fsrcnnx_free(struct fsrcnnx_net *n)
{
	gs_effect_destroy(n->effect);
	n->effect = NULL;
	gs_texrender_destroy(n->src);
	n->src = NULL;
	for (int i = 0; i < FSRCNNX_MAX_SLOTS; i++) {
		gs_texrender_destroy(n->slot[i]);
		n->slot[i] = NULL;
	}
	gs_texrender_destroy(n->y2x);
	n->y2x = NULL;
}

static void fsr1_destroy(void *data)
{
	struct fsr1_filter *f = data;

	obs_enter_graphics();
	gs_texrender_destroy(f->easu_target);
	gs_effect_destroy(f->effect);
	gs_effect_destroy(f->nis_effect);
	gs_texture_destroy(f->nis_coef_tex);
	gs_effect_destroy(f->ravu_effect);
	gs_texture_destroy(f->ravu_lut_tex);
	gs_texture_destroy(f->ravu_lut_ar_tex);
	gs_effect_destroy(f->sgsr_effect);
	fsrcnnx_free(&f->fsrcnnx);
	fsrcnnx_free(&f->fsrcnnx_hq);
	obs_leave_graphics();

	bfree(f);
}

/*
 * Lädt eine Effect-Datei aus dem Datenordner des Plugins. Compiler-Meldungen von OBS
 * werden ins Log geschrieben: ohne Fehlerpuffer verwirft OBS sie sonst stillschweigend.
 * Muss im Grafikkontext aufgerufen werden.
 */
static gs_effect_t *load_effect(const char *name)
{
	char *path = obs_module_file(name);
	char *error = NULL;

	if (!path) {
		blog(LOG_ERROR, "[obs-fsr1-filter] %s nicht im Datenordner des Plugins gefunden", name);
		return NULL;
	}

	gs_effect_t *effect = gs_effect_create_from_file(path, &error);
	if (!effect) {
		blog(LOG_ERROR, "[obs-fsr1-filter] Konnte %s nicht laden:\n%s", path,
		     error ? error : "(keine Fehlermeldung von OBS)");
	}

	bfree(error);
	bfree(path);
	return effect;
}

/* Koeffiziententextur für NIS: 4 x 64 Texel (RGBA32F).
 * Zeile = Phase, Texel 0/1 = Skalierer (8 Werte), Texel 2/3 = USM (8 Werte). */
static gs_texture_t *create_nis_coef_texture(void)
{
	float data[64 * 4 * 4];

	for (int phase = 0; phase < 64; phase++) {
		for (int k = 0; k < 8; k++) {
			data[(phase * 4 + 0) * 4 + k] = nis_coef_scale[phase][k];
			data[(phase * 4 + 2) * 4 + k] = nis_coef_usm[phase][k];
		}
	}

	const uint8_t *levels[1] = {(const uint8_t *)data};
	return gs_texture_create(4, 64, GS_RGBA32F, 1, levels, 0);
}

/*
 * Lädt eine RAVU-Gewichtstabelle (rohe RGBA16F-Daten, w x h Texel) aus dem
 * Datenordner und legt sie als Textur an. Die Dateigröße muss exakt stimmen.
 * Muss im Grafikkontext aufgerufen werden.
 */
static gs_texture_t *load_lut_texture(const char *name, uint32_t w, uint32_t h)
{
	const size_t expected = (size_t)w * h * 4 * 2;
	gs_texture_t *tex = NULL;
	uint8_t *buf = NULL;
	FILE *fp = NULL;

	char *path = obs_module_file(name);
	if (!path) {
		blog(LOG_ERROR, "[obs-fsr1-filter] %s nicht im Datenordner des Plugins gefunden", name);
		return NULL;
	}

	fp = os_fopen(path, "rb");
	if (!fp) {
		blog(LOG_ERROR, "[obs-fsr1-filter] Konnte %s nicht öffnen", path);
		goto out;
	}

	buf = bmalloc(expected + 1);
	size_t got = fread(buf, 1, expected + 1, fp);
	if (got != expected) {
		blog(LOG_ERROR, "[obs-fsr1-filter] %s hat die falsche Größe (%zu statt %zu Bytes)", path, got,
		     expected);
		goto out;
	}

	const uint8_t *levels[1] = {buf};
	tex = gs_texture_create(w, h, GS_RGBA16F, 1, levels, 0);
	if (!tex)
		blog(LOG_ERROR, "[obs-fsr1-filter] Konnte Textur aus %s nicht erzeugen", path);

out:
	if (fp)
		fclose(fp);
	bfree(buf);
	bfree(path);
	return tex;
}

/* RAVU laden; bei jedem Fehler bleibt alles NULL und der Filter nutzt FSR 1. */
static void load_ravu(struct fsr1_filter *f)
{
	f->ravu_effect = load_effect("effects/ravu.effect");
	if (!f->ravu_effect)
		return;

	f->ravu_lut_tex = load_lut_texture(RAVU_LUT_FILE, RAVU_LUT_W, RAVU_LUT_H);
	f->ravu_lut_ar_tex = load_lut_texture(RAVU_LUT_AR_FILE, RAVU_LUT_AR_W, RAVU_LUT_AR_H);
	if (!f->ravu_lut_tex || !f->ravu_lut_ar_tex) {
		gs_effect_destroy(f->ravu_effect);
		gs_texture_destroy(f->ravu_lut_tex);
		gs_texture_destroy(f->ravu_lut_ar_tex);
		f->ravu_effect = NULL;
		f->ravu_lut_tex = NULL;
		f->ravu_lut_ar_tex = NULL;
	}
}

/*
 * FSRCNNX laden: Effect, Parameter und die Zwischenziele. Bei jedem Fehler bleibt das Netz
 * leer (effect == NULL) und der Filter nutzt FSR 1. Muss im Grafikkontext aufgerufen werden.
 */
static void load_fsrcnnx(struct fsrcnnx_net *n)
{
	n->tried = true;
	if (n->nslots > FSRCNNX_MAX_SLOTS) {
		blog(LOG_ERROR, "[obs-fsr1-filter] %s braucht %d Zwischenbilder, Platz für %d", n->file, n->nslots,
		     FSRCNNX_MAX_SLOTS);
		return;
	}
	n->effect = load_effect(n->file);
	if (!n->effect)
		return;

	n->p_image = gs_effect_get_param_by_name(n->effect, "image");
	for (int i = 0; i < FSRCNNX_MAX_INPUTS; i++) {
		char name[4];
		snprintf(name, sizeof(name), "t%d", i);
		n->p_t[i] = gs_effect_get_param_by_name(n->effect, name);
	}
	n->p_y2x = gs_effect_get_param_by_name(n->effect, "y2x");
	n->p_texel = gs_effect_get_param_by_name(n->effect, "texel");
	n->p_input_size = gs_effect_get_param_by_name(n->effect, "input_size");
	n->p_y2x_size = gs_effect_get_param_by_name(n->effect, "y2x_size");

	bool ok = n->p_image && n->p_y2x && n->p_texel && n->p_input_size && n->p_y2x_size;
	n->src = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	n->y2x = gs_texrender_create(GS_RGBA16F, GS_ZS_NONE);
	ok = ok && n->src && n->y2x;
	for (int i = 0; i < n->nslots; i++) {
		n->slot[i] = gs_texrender_create(GS_RGBA16F, GS_ZS_NONE);
		ok = ok && n->slot[i];
	}
	if (!ok) {
		blog(LOG_ERROR, "[obs-fsr1-filter] %s: Parameter oder Zwischenziele fehlen", n->file);
		fsrcnnx_free(n);
		return;
	}
	blog(LOG_INFO, "[obs-fsr1-filter] %s geladen (%d Passes, %d Zwischenbilder)", n->file, n->npasses,
	     n->nslots);
}

static void *fsr1_create(obs_data_t *settings, obs_source_t *context)
{
	struct fsr1_filter *f = bzalloc(sizeof(struct fsr1_filter));
	f->context = context;

	obs_enter_graphics();

	f->easu_target = gs_texrender_create(GS_RGBA, GS_ZS_NONE);

	/* FSR 1 ist Pflicht: ohne diesen Effect ist der Filter unbrauchbar. */
	f->effect = load_effect("effects/fsr1.effect");

	/* NIS ist optional: bei einem Fehler bleibt FSR 1 verfügbar. */
	f->nis_effect = load_effect("effects/nis.effect");
	if (f->nis_effect) {
		f->nis_coef_tex = create_nis_coef_texture();
		if (!f->nis_coef_tex) {
			blog(LOG_ERROR, "[obs-fsr1-filter] Konnte NIS-Koeffiziententextur nicht erzeugen");
			gs_effect_destroy(f->nis_effect);
			f->nis_effect = NULL;
		}
	}

	/* RAVU ist ebenfalls optional. */
	load_ravu(f);

	/* SGSR ist optional und klein (ein Pass). */
	f->sgsr_effect = load_effect("effects/sgsr.effect");

	obs_leave_graphics();

	if (!f->effect) {
		fsr1_destroy(f);
		return NULL;
	}

	f->p_image = gs_effect_get_param_by_name(f->effect, "image");
	f->p_input_size = gs_effect_get_param_by_name(f->effect, "input_size");
	f->p_output_size = gs_effect_get_param_by_name(f->effect, "output_size");
	f->p_rcas_sharp = gs_effect_get_param_by_name(f->effect, "rcas_sharp");
	f->p_rcas_denoise = gs_effect_get_param_by_name(f->effect, "rcas_denoise");

	if (f->nis_effect) {
		f->n_input_size = gs_effect_get_param_by_name(f->nis_effect, "input_size");
		f->n_output_size = gs_effect_get_param_by_name(f->nis_effect, "output_size");
		f->n_coef = gs_effect_get_param_by_name(f->nis_effect, "coef");
		f->n_detect = gs_effect_get_param_by_name(f->nis_effect, "nis_detect");
		f->n_sharp1 = gs_effect_get_param_by_name(f->nis_effect, "nis_sharp1");
		f->n_sharp2 = gs_effect_get_param_by_name(f->nis_effect, "nis_sharp2");
	}

	if (f->ravu_effect) {
		f->r_input_size = gs_effect_get_param_by_name(f->ravu_effect, "input_size");
		f->r_lut = gs_effect_get_param_by_name(f->ravu_effect, "lut");
		f->r_lut_ar = gs_effect_get_param_by_name(f->ravu_effect, "lut_ar");
	}

	if (f->sgsr_effect) {
		f->g_input_size = gs_effect_get_param_by_name(f->sgsr_effect, "input_size");
		f->g_texel = gs_effect_get_param_by_name(f->sgsr_effect, "texel");
		f->g_edge_threshold = gs_effect_get_param_by_name(f->sgsr_effect, "edge_threshold");
		f->g_edge_sharpness = gs_effect_get_param_by_name(f->sgsr_effect, "edge_sharpness");
	}

	f->fsrcnnx.file = "effects/fsrcnnx_8.effect";
	f->fsrcnnx.passes = fsrcnnx_8_passes;
	f->fsrcnnx.npasses = FSRCNNX_8_PASSES;
	f->fsrcnnx.nslots = FSRCNNX_8_SLOTS;
	f->fsrcnnx.agg_in = FSRCNNX_8_AGG_IN;
	f->fsrcnnx_hq.file = "effects/fsrcnnx_16.effect";
	f->fsrcnnx_hq.passes = fsrcnnx_16_passes;
	f->fsrcnnx_hq.npasses = FSRCNNX_16_PASSES;
	f->fsrcnnx_hq.nslots = FSRCNNX_16_SLOTS;
	f->fsrcnnx_hq.agg_in = FSRCNNX_16_AGG_IN;

	blog(LOG_INFO, "[obs-fsr1-filter] Filter angelegt, verfügbar: FSR 1%s%s%s, FSRCNNX bei Bedarf",
	     f->nis_effect ? ", NIS" : "", f->ravu_effect ? ", RAVU" : "", f->sgsr_effect ? ", SGSR" : "");

	fsr1_update(f, settings);
	return f;
}

/*
 * Schreibt ins Log, welcher Algorithmus mit welchen Größen läuft, aber nur wenn
 * sich daran etwas geändert hat (Umschalten, neue Kamera-Auflösung, neue Zielgröße).
 * Schieberegler wie die Schärfe lösen bewusst keine Zeile aus.
 */
static void log_state(struct fsr1_filter *f)
{
	if (f->logged && f->logged_active == f->active && f->logged_in_w == f->in_w &&
	    f->logged_in_h == f->in_h && f->logged_out_w == f->out_w && f->logged_out_h == f->out_h)
		return;

	obs_source_t *parent = obs_filter_get_parent(f->context);
	const char *name = parent ? obs_source_get_name(parent) : NULL;

	if (f->active != f->algo) {
		blog(LOG_INFO, "[obs-fsr1-filter] '%s': %s %ux%u -> %ux%u (gewählt: %s, Rückfall)",
		     name ? name : "?", algo_name(f->active), f->in_w, f->in_h, f->out_w, f->out_h,
		     algo_name(f->algo));
	} else {
		blog(LOG_INFO, "[obs-fsr1-filter] '%s': %s %ux%u -> %ux%u", name ? name : "?",
		     algo_name(f->active), f->in_w, f->in_h, f->out_w, f->out_h);
	}

	f->logged = true;
	f->logged_active = f->active;
	f->logged_in_w = f->in_w;
	f->logged_in_h = f->in_h;
	f->logged_out_w = f->out_w;
	f->logged_out_h = f->out_h;
}

static inline uint32_t clamp_dim(double v)
{
	if (v < 2.0)
		return 2;
	if (v > (double)MAX_OUTPUT_DIMENSION)
		return MAX_OUTPUT_DIMENSION;
	return (uint32_t)(v + 0.5);
}

static void fsr1_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);
	struct fsr1_filter *f = data;

	f->valid = false;
	f->active = ALGO_FSR1;
	f->in_w = f->in_h = 0;
	f->out_w = f->out_h = 0;

	obs_source_t *target = obs_filter_get_target(f->context);
	if (!target)
		return;

	uint32_t w = obs_source_get_base_width(target);
	uint32_t h = obs_source_get_base_height(target);
	if (!w || !h)
		return;

	f->in_w = w;
	f->in_h = h;

	if (f->custom_size && f->custom_w >= 2 && f->custom_h >= 2) {
		f->out_w = clamp_dim((double)f->custom_w);
		f->out_h = clamp_dim((double)f->custom_h);
	} else {
		double factor = f->factor < 1.0 ? 1.0 : f->factor;
		f->out_w = clamp_dim((double)w * factor);
		f->out_h = clamp_dim((double)h * factor);
	}

	f->valid = f->effect && f->easu_target;

	if (f->algo == ALGO_NIS) {
		if (!f->nis_effect) {
			if (!f->warned_no_nis) {
				blog(LOG_WARNING, "[obs-fsr1-filter] NIS nicht verfügbar, verwende FSR 1");
				f->warned_no_nis = true;
			}
		} else {
			/* NIS skaliert nur 1x bis 2x pro Achse (wie im Original). */
			double rx = (double)f->in_w / (double)f->out_w;
			double ry = (double)f->in_h / (double)f->out_h;
			bool in_range = rx >= 0.5 && rx <= 1.0 && ry >= 0.5 && ry <= 1.0;

			if (in_range) {
				f->active = ALGO_NIS;
				f->warned_nis_range = false;
			} else if (!f->warned_nis_range) {
				blog(LOG_WARNING,
				     "[obs-fsr1-filter] NIS unterstützt nur Skalierungen von 1x bis 2x "
				     "(%ux%u -> %ux%u), verwende FSR 1",
				     f->in_w, f->in_h, f->out_w, f->out_h);
				f->warned_nis_range = true;
			}
		}
	}

	if (f->algo == ALGO_RAVU) {
		if (!f->ravu_effect) {
			if (!f->warned_no_ravu) {
				blog(LOG_WARNING, "[obs-fsr1-filter] RAVU nicht verfügbar, verwende FSR 1");
				f->warned_no_ravu = true;
			}
		} else if (f->out_w > f->in_w && f->out_h > f->in_h) {
			/* RAVU-Zoom vergrößert in beliebigem Verhältnis, aber nur echte
			 * Vergrößerungen auf beiden Achsen (wie im Original). */
			f->active = ALGO_RAVU;
			f->warned_ravu_range = false;
		} else if (!f->warned_ravu_range) {
			blog(LOG_WARNING,
			     "[obs-fsr1-filter] RAVU vergrößert nur (%ux%u -> %ux%u), verwende FSR 1", f->in_w,
			     f->in_h, f->out_w, f->out_h);
			f->warned_ravu_range = true;
		}
	}

	if (f->algo == ALGO_SGSR) {
		if (!f->sgsr_effect) {
			if (!f->warned_no_sgsr) {
				blog(LOG_WARNING, "[obs-fsr1-filter] SGSR nicht verfügbar, verwende FSR 1");
				f->warned_no_sgsr = true;
			}
		} else if (f->out_w > f->in_w && f->out_h > f->in_h) {
			f->active = ALGO_SGSR;
			f->warned_sgsr_range = false;
		} else if (!f->warned_sgsr_range) {
			blog(LOG_WARNING,
			     "[obs-fsr1-filter] SGSR vergrößert nur (%ux%u -> %ux%u), verwende FSR 1", f->in_w,
			     f->in_h, f->out_w, f->out_h);
			f->warned_sgsr_range = true;
		}
	}

	if (f->algo == ALGO_FSRCNNX || f->algo == ALGO_FSRCNNX_HQ) {
		struct fsrcnnx_net *n = f->algo == ALGO_FSRCNNX ? &f->fsrcnnx : &f->fsrcnnx_hq;
		if (!n->tried) {
			obs_enter_graphics();
			load_fsrcnnx(n);
			obs_leave_graphics();
		}
		/* Das Netz rechnet auf 2x; das Zwischenbild muss in eine Textur passen. */
		bool fits = 2 * f->in_w <= MAX_OUTPUT_DIMENSION && 2 * f->in_h <= MAX_OUTPUT_DIMENSION;
		if (!n->effect) {
			if (!f->warned_no_fsrcnnx) {
				blog(LOG_WARNING, "[obs-fsr1-filter] FSRCNNX nicht verfügbar, verwende FSR 1");
				f->warned_no_fsrcnnx = true;
			}
		} else if (f->out_w > f->in_w && f->out_h > f->in_h && fits) {
			f->active = f->algo;
			f->warned_fsrcnnx_range = false;
		} else if (!f->warned_fsrcnnx_range) {
			blog(LOG_WARNING,
			     "[obs-fsr1-filter] FSRCNNX vergrößert nur und braucht eine Quelle bis %ux%u "
			     "(%ux%u -> %ux%u), verwende FSR 1",
			     MAX_OUTPUT_DIMENSION / 2, MAX_OUTPUT_DIMENSION / 2, f->in_w, f->in_h, f->out_w, f->out_h);
			f->warned_fsrcnnx_range = true;
		}
	}

	if (f->valid)
		log_state(f);
}

/* ---------------------------------------------------------------------- */
/* FSR 1: Pass 1 = EASU in ein Zwischenziel, Pass 2 = RCAS in die Ausgabe    */
/* ---------------------------------------------------------------------- */
static void render_fsr1(struct fsr1_filter *f)
{
	struct vec2 in_size;
	struct vec2 out_size;
	vec2_set(&in_size, (float)f->in_w, (float)f->in_h);
	vec2_set(&out_size, (float)f->out_w, (float)f->out_h);

	/* RCAS-Stärke: AMD gibt die Abschwächung in Blenden an (0 = maximal
	 * scharf, 2 = kaum). Die UI zeigt 0..100 % Schärfe. */
	double stops = 2.0 * (1.0 - f->sharpness / 100.0);
	if (stops < 0.0)
		stops = 0.0;
	if (stops > 2.0)
		stops = 2.0;
	float rcas_sharp = (float)exp2(-stops);

	/*
	 * Pass 1: EASU.
	 * Die Quelle wird (ohne OBS_SOURCE_SRGB-Flag also im Gamma-Raum) in
	 * eine Textur gerendert und mit dem EASU-Shader auf Zielgröße
	 * hochgerechnet. Das Ergebnis landet im eigenen Zwischenziel.
	 */
	if (!obs_source_process_filter_begin_with_color_space(f->context, GS_RGBA, GS_CS_SRGB,
							      OBS_NO_DIRECT_RENDERING))
		return; /* OBS hat den Filter bereits übersprungen */

	gs_texrender_reset(f->easu_target);

	bool easu_ok = false;
	if (gs_texrender_begin(f->easu_target, f->out_w, f->out_h)) {
		struct vec4 clear;
		vec4_zero(&clear);
		gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
		gs_ortho(0.0f, (float)f->out_w, 0.0f, (float)f->out_h, -100.0f, 100.0f);

		if (f->p_input_size)
			gs_effect_set_vec2(f->p_input_size, &in_size);
		if (f->p_output_size)
			gs_effect_set_vec2(f->p_output_size, &out_size);

		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

		obs_source_process_filter_tech_end(f->context, f->effect, f->out_w, f->out_h, "EASU");

		gs_blend_state_pop();
		gs_texrender_end(f->easu_target);
		easu_ok = true;
	}

	gs_texture_t *easu_tex = easu_ok ? gs_texrender_get_texture(f->easu_target) : NULL;
	if (!easu_tex)
		return;

	/*
	 * Pass 2: RCAS direkt in das Ausgabeziel des Filters.
	 */
	gs_technique_t *tech = gs_effect_get_technique(f->effect, "RCAS");
	if (!tech)
		return;

	const bool prev_srgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(false);

	if (f->p_image)
		gs_effect_set_texture(f->p_image, easu_tex);
	if (f->p_output_size)
		gs_effect_set_vec2(f->p_output_size, &out_size);
	if (f->p_rcas_sharp)
		gs_effect_set_float(f->p_rcas_sharp, rcas_sharp);
	if (f->p_rcas_denoise)
		gs_effect_set_float(f->p_rcas_denoise, f->denoise ? 1.0f : 0.0f);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

	size_t passes = gs_technique_begin(tech);
	for (size_t i = 0; i < passes; i++) {
		gs_technique_begin_pass(tech, i);
		gs_draw_sprite(easu_tex, 0, f->out_w, f->out_h);
		gs_technique_end_pass(tech);
	}
	gs_technique_end(tech);

	gs_blend_state_pop();
	gs_enable_framebuffer_srgb(prev_srgb);
}

/* ---------------------------------------------------------------------- */
/* NIS: ein einziger Pass (Skalierung und Schärfung sind enthalten)         */
/* ---------------------------------------------------------------------- */

/* Konstanten wie in NVIDIAs NVScalerUpdateConfig (SDR). sharpness: 0..1 */
static void nis_compute_params(float sharpness, struct vec4 *detect, struct vec4 *sharp1, struct vec4 *sharp2)
{
	if (sharpness < 0.0f)
		sharpness = 0.0f;
	if (sharpness > 1.0f)
		sharpness = 1.0f;

	const float slider = sharpness - 0.5f; /* 0..1 -> -0.5..+0.5 */

	const float max_scale = (slider >= 0.0f) ? 1.25f : 1.75f;
	const float min_scale = (slider >= 0.0f) ? 1.25f : 1.0f;
	const float limit_scale = (slider >= 0.0f) ? 1.25f : 1.0f;

	const float detect_ratio = 2.0f * 1127.0f / 1024.0f;
	const float detect_thres = 64.0f / 1024.0f;
	const float min_contrast_ratio = 2.0f;
	const float max_contrast_ratio = 10.0f;

	const float sharp_start_y = 0.45f;
	const float sharp_end_y = 0.9f;
	float strength_min = 0.4f + slider * min_scale * 1.2f;
	if (strength_min < 0.0f)
		strength_min = 0.0f;
	const float strength_max = 1.6f + slider * max_scale * 1.8f;
	float limit_min = 0.14f + slider * limit_scale * 0.32f;
	if (limit_min < 0.1f)
		limit_min = 0.1f;
	const float limit_max = 0.5f + slider * limit_scale * 0.6f;

	vec4_set(detect, detect_ratio, detect_thres, min_contrast_ratio, 1.0f / (max_contrast_ratio - min_contrast_ratio));
	vec4_set(sharp1, sharp_start_y, 1.0f / (sharp_end_y - sharp_start_y), strength_min,
		 strength_max - strength_min);
	vec4_set(sharp2, limit_min, limit_max - limit_min, 0.0f, 0.0f);
}

static void render_nis(struct fsr1_filter *f)
{
	struct vec2 in_size;
	struct vec2 out_size;
	struct vec4 detect;
	struct vec4 sharp1;
	struct vec4 sharp2;

	vec2_set(&in_size, (float)f->in_w, (float)f->in_h);
	vec2_set(&out_size, (float)f->out_w, (float)f->out_h);
	nis_compute_params((float)(f->nis_sharpness / 100.0), &detect, &sharp1, &sharp2);

	if (!obs_source_process_filter_begin_with_color_space(f->context, GS_RGBA, GS_CS_SRGB,
							      OBS_NO_DIRECT_RENDERING))
		return; /* OBS hat den Filter bereits übersprungen */

	if (f->n_input_size)
		gs_effect_set_vec2(f->n_input_size, &in_size);
	if (f->n_output_size)
		gs_effect_set_vec2(f->n_output_size, &out_size);
	if (f->n_coef)
		gs_effect_set_texture(f->n_coef, f->nis_coef_tex);
	if (f->n_detect)
		gs_effect_set_vec4(f->n_detect, &detect);
	if (f->n_sharp1)
		gs_effect_set_vec4(f->n_sharp1, &sharp1);
	if (f->n_sharp2)
		gs_effect_set_vec4(f->n_sharp2, &sharp2);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

	obs_source_process_filter_tech_end(f->context, f->nis_effect, f->out_w, f->out_h, "NIS");

	gs_blend_state_pop();
}

/* ---------------------------------------------------------------------- */
/* RAVU-Zoom-AR: ein Pass, Gewichte aus zwei linear gefilterten Tabellen    */
/* ---------------------------------------------------------------------- */
static void render_ravu(struct fsr1_filter *f)
{
	struct vec2 in_size;
	vec2_set(&in_size, (float)f->in_w, (float)f->in_h);

	if (!obs_source_process_filter_begin_with_color_space(f->context, GS_RGBA, GS_CS_SRGB,
							      OBS_NO_DIRECT_RENDERING))
		return; /* OBS hat den Filter bereits übersprungen */

	if (f->r_input_size)
		gs_effect_set_vec2(f->r_input_size, &in_size);
	if (f->r_lut)
		gs_effect_set_texture(f->r_lut, f->ravu_lut_tex);
	if (f->r_lut_ar)
		gs_effect_set_texture(f->r_lut_ar, f->ravu_lut_ar_tex);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

	obs_source_process_filter_tech_end(f->context, f->ravu_effect, f->out_w, f->out_h, "RAVU");

	gs_blend_state_pop();
}

/* ---------------------------------------------------------------------- */
/* SGSR: ein Pass                                                           */
/* ---------------------------------------------------------------------- */
static void render_sgsr(struct fsr1_filter *f)
{
	struct vec2 in_size;
	struct vec2 texel;
	vec2_set(&in_size, (float)f->in_w, (float)f->in_h);
	vec2_set(&texel, 1.0f / (float)f->in_w, 1.0f / (float)f->in_h);

	double s = f->sgsr_sharpness / 100.0;
	if (s < 0.0)
		s = 0.0;
	if (s > 1.0)
		s = 1.0;

	if (!obs_source_process_filter_begin_with_color_space(f->context, GS_RGBA, GS_CS_SRGB,
							      OBS_NO_DIRECT_RENDERING))
		return; /* OBS hat den Filter bereits übersprungen */

	if (f->g_input_size)
		gs_effect_set_vec2(f->g_input_size, &in_size);
	if (f->g_texel)
		gs_effect_set_vec2(f->g_texel, &texel);
	if (f->g_edge_threshold)
		gs_effect_set_float(f->g_edge_threshold, SGSR_EDGE_THRESHOLD);
	if (f->g_edge_sharpness)
		gs_effect_set_float(f->g_edge_sharpness, (float)(1.0 + s));

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

	obs_source_process_filter_tech_end(f->context, f->sgsr_effect, f->out_w, f->out_h, "SGSR");

	gs_blend_state_pop();
}

/* ---------------------------------------------------------------------- */
/* FSRCNNX: COPY, Rechen-Passes, AGG (2x), OUT (Zielgröße)                  */
/* ---------------------------------------------------------------------- */

/* Parameter, die jeder Pass braucht. gs_technique_end() leert ALLE Parameter eines Effects
 * (libobs effect.c), deshalb vor jedem Pass neu setzen. */
static void fsrcnnx_set_common(struct fsrcnnx_net *n, gs_texture_t *src, const struct vec2 *texel,
			       const struct vec2 *in_size, const struct vec2 *y2x_size)
{
	gs_effect_set_texture(n->p_image, src);
	gs_effect_set_vec2(n->p_texel, texel);
	gs_effect_set_vec2(n->p_input_size, in_size);
	gs_effect_set_vec2(n->p_y2x_size, y2x_size);
}

/* Ein Pass mit einer Technique des Netz-Effects in ein Zwischenziel (ohne Mischen:
 * die Zwischenbilder enthalten negative Werte, die erhalten bleiben müssen). */
static bool fsrcnnx_draw(struct fsrcnnx_net *n, const char *tech_name, gs_texrender_t *target, uint32_t w,
			 uint32_t h)
{
	gs_technique_t *tech = gs_effect_get_technique(n->effect, tech_name);
	if (!tech)
		return false;

	gs_texrender_reset(target);
	if (!gs_texrender_begin(target, w, h))
		return false;

	struct vec4 clear;
	vec4_zero(&clear);
	gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
	gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);

	size_t passes = gs_technique_begin(tech);
	for (size_t i = 0; i < passes; i++) {
		gs_technique_begin_pass(tech, i);
		gs_draw_sprite(NULL, 0, w, h);
		gs_technique_end_pass(tech);
	}
	gs_technique_end(tech);

	gs_texrender_end(target);
	return true;
}

static void render_fsrcnnx(struct fsr1_filter *f, struct fsrcnnx_net *n)
{
	struct vec2 in_size;
	struct vec2 texel;
	struct vec2 y2x_size;
	vec2_set(&in_size, (float)f->in_w, (float)f->in_h);
	vec2_set(&texel, 1.0f / (float)f->in_w, 1.0f / (float)f->in_h);
	vec2_set(&y2x_size, (float)(2 * f->in_w), (float)(2 * f->in_h));

	/* Quellbild in ein eigenes Ziel: die ersten Passes und OUT lesen es. */
	if (!obs_source_process_filter_begin_with_color_space(f->context, GS_RGBA, GS_CS_SRGB,
							      OBS_NO_DIRECT_RENDERING))
		return; /* OBS hat den Filter bereits übersprungen */

	gs_texrender_reset(n->src);
	bool src_ok = false;
	if (gs_texrender_begin(n->src, f->in_w, f->in_h)) {
		struct vec4 clear;
		vec4_zero(&clear);
		gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
		gs_ortho(0.0f, (float)f->in_w, 0.0f, (float)f->in_h, -100.0f, 100.0f);
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
		obs_source_process_filter_tech_end(f->context, n->effect, f->in_w, f->in_h, "COPY");
		gs_blend_state_pop();
		gs_texrender_end(n->src);
		src_ok = true;
	}
	gs_texture_t *src = src_ok ? gs_texrender_get_texture(n->src) : NULL;
	if (!src)
		return;

	const bool prev_srgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(false);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

	bool ok = true;
	for (int p = 0; ok && p < n->npasses; p++) {
		const struct fsrcnnx_pass *ps = &n->passes[p];
		fsrcnnx_set_common(n, src, &texel, &in_size, &y2x_size);
		for (int i = 0; i < ps->inputs; i++) {
			gs_texture_t *t = ps->in[i] < 0 ? src : gs_texrender_get_texture(n->slot[ps->in[i]]);
			if (!t || !n->p_t[i]) {
				ok = false;
				break;
			}
			gs_effect_set_texture(n->p_t[i], t);
		}
		ok = ok && fsrcnnx_draw(n, ps->tech, n->slot[ps->out], f->in_w, f->in_h);
	}

	gs_texture_t *sub = ok ? gs_texrender_get_texture(n->slot[n->agg_in]) : NULL;
	if (sub && n->p_t[0]) {
		fsrcnnx_set_common(n, src, &texel, &in_size, &y2x_size);
		gs_effect_set_texture(n->p_t[0], sub);
		ok = fsrcnnx_draw(n, "AGG", n->y2x, 2 * f->in_w, 2 * f->in_h);
	} else {
		ok = false;
	}
	gs_blend_state_pop();

	gs_texture_t *y2x = ok ? gs_texrender_get_texture(n->y2x) : NULL;
	gs_technique_t *tech = gs_effect_get_technique(n->effect, "OUT");
	if (y2x && tech) {
		/* OUT direkt in das Ausgabeziel des Filters */
		fsrcnnx_set_common(n, src, &texel, &in_size, &y2x_size);
		gs_effect_set_texture(n->p_y2x, y2x);

		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
		size_t passes = gs_technique_begin(tech);
		for (size_t i = 0; i < passes; i++) {
			gs_technique_begin_pass(tech, i);
			gs_draw_sprite(src, 0, f->out_w, f->out_h);
			gs_technique_end_pass(tech);
		}
		gs_technique_end(tech);
		gs_blend_state_pop();
	}
	gs_enable_framebuffer_srgb(prev_srgb);
}

static void fsr1_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct fsr1_filter *f = data;

	if (!f->valid) {
		obs_source_skip_video_filter(f->context);
		return;
	}

	switch (f->active) {
	case ALGO_NIS:
		render_nis(f);
		break;
	case ALGO_RAVU:
		render_ravu(f);
		break;
	case ALGO_SGSR:
		render_sgsr(f);
		break;
	case ALGO_FSRCNNX:
		render_fsrcnnx(f, &f->fsrcnnx);
		break;
	case ALGO_FSRCNNX_HQ:
		render_fsrcnnx(f, &f->fsrcnnx_hq);
		break;
	case ALGO_FSR1:
	default:
		render_fsr1(f);
		break;
	}
}

/* Sichtbarkeit der Einstellungen je nach Algorithmus und Größenmodus */
static void apply_visibility(obs_properties_t *props, obs_data_t *settings)
{
	const char *algo = obs_data_get_string(settings, S_ALGO);
	const bool nis = algo && strcmp(algo, V_ALGO_NIS) == 0;
	const bool sgsr = algo && strcmp(algo, V_ALGO_SGSR) == 0;
	/* RAVU und FSRCNNX haben keine eigenen Regler (die Schärfe steckt in den trainierten Gewichten). */
	const bool fsr = !algo || !*algo || strcmp(algo, V_ALGO_FSR1) == 0;
	const bool custom = obs_data_get_bool(settings, S_CUSTOM);

	obs_property_set_visible(obs_properties_get(props, S_FACTOR), !custom);
	obs_property_set_visible(obs_properties_get(props, S_WIDTH), custom);
	obs_property_set_visible(obs_properties_get(props, S_HEIGHT), custom);

	obs_property_set_visible(obs_properties_get(props, S_SHARPNESS), fsr);
	obs_property_set_visible(obs_properties_get(props, S_DENOISE), fsr);
	obs_property_set_visible(obs_properties_get(props, S_NIS_SHARPNESS), nis);
	obs_property_set_visible(obs_properties_get(props, S_SGSR_SHARPNESS), sgsr);
}

static bool settings_modified(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(p);
	apply_visibility(props, settings);
	return true;
}

static obs_properties_t *fsr1_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();
	obs_property_t *p;

	p = obs_properties_add_list(props, S_ALGO, T_ALGO, OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, T_ALGO_FSR1, V_ALGO_FSR1);
	obs_property_list_add_string(p, T_ALGO_NIS, V_ALGO_NIS);
	obs_property_list_add_string(p, T_ALGO_RAVU, V_ALGO_RAVU);
	obs_property_list_add_string(p, T_ALGO_SGSR, V_ALGO_SGSR);
	obs_property_list_add_string(p, T_ALGO_FSRCNNX, V_ALGO_FSRCNNX);
	obs_property_list_add_string(p, T_ALGO_FSRCNNX_HQ, V_ALGO_FSRCNNX_HQ);
	obs_property_set_modified_callback(p, settings_modified);

	p = obs_properties_add_bool(props, S_CUSTOM, T_CUSTOM);
	obs_property_set_modified_callback(p, settings_modified);

	obs_properties_add_float_slider(props, S_FACTOR, T_FACTOR, 1.0, 4.0, 0.05);
	obs_properties_add_int(props, S_WIDTH, T_WIDTH, 2, MAX_OUTPUT_DIMENSION, 1);
	obs_properties_add_int(props, S_HEIGHT, T_HEIGHT, 2, MAX_OUTPUT_DIMENSION, 1);

	obs_properties_add_float_slider(props, S_SHARPNESS, T_SHARPNESS, 0.0, 100.0, 1.0);
	obs_properties_add_bool(props, S_DENOISE, T_DENOISE);
	obs_properties_add_float_slider(props, S_NIS_SHARPNESS, T_NIS_SHARPNESS, 0.0, 100.0, 1.0);
	obs_properties_add_float_slider(props, S_SGSR_SHARPNESS, T_SGSR_SHARPNESS, 0.0, 100.0, 1.0);

	return props;
}

static void fsr1_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, S_ALGO, V_ALGO_FSR1);
	obs_data_set_default_double(settings, S_FACTOR, 1.5);
	obs_data_set_default_bool(settings, S_CUSTOM, false);
	obs_data_set_default_int(settings, S_WIDTH, 1920);
	obs_data_set_default_int(settings, S_HEIGHT, 1080);
	obs_data_set_default_double(settings, S_SHARPNESS, 80.0);
	/* Webcams rauschen: RCAS-Entrauschung standardmäßig an. */
	obs_data_set_default_bool(settings, S_DENOISE, true);
	obs_data_set_default_double(settings, S_NIS_SHARPNESS, 50.0);
	/* 100 % = Kantenschärfe 2,0 wie im Original */
	obs_data_set_default_double(settings, S_SGSR_SHARPNESS, 100.0);
}

static uint32_t fsr1_get_width(void *data)
{
	struct fsr1_filter *f = data;
	return f->out_w;
}

static uint32_t fsr1_get_height(void *data)
{
	struct fsr1_filter *f = data;
	return f->out_h;
}

/*
 * Bewusst ohne OBS_SOURCE_SRGB: OBS rendert den Filter dann im Gamma-Raum,
 * den alle Algorithmen erwarten (Eingabe und Ausgabe perceptual, nicht linear).
 */
static struct obs_source_info fsr1_filter_info = {
	.id = FILTER_ID,
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = fsr1_get_name,
	.create = fsr1_create,
	.destroy = fsr1_destroy,
	.update = fsr1_update,
	.video_tick = fsr1_tick,
	.video_render = fsr1_render,
	.get_properties = fsr1_properties,
	.get_defaults = fsr1_defaults,
	.get_width = fsr1_get_width,
	.get_height = fsr1_get_height,
};

bool obs_module_load(void)
{
	obs_register_source(&fsr1_filter_info);
	blog(LOG_INFO, "[obs-fsr1-filter] geladen");
	return true;
}

void obs_module_unload(void)
{
}

const char *obs_module_name(void)
{
	return "FSR 1 / NIS / RAVU / SGSR / FSRCNNX Upscale Filter";
}

const char *obs_module_description(void)
{
	return "Hochskalierungsfilter mit AMD FidelityFX Super Resolution 1.0, NVIDIA Image Scaling, RAVU-Zoom-AR, "
	       "Snapdragon Game Super Resolution und FSRCNNX";
}

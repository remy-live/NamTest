/* Banc d'essai de la MEMOIRE DES NOMS.
 *
 * Le nom d'un favori est un rang choisi dans une liste. Il doit survivre au
 * rechargement d'une pedalboard : le plugin le range dans son etat, le reprend,
 * puis DEMANDE a l'hote de le reposer dans le port de controle (extension kx).
 * Ce banc rejoue exactement cela, hors machine :
 *
 *   1. on choisit un nom pour chaque favori, on fait tourner le plugin ;
 *   2. on lui demande de sauvegarder, on garde ce qu'il ecrit ;
 *   3. on jette l'instance, on en cree une neuve, on lui rend l'etat ;
 *   4. on la fait tourner, et les ports doivent revenir a ce qui a ete choisi.
 *
 *   aarch64-linux-gnu-gcc -O1 -o etattest outils/etattest.c \
 *       -I deps/lv2/include -I src -ldl
 *   qemu-aarch64-static -L /usr/aarch64-linux-gnu ./etattest build/src/neural_amp_modeler.so
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <stdint.h>

#include "lv2/core/lv2.h"
#include "lv2/urid/urid.h"
#include "lv2/worker/worker.h"
#include "lv2/options/options.h"
#include "lv2/buf-size/buf-size.h"
#include "lv2/state/state.h"
#include "lv2/atom/atom.h"

#include "control-input-port-change-request.h"

#define N_PORTS 85
#define N_SAMPLES 256
#define IDX_FAV_NAME 56		/* fav_name_1, les dix se suivent */
#define N_FAVS 10

static char* uris[4096];
static uint32_t n_uris = 0;

static LV2_URID map_uri(LV2_URID_Map_Handle h, const char* uri)
{
	(void)h;
	for (uint32_t i = 0; i < n_uris; i++)
		if (!strcmp(uris[i], uri)) return i + 1;
	uris[n_uris] = strdup(uri);
	return ++n_uris;
}

static const LV2_Worker_Interface* worker_iface = NULL;
static LV2_Handle instance = NULL;

static LV2_Worker_Status respond(LV2_Worker_Respond_Handle h, uint32_t size, const void* data)
{
	(void)h;
	if (worker_iface && worker_iface->work_response)
		worker_iface->work_response(instance, size, data);
	return LV2_WORKER_SUCCESS;
}

static LV2_Worker_Status schedule(LV2_Worker_Schedule_Handle h, uint32_t size, const void* data)
{
	(void)h;
	if (worker_iface && worker_iface->work)
		worker_iface->work(instance, respond, NULL, size, data);
	return LV2_WORKER_SUCCESS;
}

/* --- l'etat garde par le faux hote --- */
#define MAX_ETAT 16
static struct { uint32_t key, type; size_t size; void* val; } etat[MAX_ETAT];
static int n_etat = 0;

static LV2_State_Status store_cb(LV2_State_Handle h, uint32_t key, const void* value,
	size_t size, uint32_t type, uint32_t flags)
{
	(void)h; (void)flags;
	if (n_etat >= MAX_ETAT) return LV2_STATE_ERR_UNKNOWN;
	etat[n_etat].key = key;
	etat[n_etat].type = type;
	etat[n_etat].size = size;
	etat[n_etat].val = malloc(size);
	memcpy(etat[n_etat].val, value, size);
	n_etat++;
	return LV2_STATE_SUCCESS;
}

static const void* retrieve_cb(LV2_State_Handle h, uint32_t key, size_t* size,
	uint32_t* type, uint32_t* flags)
{
	(void)h;
	for (int i = 0; i < n_etat; i++)
	{
		if (etat[i].key != key) continue;
		if (size) *size = etat[i].size;
		if (type) *type = etat[i].type;
		if (flags) *flags = 0;
		return etat[i].val;
	}
	if (size) *size = 0;
	if (type) *type = 0;
	return NULL;
}

/* --- extension kx : l'hote accepte et pose la valeur dans le port --- */
static float* ports_ctrl = NULL;
static int kx_appels = 0;

static LV2_ControlInputPort_Change_Status kx_change(
	LV2_ControlInputPort_Change_Request_Handle h, uint32_t index, float value)
{
	(void)h;
	if (index >= N_PORTS) return LV2_CONTROL_INPUT_PORT_CHANGE_ERR_INVALID_INDEX;
	ports_ctrl[index] = value;
	kx_appels++;
	return LV2_CONTROL_INPUT_PORT_CHANGE_SUCCESS;
}

static float audio_in[N_SAMPLES], audio_out[N_SAMPLES];
static float ctrl[N_PORTS];
static uint8_t atom_in[8192], atom_out[8192];

static const LV2_Descriptor* d = NULL;

static void brancher(void)
{
	for (uint32_t p = 0; p < N_PORTS; p++)
	{
		if (p == 0)      d->connect_port(instance, p, atom_in);
		else if (p == 1) d->connect_port(instance, p, atom_out);
		else if (p == 2) d->connect_port(instance, p, audio_in);
		else if (p == 3) d->connect_port(instance, p, audio_out);
		else             d->connect_port(instance, p, &ctrl[p]);
	}
}

static void tourner(int fois)
{
	for (int i = 0; i < fois; i++)
	{
		*(uint32_t*)atom_out = sizeof(atom_out) - 8;
		d->run(instance, N_SAMPLES);
	}
}

int main(int argc, char** argv)
{
	if (argc < 2) { fprintf(stderr, "usage: %s plugin.so\n", argv[0]); return 2; }

	void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
	if (!lib) { fprintf(stderr, "dlopen : %s\n", dlerror()); return 1; }

	LV2_Descriptor_Function df = (LV2_Descriptor_Function)dlsym(lib, "lv2_descriptor");
	if (!df) { fprintf(stderr, "lv2_descriptor introuvable\n"); return 1; }

	d = df(0);
	if (!d) { fprintf(stderr, "descripteur nul\n"); return 1; }

	LV2_URID_Map map = { NULL, map_uri };
	LV2_Feature f_map = { LV2_URID__map, &map };
	LV2_Worker_Schedule sched = { NULL, schedule };
	LV2_Feature f_sched = { LV2_WORKER__schedule, &sched };

	LV2_ControlInputPort_Change_Request kx = { NULL, kx_change };
	LV2_Feature f_kx = { LV2_CONTROL_INPUT_PORT_CHANGE_REQUEST_URI, &kx };

	LV2_Options_Option opts[2];
	memset(opts, 0, sizeof(opts));
	int32_t bufsize = N_SAMPLES;
	opts[0].context = LV2_OPTIONS_INSTANCE;
	opts[0].key = map_uri(NULL, LV2_BUF_SIZE__maxBlockLength);
	opts[0].size = sizeof(int32_t);
	opts[0].type = map_uri(NULL, LV2_ATOM__Int);
	opts[0].value = &bufsize;
	LV2_Feature f_opts = { LV2_OPTIONS__options, opts };

	const LV2_Feature* features[] = { &f_map, &f_sched, &f_opts, &f_kx, NULL };

	ports_ctrl = ctrl;

	memset(atom_in, 0, sizeof(atom_in));
	memset(atom_out, 0, sizeof(atom_out));

	/* ---- 1. une premiere instance, on choisit les noms ---------------- */
	instance = d->instantiate(d, 48000.0, "/tmp/", features);
	if (!instance) { fprintf(stderr, "instantiate a renvoye NULL\n"); return 1; }

	worker_iface = (const LV2_Worker_Interface*)d->extension_data(LV2_WORKER__interface);
	const LV2_State_Interface* st =
		(const LV2_State_Interface*)d->extension_data(LV2_STATE__interface);

	if (!st) { fprintf(stderr, "pas d'interface state\n"); return 1; }

	brancher();
	if (d->activate) d->activate(instance);

	int choisis[N_FAVS];

	for (int i = 0; i < N_FAVS; i++)
	{
		choisis[i] = (i + 1) * 7;	/* 7, 14, 21 ... des rangs quelconques */
		ctrl[IDX_FAV_NAME + i] = (float)choisis[i];
	}

	tourner(5);

	/* ---- 2. sauvegarde ------------------------------------------------ */
	st->save(instance, store_cb, NULL, 0, NULL);

	printf("etat ecrit : %d cles\n", n_etat);

	int vu_noms = 0;

	for (int i = 0; i < n_etat; i++)
	{
		const char* nom = uris[etat[i].key - 1];
		printf("  %s = \"%s\"\n", nom, (const char*)etat[i].val);

		if (strstr(nom, "#favnames") != NULL)
		{
			vu_noms = 1;

			char attendu[256];
			int n = 0;
			for (int k = 0; k < N_FAVS; k++)
				n += snprintf(attendu + n, sizeof(attendu) - n, k ? ",%d" : "%d", choisis[k]);

			if (strcmp((const char*)etat[i].val, attendu) != 0)
			{
				fprintf(stderr, "FAUTE : etat \"%s\", attendu \"%s\"\n",
					(const char*)etat[i].val, attendu);
				return 1;
			}
		}
	}

	if (!vu_noms) { fprintf(stderr, "FAUTE : aucune cle #favnames dans l'etat\n"); return 1; }

	if (d->deactivate) d->deactivate(instance);
	d->cleanup(instance);

	/* ---- 3. une instance NEUVE, ports a zero, on lui rend l'etat ------ */
	memset(ctrl, 0, sizeof(ctrl));
	kx_appels = 0;

	instance = d->instantiate(d, 48000.0, "/tmp/", features);
	if (!instance) { fprintf(stderr, "seconde instantiate nulle\n"); return 1; }

	worker_iface = (const LV2_Worker_Interface*)d->extension_data(LV2_WORKER__interface);

	brancher();
	if (d->activate) d->activate(instance);

	st->restore(instance, retrieve_cb, NULL, 0, NULL);

	/* ---- 4. les ports doivent revenir a ce qui a ete choisi ----------- */
	tourner(5);

	printf("demandes kx : %d\n", kx_appels);

	int fautes = 0;

	for (int i = 0; i < N_FAVS; i++)
	{
		const int lu = (int)ctrl[IDX_FAV_NAME + i];

		printf("  fav %-2d attendu %-3d lu %-3d %s\n", i + 1, choisis[i], lu,
			lu == choisis[i] ? "" : "<-- FAUTE");

		if (lu != choisis[i]) fautes++;
	}

	if (d->deactivate) d->deactivate(instance);
	d->cleanup(instance);
	dlclose(lib);

	if (fautes) { fprintf(stderr, "%d nom(s) perdu(s) au rechargement\n", fautes); return 1; }

	printf("LES NOMS SURVIVENT AU RECHARGEMENT\n");
	return 0;
}

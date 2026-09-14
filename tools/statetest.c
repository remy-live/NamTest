/* Test bench for the NAME MEMORY.
 *
 * A favorite's name is a rank picked from a list. It has to survive a
 * pedalboard reload: the plugin puts it into its state, takes it back, then
 * ASKS the host to put it back into the control port (the kx extension). This
 * bench replays exactly that, off the machine:
 *
 *   1. pick a name for every favorite, run the plugin;
 *   2. ask it to save, keep whatever it writes;
 *   3. throw the instance away, make a fresh one, hand it the state back;
 *   4. run it, and the ports must come back to what was picked.
 *
 *   aarch64-linux-gnu-gcc -O1 -o statetest outils/etattest.c \
 *       -I deps/lv2/include -I src -ldl
 *   qemu-aarch64-static -L /usr/aarch64-linux-gnu ./statetest build/src/neural_amp_modeler.so
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
#define IDX_FAV_NAME 56		/* fav_name_1, the ten follow on */
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

/* --- the state kept by the fake host --- */
#define MAX_ETAT 16
static struct { uint32_t key, type; size_t size; void* val; } state[MAX_ETAT];
static int n_state = 0;

static LV2_State_Status store_cb(LV2_State_Handle h, uint32_t key, const void* value,
	size_t size, uint32_t type, uint32_t flags)
{
	(void)h; (void)flags;
	if (n_state >= MAX_ETAT) return LV2_STATE_ERR_UNKNOWN;
	state[n_state].key = key;
	state[n_state].type = type;
	state[n_state].size = size;
	state[n_state].val = malloc(size);
	memcpy(state[n_state].val, value, size);
	n_state++;
	return LV2_STATE_SUCCESS;
}

static const void* retrieve_cb(LV2_State_Handle h, uint32_t key, size_t* size,
	uint32_t* type, uint32_t* flags)
{
	(void)h;
	for (int i = 0; i < n_state; i++)
	{
		if (state[i].key != key) continue;
		if (size) *size = state[i].size;
		if (type) *type = state[i].type;
		if (flags) *flags = 0;
		return state[i].val;
	}
	if (size) *size = 0;
	if (type) *type = 0;
	return NULL;
}

/* --- kx extension: the host accepts and puts the value into the port --- */
static float* ports_ctrl = NULL;
static int kx_calls = 0;

static LV2_ControlInputPort_Change_Status kx_change(
	LV2_ControlInputPort_Change_Request_Handle h, uint32_t index, float value)
{
	(void)h;
	if (index >= N_PORTS) return LV2_CONTROL_INPUT_PORT_CHANGE_ERR_INVALID_INDEX;
	ports_ctrl[index] = value;
	kx_calls++;
	return LV2_CONTROL_INPUT_PORT_CHANGE_SUCCESS;
}

static float audio_in[N_SAMPLES], audio_out[N_SAMPLES];
static float ctrl[N_PORTS];
static uint8_t atom_in[8192], atom_out[8192];

static const LV2_Descriptor* d = NULL;

static void connect_all(void)
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

static void run_cycles(int times)
{
	for (int i = 0; i < times; i++)
	{
		*(uint32_t*)atom_out = sizeof(atom_out) - 8;
		d->run(instance, N_SAMPLES);
	}
}

int main(int argc, char** argv)
{
	if (argc < 2) { fprintf(stderr, "usage: %s plugin.so\n", argv[0]); return 2; }

	void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
	if (!lib) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }

	LV2_Descriptor_Function df = (LV2_Descriptor_Function)dlsym(lib, "lv2_descriptor");
	if (!df) { fprintf(stderr, "lv2_descriptor not found\n"); return 1; }

	d = df(0);
	if (!d) { fprintf(stderr, "null descriptor\n"); return 1; }

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

	/* ---- 1. a first instance, pick the names ------------------------- */
	instance = d->instantiate(d, 48000.0, "/tmp/", features);
	if (!instance) { fprintf(stderr, "instantiate returned NULL\n"); return 1; }

	worker_iface = (const LV2_Worker_Interface*)d->extension_data(LV2_WORKER__interface);
	const LV2_State_Interface* st =
		(const LV2_State_Interface*)d->extension_data(LV2_STATE__interface);

	if (!st) { fprintf(stderr, "no state interface\n"); return 1; }

	connect_all();
	if (d->activate) d->activate(instance);

	int picked[N_FAVS];

	for (int i = 0; i < N_FAVS; i++)
	{
		picked[i] = (i + 1) * 7;	/* 7, 14, 21 ... arbitrary ranks */
		ctrl[IDX_FAV_NAME + i] = (float)picked[i];
	}

	run_cycles(5);

	/* ---- 2. save ------------------------------------------------------ */
	st->save(instance, store_cb, NULL, 0, NULL);

	printf("state written: %d keys\n", n_state);

	int saw_names = 0;

	for (int i = 0; i < n_state; i++)
	{
		const char* name = uris[state[i].key - 1];
		printf("  %s = \"%s\"\n", name, (const char*)state[i].val);

		if (strstr(name, "#favnames") != NULL)
		{
			saw_names = 1;

			char expected[256];
			int n = 0;
			for (int k = 0; k < N_FAVS; k++)
				n += snprintf(expected + n, sizeof(expected) - n, k ? ",%d" : "%d", picked[k]);

			if (strcmp((const char*)state[i].val, expected) != 0)
			{
				fprintf(stderr, "FAULT: state \"%s\", expected \"%s\"\n",
					(const char*)state[i].val, expected);
				return 1;
			}
		}
	}

	if (!saw_names) { fprintf(stderr, "FAULT: no #favnames key in the state\n"); return 1; }

	if (d->deactivate) d->deactivate(instance);
	d->cleanup(instance);

	/* ---- 3. a FRESH instance, ports at zero, hand the state back ----- */
	memset(ctrl, 0, sizeof(ctrl));
	kx_calls = 0;

	instance = d->instantiate(d, 48000.0, "/tmp/", features);
	if (!instance) { fprintf(stderr, "second instantiate returned NULL\n"); return 1; }

	worker_iface = (const LV2_Worker_Interface*)d->extension_data(LV2_WORKER__interface);

	connect_all();
	if (d->activate) d->activate(instance);

	st->restore(instance, retrieve_cb, NULL, 0, NULL);

	/* ---- 4. the ports must come back to what was picked -------------- */
	run_cycles(5);

	printf("kx requests: %d\n", kx_calls);

	int faults = 0;

	for (int i = 0; i < N_FAVS; i++)
	{
		const int read = (int)ctrl[IDX_FAV_NAME + i];

		printf("  fav %-2d expected %-3d read %-3d %s\n", i + 1, picked[i], read,
			read == picked[i] ? "" : "<-- FAULT");

		if (read != picked[i]) faults++;
	}

	if (d->deactivate) d->deactivate(instance);
	d->cleanup(instance);
	dlclose(lib);

	if (faults) { fprintf(stderr, "%d name(s) lost on reload\n", faults); return 1; }

	printf("THE NAMES SURVIVE A RELOAD\n");
	return 0;
}

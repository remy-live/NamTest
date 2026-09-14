/* Off-machine test bench: loads the plugin, instantiates it with ALL of its
 * ports connected, runs it, and reports whatever falls over.
 *
 * The point: reproduce here the SEGV that kills jack2 on the Dwarf, rather
 * than having one more variant installed for every hypothesis.
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

/* every port of the descriptor: NUM_PORTS_TOTAL in src/nam_plugin.h */
#define N_PORTS 85
#define N_SAMPLES 256

/* --- a minimal urid map --- */
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

/* --- worker: the work runs STRAIGHT AWAY, like a simple host would --- */
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

int main(int argc, char** argv)
{
	if (argc < 2) { fprintf(stderr, "usage: %s plugin.so\n", argv[0]); return 2; }

	void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
	if (!lib) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }

	LV2_Descriptor_Function df = (LV2_Descriptor_Function)dlsym(lib, "lv2_descriptor");
	if (!df) { fprintf(stderr, "lv2_descriptor not found\n"); return 1; }

	const LV2_Descriptor* d = df(0);
	if (!d) { fprintf(stderr, "null descriptor\n"); return 1; }
	printf("URI    %s\n", d->URI);

	LV2_URID_Map map = { NULL, map_uri };
	LV2_Feature f_map = { LV2_URID__map, &map };
	LV2_Worker_Schedule sched = { NULL, schedule };
	LV2_Feature f_sched = { LV2_WORKER__schedule, &sched };

	LV2_Options_Option opts[2];
	memset(opts, 0, sizeof(opts));
	int32_t bufsize = N_SAMPLES;
	opts[0].context = LV2_OPTIONS_INSTANCE;
	opts[0].key = map_uri(NULL, LV2_BUF_SIZE__maxBlockLength);
	opts[0].size = sizeof(int32_t);
	opts[0].type = map_uri(NULL, "http://lv2plug.in/ns/ext/atom#Int");
	opts[0].value = &bufsize;
	LV2_Feature f_opts = { LV2_OPTIONS__options, opts };

	const LV2_Feature* features[] = { &f_map, &f_sched, &f_opts, NULL };

	printf("instantiate...\n"); fflush(stdout);
	instance = d->instantiate(d, 48000.0, "/tmp/", features);
	if (!instance) { fprintf(stderr, "instantiate returned NULL\n"); return 1; }

	if (d->extension_data)
		worker_iface = (const LV2_Worker_Interface*)d->extension_data(LV2_WORKER__interface);
	printf("worker: %s\n", worker_iface ? "present" : "absent");

	/* buffers: audio, atoms, and one float per control port */
	static float audio_in[N_SAMPLES], audio_out[N_SAMPLES];
	static float ctrl[N_PORTS];
	static uint8_t atom_in[8192], atom_out[8192];

	memset(atom_in, 0, sizeof(atom_in));
	memset(atom_out, 0, sizeof(atom_out));

	printf("connect_port x%d...\n", N_PORTS); fflush(stdout);
	for (uint32_t p = 0; p < N_PORTS; p++)
	{
		if (p == 0)      d->connect_port(instance, p, atom_in);
		else if (p == 1) d->connect_port(instance, p, atom_out);
		else if (p == 2) d->connect_port(instance, p, audio_in);
		else if (p == 3) d->connect_port(instance, p, audio_out);
		else             d->connect_port(instance, p, &ctrl[p]);
	}

	if (d->activate) d->activate(instance);

	printf("run x20...\n"); fflush(stdout);
	for (int i = 0; i < 20; i++)
	{
		/* the host resets the output sequence on every cycle */
		*(uint32_t*)atom_out = sizeof(atom_out) - 8;
		d->run(instance, N_SAMPLES);
	}

	printf("pressing every control...\n"); fflush(stdout);
	for (uint32_t p = 7; p < N_PORTS; p++)
	{
		ctrl[p] = 1.0f;
		*(uint32_t*)atom_out = sizeof(atom_out) - 8;
		d->run(instance, N_SAMPLES);
		ctrl[p] = 0.0f;
		*(uint32_t*)atom_out = sizeof(atom_out) - 8;
		d->run(instance, N_SAMPLES);
	}

	if (d->deactivate) d->deactivate(instance);
	printf("cleanup...\n"); fflush(stdout);
	d->cleanup(instance);
	dlclose(lib);
	printf("FINISHED WITHOUT CRASHING\n");
	return 0;
}

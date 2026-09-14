#include <algorithm>
#include <cmath>
#include <utility>
#include <cassert>

#include "nam_plugin.h"

#define _DEFAULT_SOURCE
#include <dirent.h>
#include <algorithm>
#include <cstring>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <functional>

#define SMOOTH_EPSILON .0001f

#ifndef BYPASS_DB_THRESHOLD
#define BYPASS_DB_THRESHOLD -100
#endif

namespace NAM {
	Plugin::Plugin()
	{
		// prevent allocations on the audio thread
		currentModelPath.reserve(MAX_FILE_NAME + 1);

		bypassThresholdLinear = powf(10, BYPASS_DB_THRESHOLD * 0.05f);

//		NeuralAudio::NeuralModel::SetLSTMLoadMode(
//#ifdef LSTM_PREFER_NAM
//			NeuralAudio::PreferNAMCore
//#else
//			NeuralAudio::PreferRTNeural
//#endif
//		);
//
//		NeuralAudio::NeuralModel::SetWaveNetLoadMode(
//#ifdef WAVENET_PREFER_NAM
//			NeuralAudio::PreferNAMCore
//#else
//			NeuralAudio::PreferRTNeural
//#endif
		//);
	}

	Plugin::~Plugin()
	{
		delete currentModel;
	}

	bool Plugin::initialize(double sampleRate, const LV2_Feature* const* features) noexcept
	{
		this->sampleRate = sampleRate;

		for (int i = 0; i < NUM_FAVS; i++)
			favIndexes[i].store(-1, std::memory_order_release);

		for (int i = 0; i < NUM_FAVS; i++)
			favNameChoice[i].store(0, std::memory_order_release);

		// announce everything on the first pass: without this the host knows
		// nothing of the paths and will not write them into the pedalboard
		favDirty.store((1u << NUM_FAVS) - 1, std::memory_order_release);

		// long-press threshold, turned into samples once and for all
		longPressSamples = (uint32_t)(sampleRate * LONG_PRESS_MS / 1000.0);
		longPressMaxSamples = (uint32_t)(sampleRate * LONG_PRESS_MAX_MS / 1000.0);
		screenPeriod = (uint32_t)(sampleRate * 0.25);	// 4 refreshes per second
		popupHold = (uint64_t)(sampleRate * 3.0);	// 3 s of silence during a full screen
		tracePeriod = (uint32_t)(sampleRate * 2.0);	// state written every 2 s

		loader.SetExternalSampleRate((int)sampleRate);

		// for fetching initial options, can be null
		LV2_Options_Option* options = nullptr;

		for (size_t i = 0; features[i]; ++i)
		{
			if (std::string(features[i]->URI) == std::string(LV2_URID__map))
				map = static_cast<LV2_URID_Map*>(features[i]->data);

			if (std::string(features[i]->URI)
				== std::string(LV2_CONTROL_INPUT_PORT_CHANGE_REQUEST_URI))
			{
				portreq = static_cast<const LV2_ControlInputPort_Change_Request*>(
					features[i]->data);

				// 1 right from init: otherwise 0 conflates "the host does not
				// allow it" with "we never asked"
				kxState = (portreq != nullptr && portreq->request_change != nullptr) ? 1 : 0;
			}

			if (std::string(features[i]->URI) == std::string(LV2_HMI__WidgetControl))
			{
				hmi = static_cast<const LV2_HMI_WidgetControl*>(features[i]->data);
				hmiSize = hmi != nullptr ? hmi->size : 0;
			}
			else if (std::string(features[i]->URI) == std::string(LV2_WORKER__schedule))
				schedule = static_cast<LV2_Worker_Schedule*>(features[i]->data);
			else if (std::string(features[i]->URI) == std::string(LV2_LOG__log))
				logger.log = static_cast<LV2_Log_Log*>(features[i]->data);
			else if (std::string(features[i]->URI) == std::string(LV2_OPTIONS__options))
				options = static_cast<LV2_Options_Option*>(features[i]->data);
		}
	
		lv2_log_logger_set_map(&logger, map);

		if (!map)
		{
			lv2_log_error(&logger, "Missing required feature: `%s`", LV2_URID__map);

			return false;
		}

		if (!schedule)
		{
			lv2_log_error(&logger, "Missing required feature: `%s`", LV2_WORKER__schedule);

			return false;
		}

		lv2_atom_forge_init(&atom_forge, map);

		uris.atom_Object = map->map(map->handle, LV2_ATOM__Object);
		uris.atom_Float = map->map(map->handle, LV2_ATOM__Float);
		uris.atom_Int = map->map(map->handle, LV2_ATOM__Int);
		uris.atom_Path = map->map(map->handle, LV2_ATOM__Path);
		uris.atom_URID = map->map(map->handle, LV2_ATOM__URID);
		uris.bufSize_maxBlockLength = map->map(map->handle, LV2_BUF_SIZE__maxBlockLength);
		uris.patch_Set = map->map(map->handle, LV2_PATCH__Set);
		uris.patch_Put = map->map(map->handle, LV2_PATCH__Put);
		uris.patch_body = map->map(map->handle, LV2_PATCH__body);
		uris.patch_Get = map->map(map->handle, LV2_PATCH__Get);
		uris.patch_property = map->map(map->handle, LV2_PATCH__property);
		uris.patch_value = map->map(map->handle, LV2_PATCH__value);
		uris.units_frame = map->map(map->handle, LV2_UNITS__frame);

		uris.model_Path = map->map(map->handle, MODEL_URI);
		uris.atom_String = map->map(map->handle, LV2_ATOM__String);
		uris.favs_String = map->map(map->handle, PlUGIN_URI "#favorites");
		// Rank picked in the list for each favorite. A key SEPARATE from the
		// paths: an older state has none, and what it holds stays readable.
		uris.favNames_String = map->map(map->handle, PlUGIN_URI "#favnames");

		{
			// A TRAP PAID FOR DEARLY: this table was written by hand with EIGHT
			// entries when NUM_FAVS moved to TEN. The last two were nullptr,
			// the mapping function dereferenced them, and the audio server
			// died as soon as the effect was added.
			// It is built from NUM_FAVS now, so it can no longer drift out of
			// step.
			static const char* favUris[NUM_FAVS] = {
				PlUGIN_URI "#fav1", PlUGIN_URI "#fav2", PlUGIN_URI "#fav3",
				PlUGIN_URI "#fav4", PlUGIN_URI "#fav5", PlUGIN_URI "#fav6",
				PlUGIN_URI "#fav7", PlUGIN_URI "#fav8", PlUGIN_URI "#fav9",
				PlUGIN_URI "#fav10"
			};

			static_assert(sizeof(favUris) / sizeof(favUris[0]) == NUM_FAVS,
				"the favorite URI table must hold exactly NUM_FAVS entries");

			for (int i = 0; i < NUM_FAVS; i++)
			{
				// guard rail: never pass a null pointer to the URI mapper
				uris.fav_Path[i] = (favUris[i] != nullptr)
					? map->map(map->handle, favUris[i]) : 0;
			}
		}

		if (options != nullptr)
			options_set(this, options);

		return true;
	}

	// runs on non-RT, can block or use [de]allocations

	// Walks MODEL_DIR two levels deep and keeps the model files.
	// Runs on the WORKER thread, never on the audio thread.
	// d_type rather than stat(): stat only exists as a symbol from
	// GLIBC_2.33 on, while the machine caps at 2.27.
	void Plugin::scan_models()
	{
		static const char* exts[] = { ".nam", ".nammodel", ".aidax", ".aidadspmodel", ".json" };

		modelFiles.clear();

		auto has_ext = [](const char* name) -> bool {
			const char* dot = strrchr(name, '.');
			if (dot == nullptr)
				return false;
			for (const char* e : exts)
			{
				if (strcasecmp(dot, e) == 0)
					return true;
			}
			return false;
		};

		// std::function so it can recurse: an auto lambda cannot call
		// itself (its own type is not deduced yet)
		std::function<void(const std::string&, bool)> scan_dir =
			[&](const std::string& dir, bool recurse)
		{
			DIR* d = opendir(dir.c_str());
			if (d == nullptr)
				return;

			struct dirent* ent;
			while ((ent = readdir(d)) != nullptr)
			{
				if (ent->d_name[0] == '.')
					continue;
				if ((int)modelFiles.size() >= MAX_MODELS)
					break;

				std::string full = dir + "/" + ent->d_name;
				bool isDir = (ent->d_type == DT_DIR);

				if (ent->d_type == DT_UNKNOWN)
				{
					// a file system that does not fill in d_type: a trial
					// opendir settles it, without calling stat.
					DIR* t = opendir(full.c_str());
					isDir = (t != nullptr);
					if (t != nullptr)
						closedir(t);
				}

				if (isDir)
				{
					if (recurse)
						scan_dir(full, false);
				}
				else if (has_ext(ent->d_name))
				{
					modelFiles.push_back(full);
				}
			}

			closedir(d);
		};

		scan_dir(MODEL_DIR, true);

		std::sort(modelFiles.begin(), modelFiles.end());

		// Find the rank of the already loaded model, if it is in the list
		int found = -1;
		for (size_t i = 0; i < modelFiles.size(); i++)
		{
			if (modelFiles[i] == currentModelPath)
			{
				found = (int)i;
				break;
			}
		}

		selectedIndex.store(found, std::memory_order_release);

		// each favorite's rank, recomputed from the PATH: adding or removing
		// other models therefore breaks no favorite
		for (int f = 0; f < NUM_FAVS; f++)
		{
			int rank = -1;

			if (favPaths[f][0] != '\0')
			{
				for (size_t i = 0; i < modelFiles.size(); i++)
				{
					if (modelFiles[i] == favPaths[f])
					{
						rank = (int)i;
						break;
					}
				}
			}

			favIndexes[f].store(rank, std::memory_order_release);
		}

		modelCount.store((int)modelFiles.size(), std::memory_order_release);
	}

	LV2_Worker_Status Plugin::work(LV2_Handle instance, LV2_Worker_Respond_Function respond, LV2_Worker_Respond_Handle handle,
		uint32_t size, const void* data)
	{
		switch (*(const LV2WorkType*)data)
		{
			case kWorkTypeLoad:
			{
				auto msg = static_cast<const LV2LoadModelMsg*>(data);
				auto nam = static_cast<NAM::Plugin*>(instance);

				NeuralAudio::NeuralModel* model = nullptr;
				LV2SwitchModelMsg response = { kWorkTypeSwitch, {}, {} };
				LV2_Worker_Status result = LV2_WORKER_SUCCESS;

				try
				{
					// load model from path
					const size_t pathlen = strlen(msg->path);

					if (pathlen == 0 || pathlen >= MAX_FILE_NAME)
					{
						// avoid logging an error on an empty path.
						// but do clear the model.
						model = nullptr;
					}
					else
					{
						lv2_log_trace(&nam->logger, "Staging model change: `%s`\n", msg->path);

						model = nam->loader.CreateFromFile(msg->path);
					}

					if (model != nullptr)
					{
						response.model = model;

						memcpy(response.path, msg->path, pathlen);
					}
				}
				catch (const std::exception&)
				{
				}

				if (model == nullptr)
				{
					response.path[0] = '\0';

					lv2_log_error(&nam->logger, "Unable to load model from: '%s'\n", msg->path);

					// logging does not come out on the Dwarf: the failure is
					// made visible through an output port instead
					nam->loadStatus.store(-1, std::memory_order_release);
					nam->pendingIndex.store(-1, std::memory_order_release);
				}

				respond(handle, sizeof(response), &response);

				return result;
			}

			case kWorkTypeAutoGain:
			{
				static_cast<NAM::Plugin*>(instance)->measure_levels();

				return LV2_WORKER_SUCCESS;
			}

			case kWorkTypeTrace:
			{
				auto nam = static_cast<NAM::Plugin*>(instance);

				FILE* f = fopen(TRACE_PATH, "w");

				if (f == nullptr)
					return LV2_WORKER_SUCCESS;

				fprintf(f, "build      %s\n", nam_build_tag());
				fprintf(f, "hmi        %s\n", nam->hmi ? "yes" : "NO");

				if (nam->hmi != nullptr)
				{
					fprintf(f, "hmi.size   %lu (popup threshold %lu)\n",
						(unsigned long)nam->hmi->size,
						(unsigned long)LV2_HMI_WIDGETCONTROL_SIZE_POPUP_MESSAGE);
					fprintf(f, "set_label  %s\n", nam->hmi->set_label ? "present" : "ABSENT");
					fprintf(f, "set_value  %s\n", nam->hmi->set_value ? "present" : "ABSENT");
					fprintf(f, "popup_msg  %s\n", nam->hmi->popup_message ? "present" : "ABSENT");
				}

				fprintf(f, "\naddressed ports (index: caps momentary)\n");

				bool none = true;
				for (int k = 0; k < NUM_PORTS_TOTAL; k++)
				{
					if (nam->hmiAddr[k] == nullptr)
						continue;

					none = false;
					fprintf(f, "  port %-3d caps=%d%s%s%s%s%s momentary=%s\n", k,
						nam->hmiCaps[k],
						(nam->hmiCaps[k] & LV2_HMI_AddressingCapability_LED) ? " led" : "",
						(nam->hmiCaps[k] & LV2_HMI_AddressingCapability_Label) ? " LABEL" : "",
						(nam->hmiCaps[k] & LV2_HMI_AddressingCapability_Value) ? " value" : "",
						(nam->hmiCaps[k] & LV2_HMI_AddressingCapability_Unit) ? " unit" : "",
						(nam->hmiCaps[k] & LV2_HMI_AddressingCapability_Indicator) ? " bar" : "",
						nam->hmiMomentary[k] ? "yes" : "no");
				}

				if (none)
					fprintf(f, "  NONE -- redo the addressing after installing\n");

				fprintf(f, "\npatch:Put received    %d\n", nam->putsSeen.load(std::memory_order_acquire));
				fprintf(f, "\nkx (writing the knobs) : %d  (0 absent, 1 available, 2 accepted, -1 refused)\n",
					nam->kxState);
				fprintf(f, "\nauto-gain  state=%d (-1 failed, 0 never run, 1 running, 2 done)\n",
					nam->autoState.load(std::memory_order_acquire));

				for (int i = 0; i < NUM_FAVS; i++)
				{
					fprintf(f, "  fav %-2d correction %+.2f dB  file %s\n", i + 1,
						nam->autoGainMilli[i].load(std::memory_order_acquire) / 1000.0,
						nam->favPaths[i][0] != '\0' ? nam->favPaths[i] : "(empty)");
				}

				fprintf(f, "\nactive fav %d\n", nam->activeFav.load(std::memory_order_acquire));
				fprintf(f, "label %d  \"%s\"\n", IDX_FAV_BROWSE, nam->hmiLbl[IDX_FAV_BROWSE]);
				fprintf(f, "value %d  \"%s\"\n", IDX_FAV_BROWSE, nam->hmiVal[IDX_FAV_BROWSE]);

				for (int i = 0; i < NUM_FAVS; i++)
				{
					char n[MAX_FAV_NAME];
					nam->fav_display_name(i + 1, n, sizeof(n));

					const int choice =
						nam->favNameChoice[i].load(std::memory_order_acquire);

					fprintf(f, "fav %-2d name=\"%s\" list=%d \"%s\"\n", i + 1, n, choice,
						(choice > 0 && choice < FAV_NAME_COUNT)
							? FAV_NAME_TABLE[choice] : "AUTO");
				}

				fclose(f);

				return LV2_WORKER_SUCCESS;
			}

			case kWorkTypeScan:
			{
				static_cast<NAM::Plugin*>(instance)->scan_models();

				return LV2_WORKER_SUCCESS;
			}

			case kWorkTypeSelect:
			{
				auto msg = static_cast<const LV2SelectMsg*>(data);
				auto nam = static_cast<NAM::Plugin*>(instance);

				const int count = (int)nam->modelFiles.size();

				if (count == 0)
					return LV2_WORKER_SUCCESS;

				int index;

				if (msg->absolute >= 0)
				{
					index = msg->absolute;

					if (index >= count)
						index = count - 1;
				}
				else
				{
					index = nam->selectedIndex.load(std::memory_order_acquire) + msg->delta;

					// the list wraps: after the last one comes the first
					while (index < 0)
						index += count;

					index %= count;
				}

				LV2LoadModelMsg load = { kWorkTypeLoad, {} };
				const std::string& path = nam->modelFiles[index];

				if (path.size() >= MAX_FILE_NAME)
					return LV2_WORKER_SUCCESS;

				memcpy(load.path, path.c_str(), path.size() + 1);

				// the rank is NOT written yet: it only counts if the load
				// lands, otherwise Current would move on with no change in sound
				nam->pendingIndex.store(index, std::memory_order_release);
				nam->loadStatus.store(1, std::memory_order_release);
				nam->activeFav.store(0, std::memory_order_release);	// we are leaving the favorites

				return work(instance, respond, handle, sizeof(load), &load);
			}

			case kWorkTypeFavSetPath:
			{
				auto msg = static_cast<const LV2FavPathMsg*>(data);
				auto nam = static_cast<NAM::Plugin*>(instance);

				if (msg->slot < 0 || msg->slot >= NUM_FAVS)
					return LV2_WORKER_SUCCESS;

				memcpy(nam->favPaths[msg->slot], msg->path, MAX_FILE_NAME);

				int rank = -1;
				for (size_t i = 0; i < nam->modelFiles.size(); i++)
				{
					if (nam->modelFiles[i] == nam->favPaths[msg->slot])
					{
						rank = (int)i;
						break;
					}
				}
				nam->favIndexes[msg->slot].store(rank, std::memory_order_release);

				return LV2_WORKER_SUCCESS;
			}

			case kWorkTypeFavStore:
			{
				auto msg = static_cast<const LV2FavMsg*>(data);
				auto nam = static_cast<NAM::Plugin*>(instance);

				if (msg->slot < 0 || msg->slot >= NUM_FAVS)
					return LV2_WORKER_SUCCESS;

				// The rank comes from the message: it was read when the press
				// happened, so BEFORE that same press loaded the favorite.
				const int index = msg->index;
				const std::string& src = (index >= 0 && index < (int)nam->modelFiles.size())
					? nam->modelFiles[index] : nam->currentModelPath;

				if (src.size() < MAX_FILE_NAME)
				{
					memcpy(nam->favPaths[msg->slot], src.c_str(), src.size() + 1);

					// warn the UI: this favorite's selector has to show
					// the new name
					nam->favDirty.fetch_or(1u << msg->slot, std::memory_order_release);
					nam->favIndexes[msg->slot].store(
						(index >= 0 && index < (int)nam->modelFiles.size()) ? index : -1,
						std::memory_order_release);
				}

				nam->activeFav.store(msg->slot + 1, std::memory_order_release);

				return LV2_WORKER_SUCCESS;
			}

			case kWorkTypeFavLoad:
			{
				auto msg = static_cast<const LV2FavMsg*>(data);
				auto nam = static_cast<NAM::Plugin*>(instance);

				if (msg->slot < 0 || msg->slot >= NUM_FAVS)
					return LV2_WORKER_SUCCESS;

				const char* path = nam->favPaths[msg->slot];
				const size_t len = strnlen(path, MAX_FILE_NAME);

				if (len == 0 || len >= MAX_FILE_NAME)
				{
					// empty slot: nothing is touched
					nam->loadStatus.store(-1, std::memory_order_release);

					return LV2_WORKER_SUCCESS;
				}

				// find its rank again so Current stays consistent
				int found = -1;
				for (size_t i = 0; i < nam->modelFiles.size(); i++)
				{
					if (nam->modelFiles[i] == path)
					{
						found = (int)i;
						break;
					}
				}

				LV2LoadModelMsg load = { kWorkTypeLoad, {} };
				memcpy(load.path, path, len + 1);

				nam->pendingIndex.store(found, std::memory_order_release);
				nam->activeFav.store(msg->slot + 1, std::memory_order_release);
				nam->loadStatus.store(1, std::memory_order_release);

				return work(instance, respond, handle, sizeof(load), &load);
			}

			case kWorkTypeFree:
			{
				auto msg = static_cast<const LV2FreeModelMsg*>(data);
				delete msg->model;

				return LV2_WORKER_SUCCESS;
			}

			case kWorkTypeSwitch:
				// should not happen!
				break;
		}

		return LV2_WORKER_ERR_UNKNOWN;
	}

	// runs on RT, right after process(), must not block or [de]allocate memory
	LV2_Worker_Status Plugin::work_response(LV2_Handle instance, uint32_t size,	const void* data)
	{
		if (*(const LV2WorkType*)data != kWorkTypeSwitch)
			return LV2_WORKER_ERR_UNKNOWN;

		auto msg = static_cast<const LV2SwitchModelMsg*>(data);
		auto nam = static_cast<NAM::Plugin*>(instance);

		// prepare reply for deleting old model
		LV2FreeModelMsg reply = { kWorkTypeFree, nam->currentModel };

		// swap current model with new one
		nam->currentModel = msg->model;
		nam->currentModelPath = msg->path;
		assert(nam->currentModelPath.capacity() >= MAX_FILE_NAME + 1);

		if (nam->currentModel != nullptr)
		{
			int receptiveFieldSize = nam->currentModel->GetReceptiveFieldSize();

			if (receptiveFieldSize > -1)
			{
				// A newly loaded model is prewarmed to have a silent sample history
				nam->silentSamples = receptiveFieldSize;
				nam->smartBypassed = true;
			}
		}

		if (nam->currentModel != nullptr)
		{
			const int pending = nam->pendingIndex.load(std::memory_order_acquire);

			if (pending >= 0)
				nam->selectedIndex.store(pending, std::memory_order_release);

			nam->loadStatus.store(2, std::memory_order_release);
			nam->loadCount.fetch_add(1, std::memory_order_release);
		}
		else
		{
			nam->loadStatus.store(-1, std::memory_order_release);
		}

		// send reply
		nam->schedule->schedule_work(nam->schedule->handle, sizeof(reply), &reply);

		// report change to host/ui
		nam->write_current_path();

		return LV2_WORKER_SUCCESS;
	}

	void Plugin::set_max_buffer_size(int size) noexcept
	{
		maxBufferSize = size;

		loader.SetDefaultMaxAudioBufferSize(size);
	}

	void Plugin::process(uint32_t n_samples) noexcept
	{
		lv2_atom_forge_set_buffer(&atom_forge, (uint8_t*)ports.notify, ports.notify->atom.size);
		lv2_atom_forge_sequence_head(&atom_forge, &sequence_frame, uris.units_frame);

		if (*(ports.quality_scale) != loader.GetDefaultQualityScaleFactor())
		{
			// Do this before checking the model path parameter so we make sure to set the quality first
			loader.SetDefaultQualityScaleFactor(*(ports.quality_scale));
		}

		// ---- navigating the models in the folder ------------------------
		// The audio thread NEVER reads the list: it sends a rank to the worker,
		// which resolves the path and loads. No lock, no allocation here.

		if (!scanRequested)
		{
			// first pass: ask for the folder scan
			LV2ScanMsg scan = { kWorkTypeScan };
			schedule->schedule_work(schedule->handle, sizeof(scan), &scan);
			scanRequested = true;
		}

		const float rescanNow = ports.rescan != nullptr ? *(ports.rescan) : 0.0f;
		if (!rescanSeen)
		{
			rescanSeen = true;
			prevRescan = rescanNow;
		}
		else if (rescanNow != prevRescan)
		{
			LV2ScanMsg scan = { kWorkTypeScan };
			schedule->schedule_work(schedule->handle, sizeof(scan), &scan);
			prevRescan = rescanNow;
		}

		// Next and Prev have only ever acted on the rising edge: that is already
		// the right behaviour for a momentary switch and for a browser click
		// that leaves the button at 1.
		const float nextNow = ports.step_next != nullptr ? *(ports.step_next) : 0.0f;
		if (nextNow > 0.5f && prevStepNext <= 0.5f)
		{
			LV2SelectMsg sel = { kWorkTypeSelect, -1, 1 };
			schedule->schedule_work(schedule->handle, sizeof(sel), &sel);
		}
		prevStepNext = nextNow;

		const float prevNow = ports.step_prev != nullptr ? *(ports.step_prev) : 0.0f;
		if (prevNow > 0.5f && prevStepPrev <= 0.5f)
		{
			LV2SelectMsg sel = { kWorkTypeSelect, -1, -1 };
			schedule->schedule_work(schedule->handle, sizeof(sel), &sel);
		}
		prevStepPrev = prevNow;

		const float indexNow = ports.model_index != nullptr ? *(ports.model_index) : 0.0f;
		if (!indexPortSeen)
		{
			// first pass: take the value as it stands and load nothing,
			// otherwise a port at zero would wipe the model restored from state
			indexPortSeen = true;
			prevIndexPort = indexNow;
		}
		else if (indexNow != prevIndexPort)
		{
			LV2SelectMsg sel = { kWorkTypeSelect, (int32_t)indexNow, 0 };
			schedule->schedule_work(schedule->handle, sizeof(sel), &sel);
			prevIndexPort = indexNow;
		}

		if (ports.model_count != nullptr)
			*(ports.model_count) = (float)modelCount.load(std::memory_order_acquire);

		if (ports.current_index != nullptr)
			*(ports.current_index) = (float)selectedIndex.load(std::memory_order_acquire);

		if (ports.load_status != nullptr)
			*(ports.load_status) = (float)loadStatus.load(std::memory_order_acquire);

		// ---- browse without loading, then commit ------------------------
		const int count = modelCount.load(std::memory_order_acquire);

		const float browseNow = ports.browse != nullptr ? *(ports.browse) : 0.0f;
		if (browseNow > 0.5f && prevBrowse <= 0.5f && count > 0)
		{
			int b = browseIndex.load(std::memory_order_acquire) + 1;

			if (b >= count)
				b = 0;

			browseIndex.store(b, std::memory_order_release);
		}
		prevBrowse = browseNow;

		const float actionNow = ports.action != nullptr ? *(ports.action) : 0.0f;
		if (actionNow > 0.5f && prevAction <= 0.5f && count > 0)
		{
			LV2SelectMsg sel = { kWorkTypeSelect, browseIndex.load(std::memory_order_acquire), 0 };
			schedule->schedule_work(schedule->handle, sizeof(sel), &sel);
		}
		prevAction = actionNow;

		// ---- the favorites: short press loads, long press stores --------
		for (int i = 0; i < NUM_FAVS; i++)
		{
			if (ports.fav[i] == nullptr)
				continue;

			const float now = *(ports.fav[i]);

			if (!favSeen[i])
			{
				// first pass: record the value without triggering anything
				favSeen[i] = true;
				prevFav[i] = now;
				continue;
			}

			if (now != prevFav[i])
			{
				const bool relachement = (now <= 0.5f);

				if (relachement && favHeldSamples[i] >= longPressSamples
					&& favHeldSamples[i] <= longPressMaxSamples)
				{
					// LONG PRESS RELEASED: store what was playing before the press.
					// Storing waits for the release, and is refused beyond the
					// ceiling: a button left ON in the browser can therefore no
					// longer overwrite a favorite on its own.
					LV2FavMsg msg = { kWorkTypeFavStore, i,
						favPressIndex[i], favPressGain[i] };
					schedule->schedule_work(schedule->handle, sizeof(msg), &msg);
					storeCount.fetch_add(1, std::memory_order_release);
				}
				else if (!(relachement && favHeldSamples[i] < longPressMaxSamples)
					&& activeFav.load(std::memory_order_acquire) != i + 1)
				{
					// Load this favorite -- except on the release of a momentary
					// switch, and except when it already plays: reloading a model
					// for nothing costs a great deal of CPU.
					favPressIndex[i] = selectedIndex.load(std::memory_order_acquire);
					favPressGain[i] = appliedGainMilli.load(std::memory_order_acquire);

					LV2FavMsg msg = { kWorkTypeFavLoad, i, -1, 0 };
					schedule->schedule_work(schedule->handle, sizeof(msg), &msg);
				}

				favHeldSamples[i] = 0;
			}
			else if (now > 0.5f && favHeldSamples[i] <= longPressMaxSamples)
			{
				favHeldSamples[i] += n_samples;
			}

			prevFav[i] = now;
		}

		if (ports.browse_index != nullptr)
			*(ports.browse_index) = (float)browseIndex.load(std::memory_order_acquire);

		// ---- the active favorite's gain ---------------------------------
		// Each favorite NOW has its own gain port: the host saves it with the
		// pedalboard, and the knob always shows the true value. No drift is
		// possible any more between what is displayed and what is heard.
		{
			const int fav = activeFav.load(std::memory_order_acquire);
			float g = 0.0f;

			if (fav >= 1 && fav <= NUM_FAVS)
			{
				if (ports.favGain[fav - 1] != nullptr)
					g = *(ports.favGain[fav - 1]);

				// The correction is no longer added here: now that we can ASK the
				// host for it, it lives in the knob itself. Otherwise it would
				// count twice.
			}
			else if (ports.fav_gain != nullptr)
				g = *(ports.fav_gain);	// outside a favorite: the general gain

			appliedGainMilli.store((int)(g * 1000.0f), std::memory_order_release);
		}

		// ---- stepping through the favorites only ------------------------
		const float favBrowseNow = ports.fav_browse != nullptr ? *(ports.fav_browse) : 0.0f;

		if (favBrowseNow > 0.5f && favBrowseHeld < longPressMaxSamples)
			favBrowseHeld += n_samples;

		if (!favBrowseSeen)
		{
			favBrowseSeen = true;
			prevFavBrowse = favBrowseNow;
		}
		else if (favBrowseNow != prevFavBrowse)
		{
			const bool relachement = (favBrowseNow <= 0.5f);
			const bool shortPress = relachement && favBrowseHeld < longPressMaxSamples;

			prevFavBrowse = favBrowseNow;

			if (!relachement)
				favBrowseHeld = 0;

			// The host's Momentary flag arrives EMPTY on this machine: a foot press
			// is therefore recognised by HOW LONG the state stays high. A release
			// after a short high state follows a press that has already acted, so
			// it is ignored -- otherwise two steps and two loads per press.
			// A browser button stays high far longer: unticking it acts
			// normally.
			if (!shortPress)
			{

			// go to the next NON-EMPTY favorite, wrapping round at the end
			const int startFav = activeFav.load(std::memory_order_acquire);

			int cycleLen = ports.cycle_count != nullptr ? (int)*(ports.cycle_count) : NUM_FAVS;
			if (cycleLen < 1) cycleLen = 1;
			if (cycleLen > NUM_FAVS) cycleLen = NUM_FAVS;

			bool found = false;

			for (int step = 1; step <= cycleLen; step++)
			{
				const int target = ((startFav - 1 + step) % cycleLen + cycleLen) % cycleLen;

				if (favPaths[target][0] == '\0')
					continue;

				LV2FavMsg msg = { kWorkTypeFavLoad, target, -1, 0 };
				schedule->schedule_work(schedule->handle, sizeof(msg), &msg);
				found = true;
				break;
			}

			if (!found)
			{
				// no filled favorite in the cycle: plain silence looked like a
				// breakdown, so it is reported through the status port
				loadStatus.store(-1, std::memory_order_release);
			}
			}
		}

		// ---- tell the UI about favorites stored with the foot -----------
		{
			unsigned bits = favDirty.exchange(0, std::memory_order_acquire);

			for (int i = 0; i < NUM_FAVS && bits != 0; i++)
			{
				if (bits & (1u << i))
					write_fav_path(i);
			}

			if (bits != 0)
			{
				// a path has changed: clear the display cache, otherwise
				// the screen would keep the old label for ever
				for (int k = 0; k < NUM_PORTS_TOTAL; k++)
				{
					hmiLbl[k][0] = '\0';
					hmiVal[k][0] = '\0';
				}
			}
		}

		// ---- storing from the web UI ------------------------------------
		// The web UI writes 1 then 0 into the port between two audio cycles:
		// the rising edge is never seen here. So anything that CHANGES the
		// value fires, rising or falling.
		const float storeNow = ports.store != nullptr ? *(ports.store) : 0.0f;
		if (!storeSeen)
		{
			storeSeen = true;
			prevStore = storeNow;
		}
		else if (storeNow != prevStore)
		{
			const int slot = ports.store_slot != nullptr ? (int)*(ports.store_slot) - 1 : -1;

			if (slot >= 0 && slot < NUM_FAVS)
			{
				LV2FavMsg msg = { kWorkTypeFavStore, slot,
					selectedIndex.load(std::memory_order_acquire),
					appliedGainMilli.load(std::memory_order_acquire) };
				schedule->schedule_work(schedule->handle, sizeof(msg), &msg);
				storeCount.fetch_add(1, std::memory_order_release);
			}

			prevStore = storeNow;
		}

		{
			// ---- a favorite's name: a rank in the list ----------------------
			// The host keeps that rank with the pedalboard like any other
			// setting; we hold a copy, the only one the screen reads, and
			// any change repaints.
			for (int i = 0; i < NUM_FAVS; i++)
			{
				if (ports.favNamePort[i] == nullptr)
					continue;

				const float v = *(ports.favNamePort[i]);

				if (!favNameSeen[i])
				{
					favNameSeen[i] = true;
					prevFavName[i] = v;

					// A value the host has already posted wins over the one
					// coming from the state. Zero means AUTO, that is to say
					// "nothing picked": the restored rank then survives.
					if (v > 0.0f)
						favNameChoice[i].store((int)v, std::memory_order_release);

					continue;
				}

				if (v == prevFavName[i])
					continue;

				prevFavName[i] = v;
				favNameChoice[i].store((int)v, std::memory_order_release);
				hmiLbl[IDX_FAV_FIRST + i][0] = '\0';
				hmiVal[IDX_FAV_FIRST + i][0] = '\0';
				hmiLbl[IDX_FAV_BROWSE][0] = '\0';
				screenFav.store(-1, std::memory_order_release);
			}

			// a state has just been restored: put the ranks back into the
			// ports, once, and AFTER seeing what the host had put there
			if (namesToRestore.exchange(false, std::memory_order_acquire))
				restore_name_ports();

			const int fav = activeFav.load(std::memory_order_acquire);
			const int pending = screenFav.load(std::memory_order_acquire);

			hmiPos += n_samples;
			screenCounter += n_samples;
			traceCounter += n_samples;

			if (traceCounter >= tracePeriod)
			{
				traceCounter = 0;
				LV2ScanMsg t = { kWorkTypeTrace };
				schedule->schedule_work(schedule->handle, sizeof(t), &t);
			}

			// Two reasons to write: the favorite changed, or the time has come.
			// The periodic refresh is essential -- the firmware redraws ON/OFF
			// on every port value change and wipes our label. 4 sends per
			// second, well under the budget of 25.
			if (pending != fav || screenCounter >= screenPeriod || screenForced)
			{
				// the full screen shows up ONLY on a real change of favorite
				write_screen(fav, pending != fav && !screenForced);
				screenForced = false;
				screenFav.store(fav, std::memory_order_release);
				screenCounter = 0;
			}
		}

		if (ports.active_fav != nullptr)
			*(ports.active_fav) = (float)activeFav.load(std::memory_order_acquire);

		for (int i = 0; i < NUM_FAVS; i++)
		{
			if (ports.favIndex[i] != nullptr)
				*(ports.favIndex[i]) = (float)favIndexes[i].load(std::memory_order_acquire);
		}

		// ---- one store button per favorite ------------------------------
		for (int i = 0; i < NUM_FAVS; i++)
		{
			if (ports.favStore[i] == nullptr)
				continue;

			const float v = *(ports.favStore[i]);

			if (!favStoreSeen[i])
			{
				favStoreSeen[i] = true;
				prevFavStore[i] = v;
				continue;
			}

			// Any change stores, rising or falling: a browser click can come
			// either way round, and storing is idempotent -- doing it twice
			// costs nothing.
			if (v != prevFavStore[i])
			{
				prevFavStore[i] = v;

				LV2FavMsg msg = { kWorkTypeFavStore, i,
					selectedIndex.load(std::memory_order_acquire),
					appliedGainMilli.load(std::memory_order_acquire) };
				schedule->schedule_work(schedule->handle, sizeof(msg), &msg);
				storeCount.fetch_add(1, std::memory_order_release);
			}
		}

		// ---- picking the favorite by voltage ----------------------------
		// The 0-10 V range is cut into NUM_FAVS equal bands. A load only happens
		// when the band CHANGES, with a margin so that a voltage sitting on a
		// boundary does not make the loading oscillate.
		if (ports.cv_select != nullptr && n_samples > 0)
		{
			// READ THE FIRST SAMPLE, never the last: when nothing is plugged into
			// the CV input, the host may allocate a single value only. Reading
			// index n_samples-1 then ran off the buffer -- and reconnecting the
			// ports, which moving a block does, moves those buffers. That is an
			// out-of-bounds read, so a crash.
			const float volts = ports.cv_select[0];
			const float largeur = 10.0f / (float)NUM_FAVS;

			int bande = (int)(volts / largeur);

			if (bande < 0) bande = 0;
			if (bande >= NUM_FAVS) bande = NUM_FAVS - 1;

			// margin: a tenth of a band has to be crossed to switch
			const float centre = ((float)bande + 0.5f) * largeur;
			const float ecart = volts > centre ? volts - centre : centre - volts;

			if (bande != cvBand && ecart < largeur * 0.4f)
			{
				cvBand = bande;

				if (favPaths[bande][0] != '\0')
				{
					LV2FavMsg msg = { kWorkTypeFavLoad, bande, -1, 0 };
					schedule->schedule_work(schedule->handle, sizeof(msg), &msg);
				}
			}
		}

		// ---- starting the automatic measurement -------------------------
		const float autoNow = ports.auto_gain != nullptr ? *(ports.auto_gain) : 0.0f;

		if (!autoGainSeen)
		{
			autoGainSeen = true;
			prevAutoGain = autoNow;
		}
		else if (autoNow != prevAutoGain)
		{
			prevAutoGain = autoNow;

			LV2ScanMsg m = { kWorkTypeAutoGain };
			schedule->schedule_work(schedule->handle, sizeof(m), &m);
		}

		if (ports.auto_state != nullptr)
			*(ports.auto_state) = (float)autoState.load(std::memory_order_acquire);

		if (ports.auto_offset != nullptr)
		{
			const int f = activeFav.load(std::memory_order_acquire);

			*(ports.auto_offset) = (f >= 1 && f <= NUM_FAVS)
				? autoGainMilli[f - 1].load(std::memory_order_acquire) / 1000.0f
				: 0.0f;
		}

		// ---- apply or undo the measured gains ---------------------------
		if (applyRequested.exchange(false, std::memory_order_acquire))
		{
			for (int i = 0; i < NUM_FAVS; i++)
				gainBefore[i] = ports.favGain[i] != nullptr ? *(ports.favGain[i]) : 0.0f;

			gainSaved = true;
			write_gains(false);
		}

		const float applyNow = ports.auto_apply != nullptr ? *(ports.auto_apply) : 0.0f;

		if (!autoApplySeen) { autoApplySeen = true; prevAutoApply = applyNow; }
		else if (applyNow != prevAutoApply)
		{
			prevAutoApply = applyNow;

			// keep the previous values so it can be undone
			for (int i = 0; i < NUM_FAVS; i++)
			{
				gainBefore[i] = ports.favGain[i] != nullptr ? *(ports.favGain[i]) : 0.0f;
			}

			gainSaved = true;
			write_gains(false);
		}

		const float undoNow = ports.auto_undo != nullptr ? *(ports.auto_undo) : 0.0f;

		if (!autoUndoSeen) { autoUndoSeen = true; prevAutoUndo = undoNow; }
		else if (undoNow != prevAutoUndo)
		{
			prevAutoUndo = undoNow;
			write_gains(true);
		}

		// ---- feedback channel: the measured correction, fav by fav ------
		// The panel gets one value at a time: one favorite per second goes
		// round. db_slot says which one it is, auto_db_slot carries its
		// correction. Names no longer travel this way: they live in a control
		// port, which the UI reads directly.
		rotationCounter += n_samples;

		if (rotationCounter >= (uint32_t)sampleRate)
		{
			rotationCounter = 0;
			rotationSlot = (rotationSlot + 1) % NUM_FAVS;
		}

		// The correction goes out FIRST, the favorite's number a tenth of a
		// second later. That number is what triggers the display in the
		// browser, so it always arrives after the value it points at.
		// Two ports changed in the same cycle reach the browser in an order
		// nothing guarantees, and the correction used to land in the previous
		// favorite's cell. This number changes on every turn, even when two
		// favorites share the same correction: the display is therefore
		// always redone.
		if (ports.auto_db_slot != nullptr)
			*(ports.auto_db_slot) =
				autoGainMilli[rotationSlot].load(std::memory_order_acquire) / 1000.0f;

		if (ports.db_slot != nullptr
			&& rotationCounter >= (uint32_t)(sampleRate * 0.1))
			*(ports.db_slot) = (float)(rotationSlot + 1);

		if (ports.kx_state != nullptr)
			*(ports.kx_state) = (float)kxState;

		if (ports.hmi_state != nullptr)
		{
			// One number says it all: units = state, tens and beyond =
			// capabilities the firmware announced for Fav Next.
			//   state: 0 no HMI, 1 HMI with no addressing, 2 addressed,
			//          3 addressed + popup available
			//   caps : 1 LED, 2 label, 4 value, 8 unit, 16 indicator
			// Read it as 2 + 10 x caps: 62 means caps 6 (label+value),
			// state 2.
			int state = 0;
			int caps = 0;

			if (hmi != nullptr)
			{
				state = 1;

				if (hmiAddr[IDX_FAV_BROWSE] != nullptr)
				{
					state = 2;
					caps = hmiCaps[IDX_FAV_BROWSE];

					if ((hmi->size == 0 || hmi->size >= LV2_HMI_WIDGETCONTROL_SIZE_POPUP_MESSAGE)
						&& hmi->popup_message != nullptr)
						state = 3;
				}
			}

			*(ports.hmi_state) = (float)(state + 10 * caps);
		}

		if (ports.store_count != nullptr)
			*(ports.store_count) = (float)storeCount.load(std::memory_order_acquire);

		if (ports.applied_gain != nullptr)
			*(ports.applied_gain) = appliedGainMilli.load(std::memory_order_acquire) / 1000.0f;

		LV2_ATOM_SEQUENCE_FOREACH(ports.control, event)
		{
			if (event->body.type == uris.atom_Object)
			{
				const auto obj = reinterpret_cast<LV2_Atom_Object*>(&event->body);
				if (obj->body.otype == uris.patch_Get)
				{
					write_current_path();
				}
				else if (obj->body.otype == uris.patch_Put)
				{
					// A PUT PUSHES A WHOLE OBJECT, property by property, inside
					// a body. That is the shape a host uses to hand over a
					// complete state -- a plugin that reads Set messages only
					// drops those values in silence.
					putsSeen.fetch_add(1, std::memory_order_release);

					const LV2_Atom_Object* corps = NULL;

					lv2_atom_object_get(obj, uris.patch_body, &corps, 0);

					if (corps != nullptr && corps->atom.type == uris.atom_Object)
					{
						LV2_ATOM_OBJECT_FOREACH(corps, prop)
						{
							apply_parameter(prop->key, &prop->value);
						}
					}
				}
				else if (obj->body.otype == uris.patch_Set)
				{
					const LV2_Atom* property = NULL;
					const LV2_Atom* file_path = NULL;

					lv2_atom_object_get(obj,
					                    uris.patch_property, &property,
					                    uris.patch_value, &file_path,
					                    0);

					if (property && property->type == uris.atom_URID &&
						file_path && file_path->type == uris.atom_Path &&
						file_path->size > 0 && file_path->size < MAX_FILE_NAME)
					{
						const LV2_URID target = ((const LV2_Atom_URID*)property)->body;

						if (target == uris.model_Path)
						{
							LV2LoadModelMsg msg = { kWorkTypeLoad, {} };
							memcpy(msg.path, file_path + 1, file_path->size);
							pendingIndex.store(-1, std::memory_order_release);
							schedule->schedule_work(schedule->handle, sizeof(msg), &msg);
						}
						else
						{
							// one of the favorites, picked in the web UI
							for (int i = 0; i < NUM_FAVS; i++)
							{
								if (target != uris.fav_Path[i])
									continue;

								LV2FavPathMsg msg = { kWorkTypeFavSetPath, i, {} };
								memcpy(msg.path, file_path + 1, file_path->size);
								schedule->schedule_work(schedule->handle, sizeof(msg), &msg);
								break;
							}
						}
					}
				}
			}
		}

		float level;

		float modelInputAdjustmentDB = 0;

		if (currentModel != nullptr)
		{
			if (*(ports.quality_scale) != currentModel->GetQualityScaleFactor())
			{
				currentModel->SetQualityScaleFactor(*(ports.quality_scale));
			}

			modelInputAdjustmentDB = currentModel->GetRecommendedInputDBAdjustment();

#ifdef SMART_BYPASS_ENABLED
			int receptiveFieldSamples = currentModel->GetReceptiveFieldSize();

			if (receptiveFieldSamples > -1)
			{
				for (unsigned int i = 0; i < n_samples; i++)
				{
					if (abs(ports.audio_in[i]) <= bypassThresholdLinear)
					{
						silentSamples++;
					}
					else
					{
						silentSamples = 0;
					}
				}

				if (silentSamples >= (uint32_t)receptiveFieldSamples)
				{
					silentSamples = (uint32_t)receptiveFieldSamples;	// Prevent silentSamples growing and eventually overflowing uint32

					if (smartBypassed)
					{
						for (unsigned int i = 0; i < n_samples; i++)
						{
							ports.audio_out[i] = ports.audio_in[i];
						}

						return;
					}

					smartBypassed = true; // If we aren't already, we'll be bypassed on the next process call
				}
				else
					smartBypassed = false;
			}
#endif
		}

		// convert input level from db
		float desiredInputLevel = powf(10, (*(ports.input_level) + modelInputAdjustmentDB) * 0.05f);

		if (fabs(desiredInputLevel - inputLevel) > SMOOTH_EPSILON)
		{
			level = inputLevel;
			for (unsigned int i = 0; i < n_samples; i++)
			{
				// do very basic smoothing
				level = (.99f * level) + (.01f * desiredInputLevel);

				ports.audio_out[i] = ports.audio_in[i] * level;
			}

			inputLevel = level;
		}
		else
		{
			level = inputLevel = desiredInputLevel;

			for (unsigned int i = 0; i < n_samples; i++)
			{
				ports.audio_out[i] = ports.audio_in[i] * level;
			}
		}

		float modelLoudnessAdjustmentDB = 0;

		if (currentModel != nullptr)
		{
			currentModel->Process(ports.audio_out, ports.audio_out, n_samples);

			modelLoudnessAdjustmentDB = currentModel->GetRecommendedOutputDBAdjustment();
		}

		// Convert output level from db
		const float favGainDB = appliedGainMilli.load(std::memory_order_acquire) / 1000.0f;

		float desiredOutputLevel = powf(10,
			(*(ports.output_level) + modelLoudnessAdjustmentDB + favGainDB) * 0.05f);

		if (fabs(desiredOutputLevel - outputLevel) > SMOOTH_EPSILON)
		{
			level = outputLevel;

			for (unsigned int i = 0; i < n_samples; i++)
			{
				// do very basic smoothing
				level = (.99f * level) + (.01f * desiredOutputLevel);

				ports.audio_out[i] = ports.audio_out[i] * outputLevel;
			}

			outputLevel = level;
		}
		else
		{
			level = outputLevel = desiredOutputLevel;

			for (unsigned int i = 0; i < n_samples; i++)
			{
				ports.audio_out[i] = ports.audio_out[i] * level;
			}
		}

		//float dcBlockCoefficient = 1 - (220.0 / sampleRate);

		//for (unsigned int i = 0; i < n_samples; i++)
		//{
		//	float dcInput = ports.audio_out[i];

		//	// dc blocker
		//	ports.audio_out[i] = ports.audio_out[i] - prevDCInput + dcBlockCoefficient * prevDCOutput;

		//	prevDCInput = dcInput;
		//	prevDCOutput = ports.audio_out[i];
		//}
	}

	uint32_t Plugin::options_get(LV2_Handle, LV2_Options_Option*)
	{
		// currently unused
		return LV2_OPTIONS_ERR_UNKNOWN;
	}

	uint32_t Plugin::options_set(LV2_Handle instance, const LV2_Options_Option* options)
	{
		auto nam = static_cast<NAM::Plugin*>(instance);

		for (int i=0; options[i].key && options[i].type; ++i)
		{
			if (options[i].key == nam->uris.bufSize_maxBlockLength && options[i].type == nam->uris.atom_Int)
			{
				nam->set_max_buffer_size(*(const int32_t*)options[i].value);
				break;
			}
		}

		return LV2_OPTIONS_SUCCESS;
	}

	LV2_State_Status Plugin::save(LV2_Handle instance, LV2_State_Store_Function store, LV2_State_Handle handle, 
		uint32_t flags, const LV2_Feature* const* features)
	{
		auto nam = static_cast<NAM::Plugin*>(instance);

		lv2_log_trace(&nam->logger, "Saving state\n");

		if (!nam->currentModel)
		{
			// no model, but the favorites deserve to be kept
			nam->store_favorites(store, handle);

			return LV2_STATE_SUCCESS;
		}

		LV2_State_Map_Path* map_path = (LV2_State_Map_Path*)lv2_features_data(features, LV2_STATE__mapPath);

		if (map_path == nullptr)
		{
			lv2_log_error(&nam->logger, "LV2_STATE__mapPath unsupported by host\n");

			return LV2_STATE_ERR_NO_FEATURE;
		}

		// Map absolute sample path to an abstract state path
		char* apath = map_path->abstract_path(map_path->handle, nam->currentModelPath.c_str());

		store(handle, nam->uris.model_Path, apath, strlen(apath) + 1, nam->uris.atom_Path,
			LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);

		nam->store_favorites(store, handle);

		LV2_State_Free_Path* free_path = (LV2_State_Free_Path *)lv2_features_data(features, LV2_STATE__freePath);

		if (free_path != nullptr)
		{
			free_path->free_path(free_path->handle, apath);
		}
		else
		{
#ifndef _WIN32	// Can't free host-allocated memory on plugin side under Windows
			free(apath);
#endif
		}

		return LV2_STATE_SUCCESS;
	}

	LV2_State_Status Plugin::restore(LV2_Handle instance, LV2_State_Retrieve_Function retrieve, LV2_State_Handle handle, 
		uint32_t flags, const LV2_Feature* const* features)
	{
		auto nam = static_cast<NAM::Plugin*>(instance);

		// Take the favorites back before the model
		{
			size_t   fsize = 0;
			uint32_t ftype = 0;
			uint32_t fflags = 0;
			const void* favs = retrieve(handle, nam->uris.favs_String, &fsize, &ftype, &fflags);

			if (favs != nullptr && ftype == nam->uris.atom_String && fsize > 0)
			{
				std::string joined(static_cast<const char*>(favs), fsize - 1);
				size_t start = 0;

				for (int i = 0; i < NUM_FAVS; i++)
				{
					const size_t cut = joined.find('\n', start);

					if (cut == std::string::npos)
						break;

					std::string line = joined.substr(start, cut - start);

					// Older states put either a gain or a typed name after a
					// tab. Both are gone: cut at the tab and keep nothing but
					// the path.
					const size_t tab = line.find('\t');

					if (tab != std::string::npos)
						line.resize(tab);

					if (line.size() < MAX_FILE_NAME)
						memcpy(nam->favPaths[i], line.c_str(), line.size() + 1);

					start = cut + 1;
				}
			}
		}

		// ---- the name picked in the list, favorite by favorite ----------
		// Without this, reopening a pedalboard would drop every favorite back
		// to AUTO as soon as the host does not repost the port value itself.
		{
			size_t   nsize = 0;
			uint32_t ntype = 0;
			uint32_t nflags = 0;
			const void* names = retrieve(handle, nam->uris.favNames_String,
				&nsize, &ntype, &nflags);

			if (names != nullptr && ntype == nam->uris.atom_String && nsize > 0)
			{
				std::string list(static_cast<const char*>(names), nsize - 1);
				size_t start = 0;

				for (int i = 0; i < NUM_FAVS; i++)
				{
					size_t cut = list.find(',', start);

					if (cut == std::string::npos)
						cut = list.size();

					const std::string field = list.substr(start, cut - start);

					if (!field.empty())
					{
						// zero included: that is AUTO, a choice like any other
						const int rank = atoi(field.c_str());

						if (rank >= 0 && rank < FAV_NAME_COUNT)
							nam->favNameChoice[i].store(rank, std::memory_order_release);
					}

					if (cut >= list.size())
						break;

					start = cut + 1;
				}

				// the audio thread will put them back into the control ports
				nam->namesToRestore.store(true, std::memory_order_release);
			}
		}

		// Get model_Path from state
		size_t      size     = 0;
		uint32_t    type     = 0;
		uint32_t    valflags = 0;
		const void* value = retrieve(handle, nam->uris.model_Path, &size, &type, &valflags);

		lv2_log_trace(&nam->logger, "Restoring model '%s'\n", (const char*)value);

		NAM::LV2LoadModelMsg msg = { NAM::kWorkTypeLoad, {} };

		LV2_State_Status result = LV2_STATE_SUCCESS;

		// Check if a path is set
		if (!value || (type != nam->uris.atom_Path))
		{
			msg.path[0] = '\0';
		}
		else
		{
			LV2_State_Map_Path* map_path = (LV2_State_Map_Path*)lv2_features_data(features, LV2_STATE__mapPath);

			if (map_path == nullptr)
			{
				lv2_log_error(&nam->logger, "LV2_STATE__mapPath unsupported by host\n");

				return LV2_STATE_ERR_NO_FEATURE;
			}

			// Map abstract state path to absolute path
			char* path = map_path->absolute_path(map_path->handle, (const char *)value);

			size_t pathLen = strlen(path);

			if (pathLen >= MAX_FILE_NAME)
			{
				lv2_log_error(&nam->logger, "Model path is too long (max %u chars)\n", MAX_FILE_NAME);

				result = LV2_STATE_ERR_UNKNOWN;
			}
			else
			{
				memcpy(msg.path, path, pathLen);
			}

			LV2_State_Free_Path* free_path = (LV2_State_Free_Path*)lv2_features_data(features, LV2_STATE__freePath);

			if (free_path != nullptr)
			{
				free_path->free_path(free_path->handle, path);
			}
			else
			{
#ifndef _WIN32	// Can't free host-allocated memory on plugin side under Windows
				free(path);
#endif
			}
		}

		if (result == LV2_STATE_SUCCESS)
		{
			// Schedule model to be loaded by the provided worker
			nam->schedule->schedule_work(nam->schedule->handle, sizeof(msg), &msg);

			nam->currentModelPath = msg.path;
		}

		return result;
	}

	// --- writing on the machine's screen ---------------------------------
	// The host tells us when the user assigns a port to an actuator, and hands
	// over the token to write on that display. Without the two notification
	// functions declared in extensionData, nothing ever happens.
	// Three rules taken from the looper, each paid for by a real fault:
	//  1. send only what has CHANGED, to stay under the screen budget;
	//  2. respect the capabilities announced at addressing time;
	//  3. stay QUIET during a full screen -- a label written after a popup
	//     repaints the page and wipes the message.
	void Plugin::hmi_label(int port, const char* txt)
	{
		if (HMI_DISABLED) return;

		if (popup_showing()) return;
		if (hmi == nullptr || port < 0 || port >= NUM_PORTS_TOTAL) return;
		if (hmiAddr[port] == nullptr || hmi->set_label == nullptr) return;
		if (!(hmiCaps[port] & LV2_HMI_AddressingCapability_Label)) return;
		if (strncmp(hmiLbl[port], txt, sizeof(hmiLbl[0]) - 1) == 0) return;

		strncpy(hmiLbl[port], txt, sizeof(hmiLbl[0]) - 1);
		hmiLbl[port][sizeof(hmiLbl[0]) - 1] = '\0';
		hmi->set_label(hmi->handle, hmiAddr[port], hmiLbl[port]);
	}

	void Plugin::hmi_value(int port, const char* txt)
	{
		if (HMI_DISABLED) return;

		if (popup_showing()) return;
		if (hmi == nullptr || port < 0 || port >= NUM_PORTS_TOTAL) return;
		if (hmiAddr[port] == nullptr || hmi->set_value == nullptr) return;
		if (!(hmiCaps[port] & LV2_HMI_AddressingCapability_Value)) return;
		if (strncmp(hmiVal[port], txt, sizeof(hmiVal[0]) - 1) == 0) return;

		strncpy(hmiVal[port], txt, sizeof(hmiVal[0]) - 1);
		hmiVal[port][sizeof(hmiVal[0]) - 1] = '\0';
		hmi->set_value(hmi->handle, hmiAddr[port], hmiVal[port]);
	}

	void Plugin::hmi_popup(int port, const char* title, const char* message)
	{
		if (HMI_DISABLED) return;

		if (hmi == nullptr || hmi->popup_message == nullptr) return;

		// The size field says what the host really provides, but SOME HOSTS
		// LEAVE IT AT ZERO: refuse only when it is filled in AND too small.
		// The earlier test (size >= ...) blocked everything in that case.
		if (hmi->size != 0 && hmi->size < LV2_HMI_WIDGETCONTROL_SIZE_POPUP_MESSAGE)
			return;

		// the title goes out AS A LABEL first: written after, it would wipe the popup
		popupAt = 0;
		hmi_label(port, title);
		popupAt = hmiPos;

		if (port >= 0 && port < NUM_PORTS_TOTAL && hmiAddr[port] != nullptr)
		{
			hmi->popup_message(hmi->handle, hmiAddr[port],
				LV2_HMI_Popup_Style_Inverted, title, message);
			return;
		}

		// fallback: any addressed port will do for a full screen
		for (int k = 0; k < NUM_PORTS_TOTAL; k++)
		{
			if (hmiAddr[k] != nullptr)
			{
				hmi->popup_message(hmi->handle, hmiAddr[k],
					LV2_HMI_Popup_Style_Inverted, title, message);
				return;
			}
		}
	}

	// Applies a property/value pair, whether it comes from a patch:Set (one
	// property at a time) or from a patch:Put (a whole object at once).
	// Approximate log2, written here because glibc's log(), log10() and exp()
	// are marked GLIBC_2.29 while the machine caps at 2.27. Same trap as
	// pow@GLIBC_2.29 on the tuner. The precision is far more than enough for
	// a gain expressed in decibels.
	static float nam_log2f(float x)
	{
		if (x <= 0.0f)
			return -127.0f;

		union { float f; uint32_t i; } u;
		u.f = x;

		const float e = (float)((int)((u.i >> 23) & 0xFF) - 127);

		u.i = (u.i & 0x007FFFFFu) | 0x3F800000u;	// mantissa brought back into [1,2)
		const float m = u.f;

		const float p = -1.7417939f + (2.8212026f + (-1.4699568f
			+ (0.44717955f - 0.056570851f * m) * m) * m) * m;

		return e + p;
	}

	static float nam_log10f(float x)
	{
		return nam_log2f(x) * 0.30102999566f;
	}

	// Measures every favorite's output level and derives a correction from it,
	// so that they all sound at the same volume.
	//
	// Runs on the WORKER thread: a model is loaded aside, made to process a
	// test signal identical for all of them, its energy is measured, and it is
	// freed. The playing model is never touched and the sound goes on.
	//
	// The test signal is deterministic noise (fixed-seed generator): two runs
	// over the same model therefore give exactly the same result.
	void Plugin::measure_levels()
	{
		static constexpr int N_ESSAI = 8192;	// about 170 ms at 48 kHz
		static constexpr int N_BLOC = 256;

		autoState.store(1, std::memory_order_release);

		float entree[N_BLOC];
		float out[N_BLOC];
		float levels[NUM_FAVS];
		bool measured[NUM_FAVS] = {};
		int combien = 0;

		for (int i = 0; i < NUM_FAVS; i++)
		{
			levels[i] = 0.0f;

			if (favPaths[i][0] == '\0')
				continue;

			auto modele = loader.CreateFromFile(favPaths[i]);

			if (modele == nullptr)
				continue;

			// ESSENTIAL: a fresh model does not know what block size it will be
			// given, and its internal states are empty. Without these two calls
			// the processing yields nothing usable -- that is what made the
			// measurement come out silent.
			modele->SetMaxAudioBufferSize(N_BLOC);
			modele->Prewarm();

			// level recommended by the model, as in normal processing
			const float inputDB = modele->GetRecommendedInputDBAdjustment();
			const float outputDB = modele->GetRecommendedOutputDBAdjustment();
			const float gainIn = powf(10.0f, inputDB * 0.05f);
			const float gainOut = powf(10.0f, outputDB * 0.05f);

			uint32_t seed = 12345u;
			double somme = 0.0;
			int comptes = 0;

			for (int fait = 0; fait < N_ESSAI; fait += N_BLOC)
			{
				for (int k = 0; k < N_BLOC; k++)
				{
					// congruential generator: identical on every call
					seed = seed * 1103515245u + 12345u;
					const float bruit = ((float)((seed >> 9) & 0xFFFF) / 32768.0f) - 1.0f;

					entree[k] = bruit * 0.25f * gainIn;
				}

				modele->Process(entree, out, N_BLOC);

				// the first blocks serve to fill the internal states
				if (fait >= N_BLOC * 4)
				{
					for (int k = 0; k < N_BLOC; k++)
					{
						const float v = out[k] * gainOut;
						somme += (double)v * (double)v;
						comptes++;
					}
				}
			}

			modele = nullptr;	// free the test model

			if (comptes > 0 && somme > 0.0)
			{
				levels[i] = (float)sqrt(somme / (double)comptes);
				measured[i] = true;
				combien++;
			}
		}

		if (combien == 0)
		{
			autoState.store(-1, std::memory_order_release);
			return;
		}

		// Reference: the MEDIAN of the measured levels. It spreads the
		// correction instead of dragging everything down to the quietest, and
		// needs no logarithm -- glibc's are out of reach here.
		float tries[NUM_FAVS];
		int n = 0;

		for (int i = 0; i < NUM_FAVS; i++)
		{
			if (measured[i])
				tries[n++] = levels[i];
		}

		for (int a = 1; a < n; a++)
		{
			const float v = tries[a];
			int b = a - 1;

			while (b >= 0 && tries[b] > v)
			{
				tries[b + 1] = tries[b];
				b--;
			}

			tries[b + 1] = v;
		}

		const float reference = (n % 2 == 1) ? tries[n / 2]
			: 0.5f * (tries[n / 2 - 1] + tries[n / 2]);

		for (int i = 0; i < NUM_FAVS; i++)
		{
			if (!measured[i])
			{
				autoGainMilli[i].store(0, std::memory_order_release);
				continue;
			}

			float db = 20.0f * nam_log10f(reference / levels[i]);

			// safety bound: never more than 12 dB either way
			if (db > 12.0f) db = 12.0f;
			if (db < -12.0f) db = -12.0f;

			autoGainMilli[i].store((int)(db * 1000.0f), std::memory_order_release);
		}

		autoState.store(2, std::memory_order_release);

		// Measuring then applying is ONE gesture: ask for the knobs to be
		// written as soon as the measurement lands. The audio thread will do
		// it, the kx extension cannot be called from the worker.
		applyRequested.store(true, std::memory_order_release);
	}

	// Asks the host to put the measured gains into the knobs -- or to put the
	// previous ones back. The knob therefore REALLY moves before your eyes, and
	// the value is saved with the pedalboard like any other setting.
	void Plugin::write_gains(bool undo)
	{
		if (portreq == nullptr || portreq->request_change == nullptr)
		{
			kxState = 0;	// the host does not provide the extension
			return;
		}

		kxState = 1;

		for (int i = 0; i < NUM_FAVS; i++)
		{
			if (ports.favGain[i] == nullptr)
				continue;

			const uint32_t index = (uint32_t)(34 + i);	// ports fav_gain_1..10

			if (undo)
			{
				if (gainSaved)
				{
					const int rep = portreq->request_change(portreq->handle, index,
						gainBefore[i]);
					kxState = (rep == LV2_CONTROL_INPUT_PORT_CHANGE_SUCCESS) ? 2 : -1;
				}
			}
			else
			{
				const float correction =
					autoGainMilli[i].load(std::memory_order_acquire) / 1000.0f;

				const int rep = portreq->request_change(portreq->handle, index, correction);
				kxState = (rep == LV2_CONTROL_INPUT_PORT_CHANGE_SUCCESS) ? 2 : -1;
			}
		}

		if (undo)
		{
			gainSaved = false;
		}
	}

	void Plugin::apply_parameter(LV2_URID target, const LV2_Atom* value)
	{
		if (value == nullptr)
			return;

		if (value->type == uris.atom_Path && value->size > 0
			&& value->size < MAX_FILE_NAME)
		{
			if (target == uris.model_Path)
			{
				LV2LoadModelMsg msg = { kWorkTypeLoad, {} };
				memcpy(msg.path, value + 1, value->size);
				pendingIndex.store(-1, std::memory_order_release);
				schedule->schedule_work(schedule->handle, sizeof(msg), &msg);
				return;
			}

			for (int i = 0; i < NUM_FAVS; i++)
			{
				if (target != uris.fav_Path[i])
					continue;

				LV2FavPathMsg msg = { kWorkTypeFavSetPath, i, {} };
				memcpy(msg.path, value + 1, value->size);
				schedule->schedule_work(schedule->handle, sizeof(msg), &msg);
				return;
			}
		}

	}

	void Plugin::hmi_addressed(LV2_Handle handle, uint32_t index,
		LV2_HMI_Addressing addressing, const LV2_HMI_AddressingInfo* info)
	{
		auto nam = static_cast<NAM::Plugin*>(handle);

		if (index >= NUM_PORTS_TOTAL)
			return;

		nam->hmiAddr[index] = addressing;

		// Announced capabilities. An info that is absent OR entirely zero is
		// taken as "everything allowed": refusing to write then means never
		// displaying anything, and at worst the firmware ignores the sends.
		const int capsAnnounced = (info != nullptr) ? (int)info->caps : 0;

		nam->hmiCaps[index] = (capsAnnounced != 0) ? capsAnnounced
			: (LV2_HMI_AddressingCapability_LED
				| LV2_HMI_AddressingCapability_Label
				| LV2_HMI_AddressingCapability_Value
				| LV2_HMI_AddressingCapability_Unit
				| LV2_HMI_AddressingCapability_Indicator);

		nam->hmiMomentary[index] = (info != nullptr)
			&& (info->flags & LV2_HMI_AddressingFlag_Momentary) != 0;

		// The firmware re-addresses the widgets on EVERY page change: we use
		// that as the signal to forget the caches and redraw everything.
		for (int k = 0; k < NUM_PORTS_TOTAL; k++)
		{
			nam->hmiLbl[k][0] = '\0';
			nam->hmiVal[k][0] = '\0';
		}

		nam->screenFav.store(-1, std::memory_order_release);
	}

	void Plugin::hmi_unaddressed(LV2_Handle handle, uint32_t index)
	{
		auto nam = static_cast<NAM::Plugin*>(handle);

		// Writing to a withdrawn addressing is forbidden: forget it at once.
		if (index < NUM_PORTS_TOTAL)
		{
			nam->hmiAddr[index] = nullptr;
			nam->hmiCaps[index] = 0;
			nam->hmiMomentary[index] = false;
		}
	}

	// A favorite's displayable name: the one picked in the list, otherwise the
	// file name without its folder or its extension. Capitals and no accents:
	// the firmware draws nothing above 127, and a footswitch label holds seven
	// characters -- measured on the bench, not a matter of taste.
	void Plugin::fav_display_name(int fav, char* out, size_t size) const
	{
		out[0] = '\0';

		if (fav < 1 || fav > NUM_FAVS)
			return;

		// Two sources, in this order:
		//  1. the name PICKED IN THE LIST -- the only channel that crosses the
		//     Starless image, where a string parameter does not come back down
		//     to the plugin, while an enumerated control port always does;
		//  2. the file name, without folder or extension.
		const char* src = "";

		const int choice = favNameChoice[fav - 1].load(std::memory_order_acquire);

		if (choice > 0 && choice < FAV_NAME_COUNT)
			src = FAV_NAME_TABLE[choice];

		if (src[0] == '\0')
		{
			const char* p = favPaths[fav - 1];
			const char* barre = strrchr(p, '/');
			src = (barre != nullptr) ? barre + 1 : p;
		}

		size_t j = 0;
		for (size_t i = 0; src[i] != '\0' && j < size - 1; i++)
		{
			const unsigned char ch = (unsigned char)src[i];

			if (ch >= 128)
				continue;

			out[j++] = (char)toupper(ch);
		}
		out[j] = '\0';

		// cut the extension off a file name
		char* point = strrchr(out, '.');
		if (point != nullptr && point != out)
			*point = '\0';
	}

	void Plugin::write_screen(int fav, bool changed)
	{
		if (hmi == nullptr)
			return;

		char name[MAX_FAV_NAME];
		fav_display_name(fav, name, sizeof(name));

		if (name[0] == '\0')
			memcpy(name, "NONE", 5);

		// footswitch label: seven usable characters
		char shortLabel[8];
		size_t n = strnlen(name, 7);
		memcpy(shortLabel, name, n);
		shortLabel[n] = '\0';

		const int idxNext = IDX_FAV_BROWSE;

		if (changed && fav >= 1)
		{
			// full screen first: it sets the title as a label and freezes the rest
			char title[16];
			snprintf(title, sizeof(title), "FAV %d", fav);
			hmi_popup(idxNext, title, name);
			return;
		}

		hmi_label(idxNext, shortLabel);

		char val[8];
		if (fav >= 1)
			snprintf(val, sizeof(val), "F%d", fav);
		else
			memcpy(val, "--", 3);

		hmi_value(idxNext, val);

		// Every favorite switch carries ITS OWN name, not "Fav 3".
		// The cache filters the sends: nothing goes out if nothing changes.
		for (int i = 0; i < NUM_FAVS; i++)
		{
			const int port = IDX_FAV_FIRST + i;

			if (hmiAddr[port] == nullptr)
				continue;

			char nameI[MAX_FAV_NAME];
			fav_display_name(i + 1, nameI, sizeof(nameI));

			char shortI[8];
			size_t k = strnlen(nameI[0] != '\0' ? nameI : "VIDE", 7);
			memcpy(shortI, nameI[0] != '\0' ? nameI : "VIDE", k);
			shortI[k] = '\0';

			hmi_label(port, shortI);
			hmi_value(port, (fav == i + 1) ? "ACTIF" : "-");
		}
	}

	void Plugin::write_fav_path(int slot)
	{
		if (slot < 0 || slot >= NUM_FAVS)
			return;

		const uint32_t len = (uint32_t)strnlen(favPaths[slot], MAX_FILE_NAME);

		LV2_Atom_Forge_Frame frame;

		lv2_atom_forge_frame_time(&atom_forge, 0);
		lv2_atom_forge_object(&atom_forge, &frame, 0, uris.patch_Set);

		lv2_atom_forge_key(&atom_forge, uris.patch_property);
		lv2_atom_forge_urid(&atom_forge, uris.fav_Path[slot]);
		lv2_atom_forge_key(&atom_forge, uris.patch_value);
		lv2_atom_forge_path(&atom_forge, favPaths[slot], len + 1);

		lv2_atom_forge_pop(&atom_forge, &frame);
	}

	// Puts the favorites into the state: the paths first, one per line, then
	// the ranks picked in the name list, comma separated. Two distinct keys --
	// a state written by an earlier version has no second key, and its
	// favorites simply start again on AUTO.
	void Plugin::store_favorites(LV2_State_Store_Function store, LV2_State_Handle handle)
	{
		std::string paths;
		std::string names;

		for (int i = 0; i < NUM_FAVS; i++)
		{
			paths.append(favPaths[i], strnlen(favPaths[i], MAX_FILE_NAME));
			paths += '\n';

			if (i != 0)
				names += ',';

			names += std::to_string(favNameChoice[i].load(std::memory_order_acquire));
		}

		store(handle, uris.favs_String, paths.c_str(), paths.size() + 1,
			uris.atom_String, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);

		store(handle, uris.favNames_String, names.c_str(), names.size() + 1,
			uris.atom_String, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
	}

	// Puts the ranks taken from the state back into the control ports. A plugin
	// cannot write to its own input ports, but the kx extension lets it ASK the
	// host -- the same recipe as for the gains.
	// Without this the panel's list would show AUTO while the machine's screen
	// already knows the right name.
	void Plugin::restore_name_ports()
	{
		if (portreq == nullptr || portreq->request_change == nullptr)
		{
			kxState = 0;	// the host does not provide the extension
			return;
		}

		for (int i = 0; i < NUM_FAVS; i++)
		{
			if (ports.favNamePort[i] == nullptr)
				continue;

			const int rank = favNameChoice[i].load(std::memory_order_acquire);

			if ((float)rank == *(ports.favNamePort[i]))
				continue;	// the host already has the right value

			const uint32_t index = (uint32_t)(56 + i);	// ports fav_name_1..10

			const int rep = portreq->request_change(portreq->handle, index, (float)rank);
			kxState = (rep == LV2_CONTROL_INPUT_PORT_CHANGE_SUCCESS) ? 2 : -1;
		}
	}

	void Plugin::write_current_path()
	{
		LV2_Atom_Forge_Frame frame;

		lv2_atom_forge_frame_time(&atom_forge, 0);
		lv2_atom_forge_object(&atom_forge, &frame, 0, uris.patch_Set);

		lv2_atom_forge_key(&atom_forge, uris.patch_property);
		lv2_atom_forge_urid(&atom_forge, uris.model_Path);
		lv2_atom_forge_key(&atom_forge, uris.patch_value);
		lv2_atom_forge_path(&atom_forge, currentModelPath.c_str(), (uint32_t)currentModelPath.length() + 1);

		lv2_atom_forge_pop(&atom_forge, &frame);
	}
}

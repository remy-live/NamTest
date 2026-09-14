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

		// tout annoncer au premier passage : sans cela l'hote ne sait rien des
		// chemins et ne les ecrira pas dans la pedalboard
		favDirty.store((1u << NUM_FAVS) - 1, std::memory_order_release);

		// seuil de l'appui long converti une fois pour toutes en echantillons
		longPressSamples = (uint32_t)(sampleRate * LONG_PRESS_MS / 1000.0);
		longPressMaxSamples = (uint32_t)(sampleRate * LONG_PRESS_MAX_MS / 1000.0);
		ecranPeriode = (uint32_t)(sampleRate * 0.25);	// 4 rafraichissements par seconde
		popupHold = (uint64_t)(sampleRate * 3.0);	// silence de 3 s pendant un plein ecran
		tracePeriode = (uint32_t)(sampleRate * 2.0);	// etat ecrit toutes les 2 s

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

				// 1 des l'initialisation : sinon 0 melange "l'hote ne le
				// permet pas" et "on n'a jamais demande"
				kxEtat = (portreq != nullptr && portreq->request_change != nullptr) ? 1 : 0;
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
		// Rang choisi dans la liste pour chaque favori. Cle SEPAREE des chemins :
		// un etat ancien n'en a pas, et ce qu'on y lit reste clair a l'oeil nu.
		uris.favNames_String = map->map(map->handle, PlUGIN_URI "#favnames");

		{
			// PIEGE PAYE CHER : cette table etait ecrite a la main avec HUIT
			// entrees quand NUM_FAVS est passe a DIX. Les deux dernieres
			// valaient nullptr, la fonction de correspondance les
			// deferencait, et le serveur audio mourait a l'ajout de l'effet.
			// Elle est desormais construite a partir de NUM_FAVS, donc elle ne
			// peut plus se desynchroniser.
			static const char* favUris[NUM_FAVS] = {
				PlUGIN_URI "#fav1", PlUGIN_URI "#fav2", PlUGIN_URI "#fav3",
				PlUGIN_URI "#fav4", PlUGIN_URI "#fav5", PlUGIN_URI "#fav6",
				PlUGIN_URI "#fav7", PlUGIN_URI "#fav8", PlUGIN_URI "#fav9",
				PlUGIN_URI "#fav10"
			};

			static_assert(sizeof(favUris) / sizeof(favUris[0]) == NUM_FAVS,
				"la table des URI de favoris doit compter exactement NUM_FAVS entrees");

			for (int i = 0; i < NUM_FAVS; i++)
			{
				// garde-fou : ne jamais passer un pointeur nul a la table des URI
				uris.fav_Path[i] = (favUris[i] != nullptr)
					? map->map(map->handle, favUris[i]) : 0;
			}
		}

		if (options != nullptr)
			options_set(this, options);

		return true;
	}

	// runs on non-RT, can block or use [de]allocations

	// Parcourt MODEL_DIR sur deux niveaux et garde les fichiers de modele.
	// Tourne dans le thread du WORKER, jamais dans le thread audio.
	// d_type plutot que stat() : stat n'existe comme symbole que depuis
	// GLIBC_2.33 alors que la machine plafonne a 2.27.
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

		// std::function pour pouvoir recurser : une lambda auto ne peut pas
		// s'appeler elle-meme (son type n'est pas encore deduit)
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
					// systeme de fichiers qui ne renseigne pas d_type :
					// un opendir d'essai tranche, sans appeler stat.
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

		// Retrouver le rang du modele deja charge, s'il est dans la liste
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

		// rang de chaque favori, recalcule sur le CHEMIN : ajouter ou retirer
		// d'autres modeles ne casse donc aucun favori
		for (int f = 0; f < NUM_FAVS; f++)
		{
			int rang = -1;

			if (favPaths[f][0] != '\0')
			{
				for (size_t i = 0; i < modelFiles.size(); i++)
				{
					if (modelFiles[i] == favPaths[f])
					{
						rang = (int)i;
						break;
					}
				}
			}

			favIndexes[f].store(rang, std::memory_order_release);
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

					// la journalisation ne ressort pas sur le Dwarf : on rend
					// l'echec visible par un port de sortie
					nam->loadStatus.store(-1, std::memory_order_release);
					nam->pendingIndex.store(-1, std::memory_order_release);
				}

				respond(handle, sizeof(response), &response);

				return result;
			}

			case kWorkTypeAutoGain:
			{
				static_cast<NAM::Plugin*>(instance)->mesurer_niveaux();

				return LV2_WORKER_SUCCESS;
			}

			case kWorkTypeTrace:
			{
				auto nam = static_cast<NAM::Plugin*>(instance);

				FILE* f = fopen(TRACE_PATH, "w");

				if (f == nullptr)
					return LV2_WORKER_SUCCESS;

				fprintf(f, "build      %s\n", nam_build_tag());
				fprintf(f, "hmi        %s\n", nam->hmi ? "oui" : "NON");

				if (nam->hmi != nullptr)
				{
					fprintf(f, "hmi.size   %lu (seuil popup %lu)\n",
						(unsigned long)nam->hmi->size,
						(unsigned long)LV2_HMI_WIDGETCONTROL_SIZE_POPUP_MESSAGE);
					fprintf(f, "set_label  %s\n", nam->hmi->set_label ? "present" : "ABSENT");
					fprintf(f, "set_value  %s\n", nam->hmi->set_value ? "present" : "ABSENT");
					fprintf(f, "popup_msg  %s\n", nam->hmi->popup_message ? "present" : "ABSENT");
				}

				fprintf(f, "\nports assignes (index: caps momentane)\n");

				bool aucun = true;
				for (int k = 0; k < NUM_PORTS_TOTAL; k++)
				{
					if (nam->hmiAddr[k] == nullptr)
						continue;

					aucun = false;
					fprintf(f, "  port %-3d caps=%d%s%s%s%s%s momentane=%s\n", k,
						nam->hmiCaps[k],
						(nam->hmiCaps[k] & LV2_HMI_AddressingCapability_LED) ? " led" : "",
						(nam->hmiCaps[k] & LV2_HMI_AddressingCapability_Label) ? " LIBELLE" : "",
						(nam->hmiCaps[k] & LV2_HMI_AddressingCapability_Value) ? " valeur" : "",
						(nam->hmiCaps[k] & LV2_HMI_AddressingCapability_Unit) ? " unite" : "",
						(nam->hmiCaps[k] & LV2_HMI_AddressingCapability_Indicator) ? " barre" : "",
						nam->hmiMomentary[k] ? "oui" : "non");
				}

				if (aucun)
					fprintf(f, "  AUCUN -- refaire l'assignation apres installation\n");

				fprintf(f, "\npatch:Put recus       %d\n", nam->putVus.load(std::memory_order_acquire));
				fprintf(f, "\nkx (ecriture des boutons) : %d  (0 absent, 1 present, 2 accepte, -1 refuse)\n",
					nam->kxEtat);
				fprintf(f, "\nauto-gain  etat=%d (-1 echec, 0 jamais lance, 1 en cours, 2 fait)\n",
					nam->autoState.load(std::memory_order_acquire));

				for (int i = 0; i < NUM_FAVS; i++)
				{
					fprintf(f, "  fav %-2d correction %+.2f dB  fichier %s\n", i + 1,
						nam->autoGainMilli[i].load(std::memory_order_acquire) / 1000.0,
						nam->favPaths[i][0] != '\0' ? nam->favPaths[i] : "(vide)");
				}

				fprintf(f, "\nfav actif  %d\n", nam->activeFav.load(std::memory_order_acquire));
				fprintf(f, "libelle 31 \"%s\"\n", nam->hmiLbl[IDX_FAV_BROWSE]);
				fprintf(f, "valeur  31 \"%s\"\n", nam->hmiVal[IDX_FAV_BROWSE]);

				for (int i = 0; i < NUM_FAVS; i++)
				{
					char n[MAX_FAV_NAME];
					nam->nom_favori(i + 1, n, sizeof(n));

					const int choix =
						nam->favNameChoice[i].load(std::memory_order_acquire);

					fprintf(f, "fav %-2d nom=\"%s\" liste=%d \"%s\"\n", i + 1, n, choix,
						(choix > 0 && choix < FAV_NAME_COUNT)
							? FAV_NAME_TABLE[choix] : "AUTO");
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

					// la liste boucle : apres le dernier on revient au premier
					while (index < 0)
						index += count;

					index %= count;
				}

				LV2LoadModelMsg load = { kWorkTypeLoad, {} };
				const std::string& path = nam->modelFiles[index];

				if (path.size() >= MAX_FILE_NAME)
					return LV2_WORKER_SUCCESS;

				memcpy(load.path, path.c_str(), path.size() + 1);

				// on n'inscrit PAS encore le rang : il ne vaudra que si le
				// chargement aboutit, sinon Current avancerait sans que le son change
				nam->pendingIndex.store(index, std::memory_order_release);
				nam->loadStatus.store(1, std::memory_order_release);
				nam->activeFav.store(0, std::memory_order_release);	// on quitte les favoris

				return work(instance, respond, handle, sizeof(load), &load);
			}

			case kWorkTypeFavSetPath:
			{
				auto msg = static_cast<const LV2FavPathMsg*>(data);
				auto nam = static_cast<NAM::Plugin*>(instance);

				if (msg->slot < 0 || msg->slot >= NUM_FAVS)
					return LV2_WORKER_SUCCESS;

				memcpy(nam->favPaths[msg->slot], msg->path, MAX_FILE_NAME);

				int rang = -1;
				for (size_t i = 0; i < nam->modelFiles.size(); i++)
				{
					if (nam->modelFiles[i] == nam->favPaths[msg->slot])
					{
						rang = (int)i;
						break;
					}
				}
				nam->favIndexes[msg->slot].store(rang, std::memory_order_release);

				return LV2_WORKER_SUCCESS;
			}

			case kWorkTypeFavStore:
			{
				auto msg = static_cast<const LV2FavMsg*>(data);
				auto nam = static_cast<NAM::Plugin*>(instance);

				if (msg->slot < 0 || msg->slot >= NUM_FAVS)
					return LV2_WORKER_SUCCESS;

				// Le rang vient du message : il a ete saisi au moment de l'appui,
				// donc AVANT que ce meme appui ne charge le favori.
				const int index = msg->index;
				const std::string& src = (index >= 0 && index < (int)nam->modelFiles.size())
					? nam->modelFiles[index] : nam->currentModelPath;

				if (src.size() < MAX_FILE_NAME)
				{
					memcpy(nam->favPaths[msg->slot], src.c_str(), src.size() + 1);

					// prevenir l'interface : le selecteur de ce favori doit
					// afficher le nouveau nom
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
					// emplacement vide : on ne touche a rien
					nam->loadStatus.store(-1, std::memory_order_release);

					return LV2_WORKER_SUCCESS;
				}

				// retrouver son rang pour que Current reste coherent
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

		// ---- navigation dans les modeles du dossier ----------------------
		// Le thread audio ne lit JAMAIS la liste : il envoie un rang au worker,
		// qui resout le chemin et charge. Aucun verrou, aucune allocation ici.

		if (!scanRequested)
		{
			// premier passage : demander le scan du dossier
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

		// Next et Prev n'ont jamais agi qu'au front montant : c'est deja le bon
		// comportement pour un momentane comme pour un clic du navigateur qui
		// laisse le bouton a 1.
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
			// premier passage : on prend la valeur telle quelle sans rien charger,
			// sinon un port a zero ecraserait le modele restaure de l'etat
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

		// ---- parcours sans charger, puis validation --------------------
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

		// ---- les huit favoris : court = charger, long = ranger ----------
		for (int i = 0; i < NUM_FAVS; i++)
		{
			if (ports.fav[i] == nullptr)
				continue;

			const float now = *(ports.fav[i]);

			if (!favSeen[i])
			{
				// premier passage : on note la valeur sans rien declencher
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
					// APPUI LONG RELACHE : on range ce qui jouait avant l'appui.
					// Le rangement attend le relachement, et refuse au-dela du
					// plafond : un bouton laisse sur ON dans le navigateur ne
					// peut donc plus ecraser un favori tout seul.
					LV2FavMsg msg = { kWorkTypeFavStore, i,
						favPressIndex[i], favPressGain[i] };
					schedule->schedule_work(schedule->handle, sizeof(msg), &msg);
					storeCount.fetch_add(1, std::memory_order_release);
				}
				else if (!(relachement && favHeldSamples[i] < longPressMaxSamples)
					&& activeFav.load(std::memory_order_acquire) != i + 1)
				{
					// Charge ce favori -- sauf au relachement d'un switch
					// momentane, et sauf s'il joue deja : recharger un modele
					// pour rien coute tres cher en CPU.
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

		// ---- gain du favori actif --------------------------------------
		// Chaque favori a MAINTENANT son propre port de gain : l'hote le
		// sauvegarde avec la pedalboard, et le bouton montre toujours la vraie
		// valeur. Plus de divergence possible entre l'affichage et le son.
		{
			const int fav = activeFav.load(std::memory_order_acquire);
			float g = 0.0f;

			if (fav >= 1 && fav <= NUM_FAVS)
			{
				if (ports.favGain[fav - 1] != nullptr)
					g = *(ports.favGain[fav - 1]);

				// La correction n'est plus ajoutee ici : depuis qu'on sait la
				// DEMANDER a l'hote, elle vit dans le bouton lui-meme. Sinon
				// elle compterait deux fois.
			}
			else if (ports.fav_gain != nullptr)
				g = *(ports.fav_gain);	// hors favori : le gain general

			appliedGainMilli.store((int)(g * 1000.0f), std::memory_order_release);
		}

		// ---- parcours des seuls favoris --------------------------------
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
			const bool piedBref = relachement && favBrowseHeld < longPressMaxSamples;

			prevFavBrowse = favBrowseNow;

			if (!relachement)
				favBrowseHeld = 0;

			// Le drapeau Momentary du host arrive VIDE sur cette machine : on
			// reconnait donc l'appui du pied a la DUREE de l'etat haut. Un
			// relachement apres un etat haut bref suit un appui qui a deja agi,
			// on l'ignore -- sinon deux crans et deux chargements par pression.
			// Un bouton du navigateur reste haut bien plus longtemps : le
			// decocher agit normalement.
			if (!piedBref)
			{

			// aller au favori NON VIDE suivant, en repartant du debut au bout
			const int depart = activeFav.load(std::memory_order_acquire);

			int longueur = ports.cycle_count != nullptr ? (int)*(ports.cycle_count) : NUM_FAVS;
			if (longueur < 1) longueur = 1;
			if (longueur > NUM_FAVS) longueur = NUM_FAVS;

			bool trouve = false;

			for (int pas = 1; pas <= longueur; pas++)
			{
				const int cible = ((depart - 1 + pas) % longueur + longueur) % longueur;

				if (favPaths[cible][0] == '\0')
					continue;

				LV2FavMsg msg = { kWorkTypeFavLoad, cible, -1, 0 };
				schedule->schedule_work(schedule->handle, sizeof(msg), &msg);
				trouve = true;
				break;
			}

			if (!trouve)
			{
				// aucun favori rempli dans le cycle : le silence complet laissait
				// croire a une panne, on le signale par le port d'etat
				loadStatus.store(-1, std::memory_order_release);
			}
			}
		}

		// ---- annoncer a l'interface les favoris ranges au pied ----------
		{
			unsigned bits = favDirty.exchange(0, std::memory_order_acquire);

			for (int i = 0; i < NUM_FAVS && bits != 0; i++)
			{
				if (bits & (1u << i))
					write_fav_path(i);
			}

			if (bits != 0)
			{
				// un chemin a change : vider le cache d'affichage,
				// sinon l'ecran garderait l'ancien libelle indefiniment
				for (int k = 0; k < NUM_PORTS_TOTAL; k++)
				{
					hmiLbl[k][0] = '\0';
					hmiVal[k][0] = '\0';
				}
			}
		}

		// ---- enregistrement depuis l'interface web ---------------------
		// L'interface web ecrit 1 puis 0 dans le port entre deux cycles audio :
		// le front montant n'est jamais vu ici. On declenche donc sur tout
		// CHANGEMENT de valeur, montant ou descendant.
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
			// ---- le nom d'un favori : un rang dans la liste ----------------
			// L'hote garde ce rang avec la pedalboard comme n'importe quel
			// reglage ; on en tient une copie, seule consultee par l'ecran, et
			// tout changement repeint.
			for (int i = 0; i < NUM_FAVS; i++)
			{
				if (ports.favNamePort[i] == nullptr)
					continue;

				const float v = *(ports.favNamePort[i]);

				if (!favNameSeen[i])
				{
					favNameSeen[i] = true;
					prevFavName[i] = v;

					// Valeur deja posee par l'hote : elle l'emporte sur celle
					// qui vient de l'etat. Zero veut dire AUTO, c'est-a-dire
					// « rien de choisi » : le rang restaure survit alors.
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
				ecranFav.store(-1, std::memory_order_release);
			}

			// un etat vient d'etre repris : reposer les rangs dans les ports,
			// une seule fois, et APRES avoir regarde ce que l'hote y avait mis
			if (nomsARemettre.exchange(false, std::memory_order_acquire))
				remettre_noms();

			const int fav = activeFav.load(std::memory_order_acquire);
			const int aFaire = ecranFav.load(std::memory_order_acquire);

			hmiPos += n_samples;
			ecranCompteur += n_samples;
			traceCompteur += n_samples;

			if (traceCompteur >= tracePeriode)
			{
				traceCompteur = 0;
				LV2ScanMsg t = { kWorkTypeTrace };
				schedule->schedule_work(schedule->handle, sizeof(t), &t);
			}

			// Deux raisons d'ecrire : le favori a change, ou le temps est venu.
			// Le rafraichissement periodique est indispensable -- le firmware
			// redessine ON/OFF a chaque changement de valeur du port et efface
			// notre libelle. 4 envois/s, tres en dessous du budget de 25.
			if (aFaire != fav || ecranCompteur >= ecranPeriode || ecranForce)
			{
				// le plein ecran n'apparait QUE sur un vrai changement de favori
				ecrire_ecran(fav, aFaire != fav && !ecranForce);
				ecranForce = false;
				ecranFav.store(fav, std::memory_order_release);
				ecranCompteur = 0;
			}
		}

		if (ports.active_fav != nullptr)
			*(ports.active_fav) = (float)activeFav.load(std::memory_order_acquire);

		for (int i = 0; i < NUM_FAVS; i++)
		{
			if (ports.favIndex[i] != nullptr)
				*(ports.favIndex[i]) = (float)favIndexes[i].load(std::memory_order_acquire);
		}

		// ---- un bouton de rangement par favori --------------------------
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

			// Tout changement range, montant ou descendant : un clic du
			// navigateur peut arriver dans les deux sens, et le rangement est
			// idempotent -- le refaire deux fois ne coute rien.
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

		// ---- selection du favori par tension ----------------------------
		// La plage 0-10 V est decoupee en NUM_FAVS bandes egales. On ne charge
		// qu'au CHANGEMENT de bande, avec une marge pour qu'une tension posee
		// sur une frontiere ne fasse pas osciller le chargement.
		if (ports.cv_select != nullptr && n_samples > 0)
		{
			// LIRE LE PREMIER ECHANTILLON, jamais le dernier : quand rien n'est
			// branche sur l'entree CV, l'hote peut n'allouer qu'une seule valeur.
			// Lire l'indice n_samples-1 sortait alors du tampon -- et rebrancher
			// les ports, ce que fait le deplacement d'un bloc, deplace ces
			// tampons. C'est une lecture hors limites, donc un plantage.
			const float volts = ports.cv_select[0];
			const float largeur = 10.0f / (float)NUM_FAVS;

			int bande = (int)(volts / largeur);

			if (bande < 0) bande = 0;
			if (bande >= NUM_FAVS) bande = NUM_FAVS - 1;

			// marge : il faut depasser le dixieme de bande pour basculer
			const float centre = ((float)bande + 0.5f) * largeur;
			const float ecart = volts > centre ? volts - centre : centre - volts;

			if (bande != cvBande && ecart < largeur * 0.4f)
			{
				cvBande = bande;

				if (favPaths[bande][0] != '\0')
				{
					LV2FavMsg msg = { kWorkTypeFavLoad, bande, -1, 0 };
					schedule->schedule_work(schedule->handle, sizeof(msg), &msg);
				}
			}
		}

		// ---- lancement de la mesure automatique -------------------------
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

		// ---- appliquer ou annuler les gains mesures ---------------------
		if (appliquerDemande.exchange(false, std::memory_order_acquire))
		{
			for (int i = 0; i < NUM_FAVS; i++)
				gainAvant[i] = ports.favGain[i] != nullptr ? *(ports.favGain[i]) : 0.0f;

			gainSauve = true;
			ecrire_gains(false);
		}

		const float applyNow = ports.auto_apply != nullptr ? *(ports.auto_apply) : 0.0f;

		if (!autoApplySeen) { autoApplySeen = true; prevAutoApply = applyNow; }
		else if (applyNow != prevAutoApply)
		{
			prevAutoApply = applyNow;

			// garder les valeurs d'avant pour pouvoir revenir en arriere
			for (int i = 0; i < NUM_FAVS; i++)
			{
				gainAvant[i] = ports.favGain[i] != nullptr ? *(ports.favGain[i]) : 0.0f;
			}

			gainSauve = true;
			ecrire_gains(false);
		}

		const float undoNow = ports.auto_undo != nullptr ? *(ports.auto_undo) : 0.0f;

		if (!autoUndoSeen) { autoUndoSeen = true; prevAutoUndo = undoNow; }
		else if (undoNow != prevAutoUndo)
		{
			prevAutoUndo = undoNow;
			ecrire_gains(true);
		}

		// ---- canal de retour : la correction mesuree, favori par favori --
		// Le panneau n'a qu'une valeur a la fois : on fait tourner un favori
		// par seconde. db_slot dit duquel il s'agit, auto_db_slot porte sa
		// correction. Les noms ne passent plus par la : ils sont dans un port
		// de controle, que l'interface lit directement.
		rotationCompteur += n_samples;

		if (rotationCompteur >= (uint32_t)sampleRate)
		{
			rotationCompteur = 0;
			rotationSlot = (rotationSlot + 1) % NUM_FAVS;
		}

		// La correction part EN PREMIER, le numero du favori un dixieme de
		// seconde plus tard. C'est lui qui declenche l'affichage cote
		// navigateur : il arrive donc toujours apres la valeur qu'il designe.
		// Deux ports changes dans le meme cycle parviennent au navigateur dans
		// un ordre que rien ne garantit, et la correction se posait alors dans
		// la case du favori precedent. Ce numero change a chaque tour, meme
		// quand deux favoris ont la meme correction : l'affichage se refait
		// toujours.
		if (ports.auto_db_slot != nullptr)
			*(ports.auto_db_slot) =
				autoGainMilli[rotationSlot].load(std::memory_order_acquire) / 1000.0f;

		if (ports.db_slot != nullptr
			&& rotationCompteur >= (uint32_t)(sampleRate * 0.1))
			*(ports.db_slot) = (float)(rotationSlot + 1);

		if (ports.kx_state != nullptr)
			*(ports.kx_state) = (float)kxEtat;

		if (ports.hmi_state != nullptr)
		{
			// Un seul nombre qui dit tout : unites = etat, dizaines et au-dela
			// = capacites annoncees par le firmware pour Fav Next.
			//   etat : 0 pas de HMI, 1 HMI sans assignation, 2 assigne,
			//          3 assigne + popup disponible
			//   caps : 1 LED, 2 libelle, 4 valeur, 8 unite, 16 indicateur
			// Exemple : 36 = capacites 3 (LED+libelle) et etat 6... non :
			// on lit 2 + 10 x caps. 62 = caps 6 (libelle+valeur), etat 2.
			int etat = 0;
			int caps = 0;

			if (hmi != nullptr)
			{
				etat = 1;

				if (hmiAddr[IDX_FAV_BROWSE] != nullptr)
				{
					etat = 2;
					caps = hmiCaps[IDX_FAV_BROWSE];

					if ((hmi->size == 0 || hmi->size >= LV2_HMI_WIDGETCONTROL_SIZE_POPUP_MESSAGE)
						&& hmi->popup_message != nullptr)
						etat = 3;
				}
			}

			*(ports.hmi_state) = (float)(etat + 10 * caps);
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
					// UN PUT POUSSE UN OBJET ENTIER, propriete par propriete,
					// dans un corps. C'est la forme qu'un hote emploie pour
					// transmettre un etat complet -- un plugin qui ne lit que
					// les Set jette ces valeurs en silence.
					putVus.fetch_add(1, std::memory_order_release);

					const LV2_Atom_Object* corps = NULL;

					lv2_atom_object_get(obj, uris.patch_body, &corps, 0);

					if (corps != nullptr && corps->atom.type == uris.atom_Object)
					{
						LV2_ATOM_OBJECT_FOREACH(corps, prop)
						{
							appliquer_parametre(prop->key, &prop->value);
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
						const LV2_URID cible = ((const LV2_Atom_URID*)property)->body;

						if (cible == uris.model_Path)
						{
							LV2LoadModelMsg msg = { kWorkTypeLoad, {} };
							memcpy(msg.path, file_path + 1, file_path->size);
							pendingIndex.store(-1, std::memory_order_release);
							schedule->schedule_work(schedule->handle, sizeof(msg), &msg);
						}
						else
						{
							// l'un des huit favoris, choisi dans l'interface web
							for (int i = 0; i < NUM_FAVS; i++)
							{
								if (cible != uris.fav_Path[i])
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
			// pas de modele, mais les favoris meritent d'etre gardes
			nam->ranger_favoris(store, handle);

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

		nam->ranger_favoris(store, handle);

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

		// Reprendre les favoris avant le modele
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

					// Les etats d'avant rangeaient apres une tabulation tantot
					// un gain, tantot un nom tape. Les deux ont disparu : on
					// coupe a la tabulation et on ne garde que le chemin.
					const size_t tab = line.find('\t');

					if (tab != std::string::npos)
						line.resize(tab);

					if (line.size() < MAX_FILE_NAME)
						memcpy(nam->favPaths[i], line.c_str(), line.size() + 1);

					start = cut + 1;
				}
			}
		}

		// ---- le nom choisi dans la liste, favori par favori --------------
		// Sans cela, rouvrir une pedalboard ramenerait tous les favoris a AUTO
		// des que l'hote ne repose pas lui-meme la valeur du port.
		{
			size_t   nsize = 0;
			uint32_t ntype = 0;
			uint32_t nflags = 0;
			const void* noms = retrieve(handle, nam->uris.favNames_String,
				&nsize, &ntype, &nflags);

			if (noms != nullptr && ntype == nam->uris.atom_String && nsize > 0)
			{
				std::string liste(static_cast<const char*>(noms), nsize - 1);
				size_t start = 0;

				for (int i = 0; i < NUM_FAVS; i++)
				{
					size_t cut = liste.find(',', start);

					if (cut == std::string::npos)
						cut = liste.size();

					const std::string champ = liste.substr(start, cut - start);

					if (!champ.empty())
					{
						// zero compris : c'est AUTO, un choix comme un autre
						const int rang = atoi(champ.c_str());

						if (rang >= 0 && rang < FAV_NAME_COUNT)
							nam->favNameChoice[i].store(rang, std::memory_order_release);
					}

					if (cut >= liste.size())
						break;

					start = cut + 1;
				}

				// le thread audio les reposera dans les ports de controle
				nam->nomsARemettre.store(true, std::memory_order_release);
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

	// Annonce a l'hote le chemin d'un favori : c'est ce qui fait apparaitre le
	// NOM dans le selecteur de l'interface web apres un rangement au pied.
	// Le host previent quand l'utilisateur assigne un port a un actuateur, et
	// donne le jeton a employer pour ecrire sur cet afficheur. Sans ces deux
	// fonctions declarees en extensionData, rien n'arrive jamais.
	// Trois regles reprises du looper, chacune payee par un defaut observe :
	//  1. n'envoyer que ce qui a CHANGE, pour rester sous le budget d'ecran ;
	//  2. respecter les capacites annoncees a l'assignation ;
	//  3. se TAIRE pendant un plein ecran -- un libelle ecrit apres un popup
	//     repeint la page et efface le message.
	void Plugin::hmi_label(int port, const char* txt)
	{
		if (HMI_DESACTIVE) return;

		if (popup_en_cours()) return;
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
		if (HMI_DESACTIVE) return;

		if (popup_en_cours()) return;
		if (hmi == nullptr || port < 0 || port >= NUM_PORTS_TOTAL) return;
		if (hmiAddr[port] == nullptr || hmi->set_value == nullptr) return;
		if (!(hmiCaps[port] & LV2_HMI_AddressingCapability_Value)) return;
		if (strncmp(hmiVal[port], txt, sizeof(hmiVal[0]) - 1) == 0) return;

		strncpy(hmiVal[port], txt, sizeof(hmiVal[0]) - 1);
		hmiVal[port][sizeof(hmiVal[0]) - 1] = '\0';
		hmi->set_value(hmi->handle, hmiAddr[port], hmiVal[port]);
	}

	void Plugin::hmi_popup(int port, const char* titre, const char* message)
	{
		if (HMI_DESACTIVE) return;

		if (hmi == nullptr || hmi->popup_message == nullptr) return;

		// Le champ size dit ce que le host fournit vraiment, mais CERTAINS
		// HOSTS LE LAISSENT A ZERO : ne refuser que s'il est renseigne ET trop
		// petit. Mon test precedent (size >= ...) bloquait tout dans ce cas.
		if (hmi->size != 0 && hmi->size < LV2_HMI_WIDGETCONTROL_SIZE_POPUP_MESSAGE)
			return;

		// le titre part EN LIBELLE d'abord : ecrit apres, il effacerait le popup
		popupAt = 0;
		hmi_label(port, titre);
		popupAt = hmiPos;

		if (port >= 0 && port < NUM_PORTS_TOTAL && hmiAddr[port] != nullptr)
		{
			hmi->popup_message(hmi->handle, hmiAddr[port],
				LV2_HMI_Popup_Style_Inverted, titre, message);
			return;
		}

		// repli : n'importe quel port assigne fait l'affaire pour un plein ecran
		for (int k = 0; k < NUM_PORTS_TOTAL; k++)
		{
			if (hmiAddr[k] != nullptr)
			{
				hmi->popup_message(hmi->handle, hmiAddr[k],
					LV2_HMI_Popup_Style_Inverted, titre, message);
				return;
			}
		}
	}

	// Applique un couple propriete/valeur, qu'il vienne d'un patch:Set (une
	// propriete a la fois) ou d'un patch:Put (un objet entier d'un coup).
	// log2 approche, ecrit ici parce que log(), log10() et exp() de la glibc
	// sont marques GLIBC_2.29 alors que la machine plafonne a 2.27. Meme piege
	// que pow@GLIBC_2.29 sur l'accordeur. Precision largement suffisante pour
	// un gain exprime en decibels.
	static float nam_log2f(float x)
	{
		if (x <= 0.0f)
			return -127.0f;

		union { float f; uint32_t i; } u;
		u.f = x;

		const float e = (float)((int)((u.i >> 23) & 0xFF) - 127);

		u.i = (u.i & 0x007FFFFFu) | 0x3F800000u;	// mantisse ramenee dans [1,2)
		const float m = u.f;

		const float p = -1.7417939f + (2.8212026f + (-1.4699568f
			+ (0.44717955f - 0.056570851f * m) * m) * m) * m;

		return e + p;
	}

	static float nam_log10f(float x)
	{
		return nam_log2f(x) * 0.30102999566f;
	}

	// Mesure le niveau de sortie de chaque favori et en deduit une correction,
	// pour qu'ils sonnent tous au meme volume.
	//
	// Tourne dans le thread du WORKER : on charge un modele a part, on lui fait
	// traiter un signal d'essai identique pour tous, on mesure l'energie, on
	// libere. Le modele qui joue n'est jamais touche et le son continue.
	//
	// Le signal d'essai est un bruit deterministe (generateur a graine fixe) :
	// deux mesures du meme modele donnent donc exactement le meme resultat.
	void Plugin::mesurer_niveaux()
	{
		static constexpr int N_ESSAI = 8192;	// environ 170 ms a 48 kHz
		static constexpr int N_BLOC = 256;

		autoState.store(1, std::memory_order_release);

		float entree[N_BLOC];
		float sortie[N_BLOC];
		float niveaux[NUM_FAVS];
		bool mesure[NUM_FAVS] = {};
		int combien = 0;

		for (int i = 0; i < NUM_FAVS; i++)
		{
			niveaux[i] = 0.0f;

			if (favPaths[i][0] == '\0')
				continue;

			auto modele = loader.CreateFromFile(favPaths[i]);

			if (modele == nullptr)
				continue;

			// INDISPENSABLE : un modele frais ne sait pas quelle taille de bloc
			// on va lui donner, et ses etats internes sont vides. Sans ces deux
			// appels, le traitement ne donne rien d'exploitable -- c'est ce qui
			// rendait la mesure muette.
			modele->SetMaxAudioBufferSize(N_BLOC);
			modele->Prewarm();

			// niveau recommande par le modele, comme dans le traitement normal
			const float entreeDB = modele->GetRecommendedInputDBAdjustment();
			const float sortieDB = modele->GetRecommendedOutputDBAdjustment();
			const float gainEntree = powf(10.0f, entreeDB * 0.05f);
			const float gainSortie = powf(10.0f, sortieDB * 0.05f);

			uint32_t graine = 12345u;
			double somme = 0.0;
			int comptes = 0;

			for (int fait = 0; fait < N_ESSAI; fait += N_BLOC)
			{
				for (int k = 0; k < N_BLOC; k++)
				{
					// generateur congruentiel : identique a chaque appel
					graine = graine * 1103515245u + 12345u;
					const float bruit = ((float)((graine >> 9) & 0xFFFF) / 32768.0f) - 1.0f;

					entree[k] = bruit * 0.25f * gainEntree;
				}

				modele->Process(entree, sortie, N_BLOC);

				// les premiers blocs servent a remplir les etats internes
				if (fait >= N_BLOC * 4)
				{
					for (int k = 0; k < N_BLOC; k++)
					{
						const float v = sortie[k] * gainSortie;
						somme += (double)v * (double)v;
						comptes++;
					}
				}
			}

			modele = nullptr;	// libere le modele d'essai

			if (comptes > 0 && somme > 0.0)
			{
				niveaux[i] = (float)sqrt(somme / (double)comptes);
				mesure[i] = true;
				combien++;
			}
		}

		if (combien == 0)
		{
			autoState.store(-1, std::memory_order_release);
			return;
		}

		// Reference : la MEDIANE des niveaux mesures. Elle repartit la
		// correction au lieu de tout tirer vers le plus faible, et se calcule
		// sans logarithme -- ceux de la glibc sont hors de portee ici.
		float tries[NUM_FAVS];
		int n = 0;

		for (int i = 0; i < NUM_FAVS; i++)
		{
			if (mesure[i])
				tries[n++] = niveaux[i];
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
			if (!mesure[i])
			{
				autoGainMilli[i].store(0, std::memory_order_release);
				continue;
			}

			float db = 20.0f * nam_log10f(reference / niveaux[i]);

			// borne de securite : jamais plus de 12 dB dans un sens ou l'autre
			if (db > 12.0f) db = 12.0f;
			if (db < -12.0f) db = -12.0f;

			autoGainMilli[i].store((int)(db * 1000.0f), std::memory_order_release);
		}

		autoState.store(2, std::memory_order_release);

		// Mesurer puis appliquer sont UN SEUL geste : demander l'ecriture des
		// boutons des que la mesure aboutit. Le thread audio s'en chargera,
		// l'extension kx ne s'appelle pas depuis le worker.
		appliquerDemande.store(true, std::memory_order_release);
	}

	// Demande a l'hote de poser les gains mesures dans les boutons -- ou de
	// remettre ceux d'avant. Le bouton bouge donc VRAIMENT sous les yeux, et la
	// valeur est sauvegardee avec la pedalboard comme n'importe quel reglage.
	void Plugin::ecrire_gains(bool annuler)
	{
		if (portreq == nullptr || portreq->request_change == nullptr)
		{
			kxEtat = 0;	// l'hote ne fournit pas l'extension
			return;
		}

		kxEtat = 1;

		for (int i = 0; i < NUM_FAVS; i++)
		{
			if (ports.favGain[i] == nullptr)
				continue;

			const uint32_t index = (uint32_t)(34 + i);	// ports fav_gain_1..10

			if (annuler)
			{
				if (gainSauve)
				{
					const int rep = portreq->request_change(portreq->handle, index,
						gainAvant[i]);
					kxEtat = (rep == LV2_CONTROL_INPUT_PORT_CHANGE_SUCCESS) ? 2 : -1;
				}
			}
			else
			{
				const float correction =
					autoGainMilli[i].load(std::memory_order_acquire) / 1000.0f;

				const int rep = portreq->request_change(portreq->handle, index, correction);
				kxEtat = (rep == LV2_CONTROL_INPUT_PORT_CHANGE_SUCCESS) ? 2 : -1;
			}
		}

		if (annuler)
		{
			gainSauve = false;
		}
	}

	void Plugin::appliquer_parametre(LV2_URID cible, const LV2_Atom* valeur)
	{
		if (valeur == nullptr)
			return;

		if (valeur->type == uris.atom_Path && valeur->size > 0
			&& valeur->size < MAX_FILE_NAME)
		{
			if (cible == uris.model_Path)
			{
				LV2LoadModelMsg msg = { kWorkTypeLoad, {} };
				memcpy(msg.path, valeur + 1, valeur->size);
				pendingIndex.store(-1, std::memory_order_release);
				schedule->schedule_work(schedule->handle, sizeof(msg), &msg);
				return;
			}

			for (int i = 0; i < NUM_FAVS; i++)
			{
				if (cible != uris.fav_Path[i])
					continue;

				LV2FavPathMsg msg = { kWorkTypeFavSetPath, i, {} };
				memcpy(msg.path, valeur + 1, valeur->size);
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

		// Capacites annoncees. Un info absent OU entierement a zero est traite
		// comme « tout permis » : refuser d'ecrire dans ce cas revient a ne
		// jamais rien afficher, et au pire le firmware ignore les envois.
		const int capsAnnonces = (info != nullptr) ? (int)info->caps : 0;

		nam->hmiCaps[index] = (capsAnnonces != 0) ? capsAnnonces
			: (LV2_HMI_AddressingCapability_LED
				| LV2_HMI_AddressingCapability_Label
				| LV2_HMI_AddressingCapability_Value
				| LV2_HMI_AddressingCapability_Unit
				| LV2_HMI_AddressingCapability_Indicator);

		nam->hmiMomentary[index] = (info != nullptr)
			&& (info->flags & LV2_HMI_AddressingFlag_Momentary) != 0;

		// Le firmware re-adresse les widgets a CHAQUE changement de page : on
		// s'en sert comme signal pour oublier les caches et tout redessiner.
		for (int k = 0; k < NUM_PORTS_TOTAL; k++)
		{
			nam->hmiLbl[k][0] = '\0';
			nam->hmiVal[k][0] = '\0';
		}

		nam->ecranFav.store(-1, std::memory_order_release);
	}

	void Plugin::hmi_unaddressed(LV2_Handle handle, uint32_t index)
	{
		auto nam = static_cast<NAM::Plugin*>(handle);

		// Interdit d'ecrire dans une addressing retiree : on l'oublie aussitot.
		if (index < NUM_PORTS_TOTAL)
		{
			nam->hmiAddr[index] = nullptr;
			nam->hmiCaps[index] = 0;
			nam->hmiMomentary[index] = false;
		}
	}

	// Ecrit le nom du favori sur le libelle du footswitch Fav Next, et le fait
	// apparaitre en plein ecran. Sept caracteres au maximum sur un footswitch,
	// en capitales et sans accent : mesure du banc, pas une preference.
	// Nom affichable d'un favori : le NOM LIBRE saisi dans l'interface s'il
	// existe, sinon le nom du fichier sans son dossier ni son extension.
	// Capitales, sans accent : le firmware ne dessine pas au-dela de 127.
	void Plugin::nom_favori(int fav, char* sortie, size_t taille) const
	{
		sortie[0] = '\0';

		if (fav < 1 || fav > NUM_FAVS)
			return;

		// Deux sources, dans cet ordre :
		//  1. le nom CHOISI DANS LA LISTE -- seul canal qui traverse l'image
		//     Starless : un parametre de type chaine n'y redescend pas jusqu'au
		//     plugin, alors qu'un port de controle enumere passe toujours ;
		//  2. le nom du fichier, sans dossier ni extension.
		const char* src = "";

		const int choix = favNameChoice[fav - 1].load(std::memory_order_acquire);

		if (choix > 0 && choix < FAV_NAME_COUNT)
			src = FAV_NAME_TABLE[choix];

		if (src[0] == '\0')
		{
			const char* p = favPaths[fav - 1];
			const char* barre = strrchr(p, '/');
			src = (barre != nullptr) ? barre + 1 : p;
		}

		size_t j = 0;
		for (size_t i = 0; src[i] != '\0' && j < taille - 1; i++)
		{
			const unsigned char ch = (unsigned char)src[i];

			if (ch >= 128)
				continue;

			sortie[j++] = (char)toupper(ch);
		}
		sortie[j] = '\0';

		// couper l'extension d'un nom de fichier
		char* point = strrchr(sortie, '.');
		if (point != nullptr && point != sortie)
			*point = '\0';
	}

	void Plugin::ecrire_ecran(int fav, bool changement)
	{
		if (hmi == nullptr)
			return;

		char nom[MAX_FAV_NAME];
		nom_favori(fav, nom, sizeof(nom));

		if (nom[0] == '\0')
			memcpy(nom, "AUCUN", 6);

		// libelle du footswitch : sept caracteres utiles
		char courtLabel[8];
		size_t n = strnlen(nom, 7);
		memcpy(courtLabel, nom, n);
		courtLabel[n] = '\0';

		const int idxNext = IDX_FAV_BROWSE;

		if (changement && fav >= 1)
		{
			// plein ecran d'abord : il pose le titre en libelle et gele le reste
			char titre[16];
			snprintf(titre, sizeof(titre), "FAV %d", fav);
			hmi_popup(idxNext, titre, nom);
			return;
		}

		hmi_label(idxNext, courtLabel);

		char val[8];
		if (fav >= 1)
			snprintf(val, sizeof(val), "F%d", fav);
		else
			memcpy(val, "--", 3);

		hmi_value(idxNext, val);

		// Chaque switch de favori porte SON propre nom, pas « Fav 3 ».
		// Les envois sont filtres par le cache : rien ne part si rien ne change.
		for (int i = 0; i < NUM_FAVS; i++)
		{
			const int port = IDX_FAV_FIRST + i;

			if (hmiAddr[port] == nullptr)
				continue;

			char nomI[MAX_FAV_NAME];
			nom_favori(i + 1, nomI, sizeof(nomI));

			char courtI[8];
			size_t k = strnlen(nomI[0] != '\0' ? nomI : "VIDE", 7);
			memcpy(courtI, nomI[0] != '\0' ? nomI : "VIDE", k);
			courtI[k] = '\0';

			hmi_label(port, courtI);
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

	// Range les favoris dans l'etat : les chemins d'abord, un par ligne, puis
	// les rangs choisis dans la liste des noms, separes par des virgules. Deux
	// cles distinctes -- un etat ecrit par une version d'avant n'a pas la
	// seconde, et ses favoris repartent simplement sur AUTO.
	void Plugin::ranger_favoris(LV2_State_Store_Function store, LV2_State_Handle handle)
	{
		std::string chemins;
		std::string noms;

		for (int i = 0; i < NUM_FAVS; i++)
		{
			chemins.append(favPaths[i], strnlen(favPaths[i], MAX_FILE_NAME));
			chemins += '\n';

			if (i != 0)
				noms += ',';

			noms += std::to_string(favNameChoice[i].load(std::memory_order_acquire));
		}

		store(handle, uris.favs_String, chemins.c_str(), chemins.size() + 1,
			uris.atom_String, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);

		store(handle, uris.favNames_String, noms.c_str(), noms.size() + 1,
			uris.atom_String, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
	}

	// Repose dans les ports de controle les rangs repris de l'etat. Un plugin
	// ne peut pas ecrire dans ses propres ports d'entree, mais l'extension kx
	// permet de le DEMANDER a l'hote -- meme recette que pour les gains.
	// Sans cela, la liste du panneau afficherait AUTO alors que l'ecran de la
	// machine, lui, connait deja le bon nom.
	void Plugin::remettre_noms()
	{
		if (portreq == nullptr || portreq->request_change == nullptr)
		{
			kxEtat = 0;	// l'hote ne fournit pas l'extension
			return;
		}

		for (int i = 0; i < NUM_FAVS; i++)
		{
			if (ports.favNamePort[i] == nullptr)
				continue;

			const int rang = favNameChoice[i].load(std::memory_order_acquire);

			if ((float)rang == *(ports.favNamePort[i]))
				continue;	// l'hote a deja la bonne valeur

			const uint32_t index = (uint32_t)(56 + i);	// ports fav_name_1..10

			const int rep = portreq->request_change(portreq->handle, index, (float)rang);
			kxEtat = (rep == LV2_CONTROL_INPUT_PORT_CHANGE_SUCCESS) ? 2 : -1;
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

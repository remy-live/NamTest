#pragma once

#include <array>
#include <atomic>
#include "lv2-hmi.h"
#include "control-input-port-change-request.h"

extern "C" const char* nam_build_tag(void);
#include "fav_names.h"
#include <string>
#include <vector>
#include <cstddef>
#include <cstdint>
#include <random>
#include <string_view>

// LV2
#include <lv2/core/lv2.h>
#include <lv2/core/lv2_util.h>
#include <lv2/atom/atom.h>
#include <lv2/log/log.h>
#include <lv2/log/logger.h>
#include <lv2/urid/urid.h>
#include <lv2/atom/forge.h>
#include <lv2/buf-size/buf-size.h>
#include <lv2/options/options.h>
#include <lv2/patch/patch.h>
#include <lv2/worker/worker.h>
#include <lv2/state/state.h>
#include <lv2/units/units.h>

#include <NeuralAudio/NeuralModel.h>

#define PlUGIN_URI "http://remy.local/nam-test"
#define MODEL_URI PlUGIN_URI "#model"

namespace NAM {
	static constexpr unsigned int MAX_FILE_NAME = 1024;

	enum LV2WorkType {
		kWorkTypeLoad,
		kWorkTypeSwitch,
		kWorkTypeFree,
		kWorkTypeScan,		// relire le dossier des modeles (hors thread audio)
		kWorkTypeSelect,	// choisir un modele par son rang dans la liste
		kWorkTypeFavLoad,	// charger le favori d'un emplacement
		kWorkTypeFavStore,	// ranger le modele courant dans un emplacement
		kWorkTypeFavSetPath,	// poser le chemin d'un favori (venu de l'interface)
		kWorkTypeFavSetName,	// poser le nom libre d'un favori
		kWorkTypeTrace,		// ecrire l'etat dans un fichier, pour diagnostic
		kWorkTypeAutoGain	// mesurer le niveau de chaque favori, hors temps reel
	};

	// Le journal du plugin ne ressort pas sur cette machine et les ports de
	// sortie ne sont pas lisibles dans le panneau : on ecrit donc l'etat dans
	// un fichier, seul canal qui ne depende ni du navigateur ni du journal.
	static constexpr const char* TRACE_PATH = "/tmp/nam_test_state.txt";

	static constexpr unsigned int MAX_FAV_NAME = 64;

	static constexpr int NUM_FAVS = 10;
	static constexpr int NUM_PORTS_TOTAL = 95;
	// duree au-dela de laquelle un appui devient long, en millisecondes
	// Indices codes en dur, verifies par script contre le descripteur.
	// Interrupteur d'isolement : a true, le plugin n'ecrit plus rien sur l'ecran
	// de la machine. Sert a savoir si le plantage au deplacement d'un bloc vient
	// de la, l'assignation etant prise et rendue par un autre fil.
	static constexpr bool HMI_DESACTIVE = false;

	static constexpr int IDX_FAV_BROWSE = 33;
	static constexpr int IDX_FAV_FIRST = 18;

	static constexpr int LONG_PRESS_MS = 800;
	// Au-dela, ce n'est plus un appui du pied mais un bouton laisse sur ON dans
	// le navigateur : on ne range pas, sinon chaque clic ecraserait un favori.
	static constexpr int LONG_PRESS_MAX_MS = 5000;

	// Dossier scanne pour la navigation au pied
	static constexpr const char* MODEL_DIR = "/data/user-files/NAM Models";
	static constexpr int MAX_MODELS = 200;

	struct LV2ScanMsg {
		LV2WorkType type;
	};

	struct LV2SelectMsg {
		LV2WorkType type;
		int32_t absolute;	// rang vise, ou -1
		int32_t delta;		// deplacement relatif si absolute vaut -1
	};

	struct LV2FavPathMsg {
		LV2WorkType type;
		int32_t slot;
		char path[MAX_FILE_NAME];
	};

	struct LV2FavNameMsg {
		LV2WorkType type;
		int32_t slot;
		char name[MAX_FAV_NAME];
	};

	struct LV2FavMsg {
		LV2WorkType type;
		int32_t slot;		// 0 a NUM_FAVS-1
		int32_t index;		// rang a ranger, saisi AVANT le chargement, ou -1
		int32_t gainMilli;	// gain a ranger, en millidecibels
	};

	struct LV2LoadModelMsg {
		LV2WorkType type;
		char path[MAX_FILE_NAME];
	};

	struct LV2SwitchModelMsg {
		LV2WorkType type;
		char path[MAX_FILE_NAME];
		NeuralAudio::NeuralModel* model;
	};

	struct LV2FreeModelMsg {
		LV2WorkType type;
		NeuralAudio::NeuralModel* model;
	};

	class Plugin {
	public:
		struct Ports {
			const LV2_Atom_Sequence* control;
			LV2_Atom_Sequence* notify;
			const float* audio_in;
			float* audio_out;
			float* input_level;
			float* output_level;
			float* quality_scale;
			// Ports ajoutes a la FIN, indices 7 et suivants.
			// connect_port() indexe cette structure par le numero de port :
			// ne JAMAIS inserer un champ ailleurs qu'a la fin.
			// Ports ajoutes a la FIN, indices 7 et suivants. connect_port()
			// indexe cette structure par le numero de port : l'ordre ci-dessous
			// est celui du descripteur, verifie par script a chaque livraison.
			float* model_index;		// 7
			float* step_next;		// 8
			float* step_prev;		// 9
			float* rescan;			// 10
			float* model_count;		// 11 sortie
			float* current_index;		// 12 sortie
			float* load_status;		// 13 sortie
			float* browse;			// 14
			float* action;			// 15
			float* browse_index;		// 16 sortie
			float* active_fav;		// 17 sortie
			float* fav[NUM_FAVS];		// 18..27
			float* store_slot;		// 28
			float* store;			// 29
			float* fav_gain;		// 30
			float* applied_gain;		// 31 sortie
			float* store_count;		// 32 sortie
			float* fav_browse;		// 33
			float* favGain[NUM_FAVS];	// 34..43
			float* favIndex[NUM_FAVS];	// 44..53 sorties
			float* cycle_count;		// 54
			float* hmi_state;		// 55 sortie
			float* favNamePort[NUM_FAVS];	// 56..65
			float* favStore[NUM_FAVS];	// 66..75
			float* auto_gain;		// 76
			float* auto_state;		// 77 sortie
			float* auto_offset;		// 78 sortie
			float* cv_select;		// 79 entree CV, 0 a 10 V
			float* web_slot;		// 80 favori vise par la saisie
			float* web_char;		// 81 code du caractere
			float* web_strobe;		// 82 tout changement fait lire un caractere
			float* name_slot;		// 83 sortie : de qui n1..n7 portent le nom
			float* nameChar[7];		// 84..90 sorties : les sept caracteres
			float* auto_db_slot;		// 91 sortie : correction de ce favori
			float* auto_apply;		// 92 ecrire les corrections dans les gains
			float* auto_undo;		// 93 remettre les gains d'avant
			float* kx_state;		// 94 sortie : 0 absent, 1 present, 2 accepte, -1 refuse
		};

		Ports ports = {};

		double sampleRate;

		LV2_URID_Map* map = nullptr;
		LV2_Log_Logger logger = {};
		LV2_Worker_Schedule* schedule = nullptr;

		NeuralAudio::NeuralModelLoader loader;
		NeuralAudio::NeuralModel* currentModel = nullptr;
		std::string currentModelPath;
		float prevDCInput = 0;
		float prevDCOutput = 0;

		Plugin();
		~Plugin();

		bool initialize(double rate, const LV2_Feature* const* features) noexcept;
		void set_max_buffer_size(int size) noexcept;
		void activate() noexcept;
		void process(uint32_t n_samples) noexcept;

		void write_current_path();

		// Liste des modeles : ecrite et lue UNIQUEMENT par le thread du worker,
		// donc sans verrou. Le thread audio ne touche que les deux atomiques.
		std::vector<std::string> modelFiles;
		std::atomic<int> modelCount{0};
		std::atomic<int> selectedIndex{-1};
		std::atomic<int> pendingIndex{-1};	// rang demande, valide seulement s'il aboutit
		std::atomic<int> loadStatus{0};
		std::atomic<int> loadCount{0};		// nombre de chargements reussis, pour voir bouger

		void scan_models();

		// Favoris : chemins detenus par le thread du WORKER uniquement
		char favPaths[NUM_FAVS][MAX_FILE_NAME] = {};
		// noms tapes dans l'interface, recus caractere par caractere
		char favNames[NUM_FAVS][MAX_FAV_NAME] = {};
		std::atomic<unsigned> favNameDirty{0};
		std::atomic<int> putVus{0};	// compteurs de diagnostic, lus dans la trace
		std::atomic<int> nameSetVus{0};
		std::atomic<unsigned> favDirty{0};	// bits des favoris a annoncer a l'interface
		// rang de chaque favori dans la liste scannee, -1 si vide ou introuvable.
		// Publie par des ports de SORTIE : c'est ainsi que la pedale retrouve le
		// nom a afficher, sans avoir a le deviner dans le DOM.
		std::atomic<int> favIndexes[NUM_FAVS];
		std::atomic<int> activeFav{0};		// 0 = aucun, 1 a 8
		std::atomic<int> browseIndex{0};
		// gain applique en ce moment : celui du favori charge, ou celui du
		// bouton si l'utilisateur y a touche depuis
		std::atomic<int> appliedGainMilli{0};	// en millidecibels, pour rester atomique
		std::atomic<int> gainToStore{0};
		std::atomic<int> storeCount{0};

		static void hmi_addressed(LV2_Handle handle, uint32_t index,
			LV2_HMI_Addressing addressing, const LV2_HMI_AddressingInfo* info);
		static void hmi_unaddressed(LV2_Handle handle, uint32_t index);

		static uint32_t options_get(LV2_Handle instance, LV2_Options_Option* options);
		static uint32_t options_set(LV2_Handle instance, const LV2_Options_Option* options);

		static LV2_Worker_Status work(LV2_Handle instance, LV2_Worker_Respond_Function respond, LV2_Worker_Respond_Handle handle,
			uint32_t size, const void* data);
		static LV2_Worker_Status work_response(LV2_Handle instance, uint32_t size, const void* data);

		static LV2_State_Status save(LV2_Handle instance, LV2_State_Store_Function store, LV2_State_Handle handle, uint32_t flags, 
			const LV2_Feature* const* features);
		static LV2_State_Status restore(LV2_Handle instance, LV2_State_Retrieve_Function retrieve, LV2_State_Handle handle, uint32_t flags,
			const LV2_Feature* const* features);

	private:
		struct URIs {
			LV2_URID atom_Object;
			LV2_URID atom_Float;
			LV2_URID atom_Int;
			LV2_URID atom_Path;
			LV2_URID atom_URID;
			LV2_URID bufSize_maxBlockLength;
			LV2_URID patch_Set;
			LV2_URID patch_Get;
			LV2_URID patch_Put;
			LV2_URID patch_body;
			LV2_URID patch_property;
			LV2_URID patch_value;
			LV2_URID units_frame;
			LV2_URID model_Path;
			LV2_URID atom_String;
			LV2_URID favs_String;
			LV2_URID fav_Path[NUM_FAVS];
			LV2_URID fav_Name[NUM_FAVS];
		};

		URIs uris = {};

		LV2_Atom_Forge atom_forge = {};
		LV2_Atom_Forge_Frame sequence_frame;

		bool scanRequested = false;
		bool indexPortSeen = false;
		float prevIndexPort = 0;
		float prevStepNext = 0;
		float prevStepPrev = 0;
		float prevRescan = 0;
		float prevBrowse = 0;
		float prevAction = 0;
		float prevStore = 0;
		float favGainPortAtLoad = 0;
		bool favGainSeen = false;
		bool storeSeen = false;
		bool rescanSeen = false;
		float prevFav[NUM_FAVS] = {};
		uint32_t favHeldSamples[NUM_FAVS] = {};
		bool favLongDone[NUM_FAVS] = {};
		// ce qui jouait juste avant l'appui : c'est CELA qu'un maintien range,
		// pas le favori que l'appui vient de charger
		int32_t favPressIndex[NUM_FAVS] = {};
		int32_t favPressGain[NUM_FAVS] = {};
		bool favSeen[NUM_FAVS] = {};
		float prevFavBrowse = 0;
		bool favBrowseSeen = false;
		uint32_t favBrowseHeld = 0;	// duree de l'etat haut, en echantillons
		float prevFavName[NUM_FAVS] = {};
		float prevFavStore[NUM_FAVS] = {};
		float prevAutoGain = 0;
		float prevStrobe = 0;
		bool strobeSeen = false;
		float prevWebChar = 0;
		bool webCharSeen = false;
		bool ecranForce = false;
		uint32_t rotationCompteur = 0;	// cadence du canal de retour
		int rotationSlot = 0;	// redessiner sans declencher le plein ecran
		uint64_t vieEchantillons = 0;	// age du plugin, pour ignorer la restauration
		int cvBande = -1;	// bande CV en cours, pour ne charger qu'au changement
		bool autoGainSeen = false;

		// Correction mesuree pour chaque favori, en millidecibels. Elle s'AJOUTE
		// au gain manuel : le plugin ne peut pas ecrire dans ses propres ports
		// d'entree, donc les boutons de gain restent la propriete de l'hote.
		std::atomic<int> autoGainMilli[NUM_FAVS];
		std::atomic<int> autoState{0};

		void mesurer_niveaux();

		// Ecrire dans ses PROPRES ports d'entree : impossible directement, mais
		// l'extension kx permet de le DEMANDER a l'hote. Recette reprise de MFX.
		const LV2_ControlInputPort_Change_Request* portreq = nullptr;
		float gainAvant[NUM_FAVS] = {};		// pour le bouton Annuler
		bool gainSauve = false;
		std::atomic<bool> appliquerDemande{false};
		std::atomic<bool> annulerDemande{false};
		int kxEtat = 0;
		int webVus = 0;		// temoin : ce que l'hote a repondu a la derniere demande
		float prevAutoApply = 0;
		float prevAutoUndo = 0;
		bool autoApplySeen = false;
		bool autoUndoSeen = false;

		void ecrire_gains(bool annuler);
		bool favStoreSeen[NUM_FAVS] = {};

		void appliquer_parametre(LV2_URID cible, const LV2_Atom* valeur);
		void write_fav_path(int slot);
		void write_fav_name(int slot);

		// --- ecran de la machine (HMI) --------------------------------
		// Le host prete une « addressing » par port assigne a un actuateur.
		// On la garde et on ecrit dedans : libellé du footswitch, et popup.
		const LV2_HMI_WidgetControl* hmi = nullptr;
		size_t hmiSize = 0;
		LV2_HMI_Addressing hmiAddr[NUM_PORTS_TOTAL] = {};



		void nom_favori(int fav, char* sortie, size_t taille) const;
		void ecrire_ecran(int fav, bool changement);
		std::atomic<int> ecranFav{-1};	// favori dont l'affichage reste a faire
		uint32_t ecranCompteur = 0;	// echantillons depuis le dernier rafraichissement
		uint32_t ecranPeriode = 0;	// 250 ms en echantillons
		int hmiCaps[NUM_PORTS_TOTAL] = {};
		// Le host DIT si l'assignation est momentanee : on ne le devine pas.
		// Momentane = agir au seul front montant (sinon le relachement
		// declenche une SECONDE fois : deux chargements par appui, et un cycle
		// qui avance de deux crans).
		bool hmiMomentary[NUM_PORTS_TOTAL] = {};
		char hmiLbl[NUM_PORTS_TOTAL][16] = {};	// dernier libelle envoye
		char hmiVal[NUM_PORTS_TOTAL][16] = {};	// derniere valeur envoyee
		uint64_t hmiPos = 0;		// position en echantillons
		uint64_t popupAt = 0;		// debut du dernier plein ecran
		uint64_t popupHold = 0;		// 3 s en echantillons
		uint32_t traceCompteur = 0;
		uint32_t tracePeriode = 0;

		bool popup_en_cours() const
		{
			return popupAt != 0 && (hmiPos - popupAt) < popupHold;
		}

		void hmi_label(int port, const char* txt);
		void hmi_value(int port, const char* txt);
		void hmi_popup(int port, const char* titre, const char* message);
		uint32_t longPressSamples = 0;		// seuil converti en echantillons
		uint32_t longPressMaxSamples = 0;	// plafond, au-dela c'est la souris

		float inputLevel = 0;
		float outputLevel = 0;
		int32_t maxBufferSize = 512;
		float bypassThresholdLinear = 0;
		uint32_t silentSamples = 0;
		bool smartBypassed = true;
	};
}

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
		kWorkTypeScan,		// re-read the model folder (off the audio thread)
		kWorkTypeSelect,	// pick a model by its rank in the list
		kWorkTypeFavLoad,	// load the favorite held in one slot
		kWorkTypeFavStore,	// store the current model into one slot
		kWorkTypeFavSetPath,	// set a favorite's path (sent by the web UI)
		kWorkTypeTrace,		// write the state to a file, for diagnosis
		kWorkTypeAutoGain	// measure every favorite's level, off the RT thread
	};

	// The plugin log does not come out on this machine, and output ports are
	// not readable from the settings panel: the state is therefore written to
	// a file, the only channel that depends on neither browser nor log.
	static constexpr const char* TRACE_PATH = "/tmp/nam_test_state.txt";

	// Buffer size for a DISPLAYED name: the one picked from the list, or the
	// file name folded to capitals.
	static constexpr unsigned int MAX_FAV_NAME = 64;

	static constexpr int NUM_FAVS = 10;
	static constexpr int NUM_PORTS_TOTAL = 85;

	// Isolation switch: when true the plugin writes nothing to the machine's
	// screen. It tells whether a crash on moving a block comes from there, the
	// addressing being taken and given back by another thread.
	static constexpr bool HMI_DISABLED = false;

	// Port indices hard-coded here, checked against the descriptor by
	// outils/verif_ports.py.
	static constexpr int IDX_FAV_BROWSE = 33;
	static constexpr int IDX_FAV_FIRST = 18;

	// How long a press has to last to count as a long one, in milliseconds
	static constexpr int LONG_PRESS_MS = 800;
	// Beyond this it is no longer a foot press but a button left ON in the
	// browser: nothing is stored, otherwise every click would overwrite a
	// favorite.
	static constexpr int LONG_PRESS_MAX_MS = 5000;

	// Folder scanned for footswitch navigation
	static constexpr const char* MODEL_DIR = "/data/user-files/NAM Models";
	static constexpr int MAX_MODELS = 200;

	struct LV2ScanMsg {
		LV2WorkType type;
	};

	struct LV2SelectMsg {
		LV2WorkType type;
		int32_t absolute;	// wanted rank, or -1
		int32_t delta;		// relative move when absolute is -1
	};

	struct LV2FavPathMsg {
		LV2WorkType type;
		int32_t slot;
		char path[MAX_FILE_NAME];
	};

	struct LV2FavMsg {
		LV2WorkType type;
		int32_t slot;		// 0 to NUM_FAVS-1
		int32_t index;		// rank to store, read BEFORE the load, or -1
		int32_t gainMilli;	// gain to store, in millidecibels
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
			// Ports added at the END, index 7 onwards. connect_port() indexes
			// this struct by port number, so the order below is the order of
			// the descriptor -- NEVER insert a field anywhere but at the end.
			// verif_ports.py compares the two at every delivery.
			float* model_index;		// 7
			float* step_next;		// 8
			float* step_prev;		// 9
			float* rescan;			// 10
			float* model_count;		// 11 output
			float* current_index;		// 12 output
			float* load_status;		// 13 output
			float* browse;			// 14
			float* action;			// 15
			float* browse_index;		// 16 output
			float* active_fav;		// 17 output
			float* fav[NUM_FAVS];		// 18..27
			float* store_slot;		// 28
			float* store;			// 29
			float* fav_gain;		// 30
			float* applied_gain;		// 31 output
			float* store_count;		// 32 output
			float* fav_browse;		// 33
			float* favGain[NUM_FAVS];	// 34..43
			float* favIndex[NUM_FAVS];	// 44..53 outputs
			float* cycle_count;		// 54
			float* hmi_state;		// 55 output
			float* favNamePort[NUM_FAVS];	// 56..65
			float* favStore[NUM_FAVS];	// 66..75
			float* auto_gain;		// 76
			float* auto_state;		// 77 output
			float* auto_offset;		// 78 output
			float* cv_select;		// 79 CV input, 0 to 10 V
			float* db_slot;			// 80 output: favorite auto_db_slot describes
			float* auto_db_slot;		// 81 output: that favorite's correction
			float* auto_apply;		// 82 write the corrections into the gains
			float* auto_undo;		// 83 put the previous gains back
			float* kx_state;		// 84 output: 0 absent, 1 present, 2 accepted, -1 refused
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

		// Model list: written and read ONLY by the worker thread, so no lock.
		// The audio thread touches nothing but the two atomics.
		std::vector<std::string> modelFiles;
		std::atomic<int> modelCount{0};
		std::atomic<int> selectedIndex{-1};
		std::atomic<int> pendingIndex{-1};	// wanted rank, valid only once it lands
		std::atomic<int> loadStatus{0};
		std::atomic<int> loadCount{0};		// successful loads, so movement is visible

		void scan_models();

		// Favorites: paths owned by the WORKER thread alone
		char favPaths[NUM_FAVS][MAX_FILE_NAME] = {};
		// Each favorite's name: the RANK picked in FAV_NAME_TABLE, 0 = AUTO.
		// This is the ONLY source of truth for the name. The control port
		// updates it as soon as the user touches the list, and the saved state
		// gives it back on reload -- even when the host does not put the port
		// value back itself.
		std::atomic<int> favNameChoice[NUM_FAVS];
		// a restored state is waiting to be put back into the control ports
		std::atomic<bool> namesToRestore{false};
		std::atomic<int> putsSeen{0};	// diagnostic counter, read in the trace
		std::atomic<unsigned> favDirty{0};	// favorites left to announce to the UI
		// rank of each favorite in the scanned list, -1 when empty or missing.
		// Published through OUTPUT ports: that is how the pedal finds the name
		// to display, without having to guess it from the DOM.
		std::atomic<int> favIndexes[NUM_FAVS];
		std::atomic<int> activeFav{0};		// 0 = none, 1 to NUM_FAVS
		std::atomic<int> browseIndex{0};
		// gain applied right now: the loaded favorite's, or the knob's if the
		// user has touched it since
		std::atomic<int> appliedGainMilli{0};	// millidecibels, to stay atomic
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
			LV2_URID favNames_String;
			LV2_URID fav_Path[NUM_FAVS];
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
		// what was playing just before the press: THAT is what a hold stores,
		// not the favorite the same press has just loaded
		int32_t favPressIndex[NUM_FAVS] = {};
		int32_t favPressGain[NUM_FAVS] = {};
		bool favSeen[NUM_FAVS] = {};
		float prevFavBrowse = 0;
		bool favBrowseSeen = false;
		uint32_t favBrowseHeld = 0;	// how long the state stayed high, in samples
		float prevFavName[NUM_FAVS] = {};
		// First pass: a value the host has ALREADY posted wins over the one
		// coming from the state; after that, only changes count.
		bool favNameSeen[NUM_FAVS] = {};
		float prevFavStore[NUM_FAVS] = {};
		float prevAutoGain = 0;
		bool screenForced = false;
		uint32_t rotationCounter = 0;	// pace of the feedback channel
		int rotationSlot = 0;		// favorite the feedback channel describes
		int cvBand = -1;	// current CV band, to load only when it changes
		bool autoGainSeen = false;

		// Correction measured for each favorite, in millidecibels. It ADDS to
		// the manual gain: a plugin cannot write to its own input ports, so the
		// gain knobs stay the host's property.
		std::atomic<int> autoGainMilli[NUM_FAVS];
		std::atomic<int> autoState{0};

		void measure_levels();

		// Writing to its OWN input ports: impossible directly, but the kx
		// extension lets the plugin ASK the host to do it. Recipe taken from MFX.
		const LV2_ControlInputPort_Change_Request* portreq = nullptr;
		float gainBefore[NUM_FAVS] = {};	// for the Undo button
		bool gainSaved = false;
		std::atomic<bool> applyRequested{false};
		std::atomic<bool> undoRequested{false};
		int kxState = 0;
		float prevAutoApply = 0;
		float prevAutoUndo = 0;
		bool autoApplySeen = false;
		bool autoUndoSeen = false;

		void write_gains(bool undo);
		bool favStoreSeen[NUM_FAVS] = {};

		void apply_parameter(LV2_URID target, const LV2_Atom* value);
		void write_fav_path(int slot);
		// put paths and picked ranks into the plugin state
		void store_favorites(LV2_State_Store_Function store, LV2_State_Handle handle);
		// put the ranks coming from the state back into the control ports
		void restore_name_ports();

		// --- the machine's screen (HMI) -------------------------------
		// The host lends one "addressing" per port assigned to an actuator.
		// We keep it and write through it: footswitch label, and popup.
		const LV2_HMI_WidgetControl* hmi = nullptr;
		size_t hmiSize = 0;
		LV2_HMI_Addressing hmiAddr[NUM_PORTS_TOTAL] = {};

		void fav_display_name(int fav, char* out, size_t size) const;
		void write_screen(int fav, bool changed);
		std::atomic<int> screenFav{-1};	// favorite whose display is still owed
		uint32_t screenCounter = 0;	// samples since the last refresh
		uint32_t screenPeriod = 0;	// 250 ms in samples
		int hmiCaps[NUM_PORTS_TOTAL] = {};
		// The host SAYS whether the addressing is momentary: we do not guess.
		// Momentary = act on the rising edge only (otherwise the release fires
		// a SECOND time: two loads per press, and a cycle that steps twice).
		bool hmiMomentary[NUM_PORTS_TOTAL] = {};
		char hmiLbl[NUM_PORTS_TOTAL][16] = {};	// last label sent
		char hmiVal[NUM_PORTS_TOTAL][16] = {};	// last value sent
		uint64_t hmiPos = 0;		// position in samples
		uint64_t popupAt = 0;		// start of the last full screen
		uint64_t popupHold = 0;		// 3 s in samples
		uint32_t traceCounter = 0;
		uint32_t tracePeriod = 0;

		bool popup_showing() const
		{
			return popupAt != 0 && (hmiPos - popupAt) < popupHold;
		}

		void hmi_label(int port, const char* txt);
		void hmi_value(int port, const char* txt);
		void hmi_popup(int port, const char* title, const char* message);
		uint32_t longPressSamples = 0;		// threshold turned into samples
		uint32_t longPressMaxSamples = 0;	// ceiling, above it is the mouse

		float inputLevel = 0;
		float outputLevel = 0;
		int32_t maxBufferSize = 512;
		float bypassThresholdLinear = 0;
		uint32_t silentSamples = 0;
		bool smartBypassed = true;
	};
}

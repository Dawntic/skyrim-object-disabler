
#include <spdlog/sinks/basic_file_sink.h>
#include <filesystem>
#include <fstream>
#include <optional>
#include <SimpleIni.h>

const std::string defaultListName = "Master";
std::filesystem::path basePath;
const std::string fillerCommand = "SSG"; // stripped game func - repurpose as inline nop

bool allObjectsEnabled = false;

// Tracked refs grouped by the cell they live in. The value is the DESIRED enabled state (false = should be
// disabled, the normal state for a tracked object; true = re-enabled via a toggle). Keeping the desired
// state in memory lets UpdatePerCell re-apply the correct state when a cell finishes loading, even for refs
// whose cell isn't currently loaded - the engine only resolves a ref pointer (LookupByID) while its cell is
// loaded. Persistent refs are always loaded and have no owning cell to wait on, so they live under the
// sentinel cellID 0 and are only ever touched by the global sweeps (SetAllObjectState / ApplyAllLoaded).
std::unordered_map<RE::FormID, std::unordered_map<RE::FormID, bool>> cellRefMap; // cellID (0 = persistent) -> { refID -> enabled }

// One-shot enables for refs whose cell is unloaded (e.g. cleared while the player is away). Drained the
// next time the owning cell finishes loading. Lives outside cellRefMap so it survives BuildDatabase, which
// rebuilds cellRefMap from the (now emptied) json files after a clear.
std::unordered_map<RE::FormID, std::vector<RE::FormID>> pendingEnable; // cellID -> refIDs to enable on load

struct CachedChange {
	RE::FormID refID; 
	RE::FormID cellID;
	std::string list; 
};
std::vector<CachedChange> cachedChanges;

struct ObjectID 
{
	RE::FormID formID; // base form ID
	RE::FormID cellID;
	std::string refMaster; // esp/esm ref comes from
	std::string cellMaster;
};


//// File ops ////////////////////////////////////////////////////////////////////////////

bool ObjectFromJson(const json& data, ObjectID& object)
{
	if (!data.is_array() || data.size() != 4){
		logger::error("[LoadObjectFromJson] Malformed object data");
		return false;
	}

	auto hexRefID = data[0].get<std::string>();
	std::from_chars(hexRefID.data(), hexRefID.data() + hexRefID.size(), object.formID, 16);

	auto hexCellID = data[1].get<std::string>();
	std::from_chars(hexCellID.data(), hexCellID.data() + hexCellID.size(), object.cellID, 16);

	object.refMaster = data[2].get<std::string>();
	object.cellMaster = data[3].get<std::string>();

	return true;
}

bool ReadFile(std::filesystem::path path, nlohmann::json& jsonData) 
{
    if (!std::filesystem::exists(path)) {
        logger::error("File not found: {}", path.string());
        return false;
    }

    std::ifstream inputFile(path);
    if (!inputFile.is_open()) {
        logger::error("Failed to open json: {}", path.string());
        return false;
    }

    try {
        inputFile >> jsonData;
    } catch (...) {
        logger::error("Failed to read json file: {}", path.string());
        return false;
    }

    if (!jsonData.is_array()) {
        logger::error("Json file: {} has invalid format", path.string());
        return false;
    } 

    return true;
}

bool WriteFile(std::filesystem::path& file, json data) {
    std::ofstream outputFile(file);
    if (!outputFile.is_open()) {
        logger::error("Failed to open file: {}", file.string());
        return false;
    }

    outputFile << data.dump(4);

	return true;
}

void BackupAllFiles() {
    namespace fs = std::filesystem;

    auto filePath = basePath / ("DisabledObjects_Backup");

	std::error_code errorCode;
	if(!fs::is_directory(filePath)){
		fs::create_directories(filePath, errorCode);
		if (errorCode) {
			logger::error("Backup: couldn't create {}: {}", filePath.string(), errorCode.message());
			return;
		}
	}
	
    for (const auto& entry : fs::directory_iterator(basePath, errorCode)) {
        if (entry.is_regular_file() && entry.path().extension() == ".json") {
            auto name = entry.path().filename();
            fs::copy_file(entry.path(), filePath / name, fs::copy_options::overwrite_existing, errorCode);
            if (errorCode) {
                logger::error("Backup: failed to copy {}: {}", name.string(), errorCode.message());
                errorCode.clear();
            }
        }
    }
}
/////////////////////////////////////////////////////////////////////////////////////////



//// Object ops /////////////////////////////////////////////////////////////////////////

void DisableObj(RE::TESObjectREFR* ref) { ref->Disable(); }
void EnableObj(RE::TESObjectREFR* ref) { ref->Enable(false); }

void SaveObjectState(const ObjectID& object, std::string saveFileName) {
	auto file = basePath / (saveFileName + ".json");

	json data;
	if (!ReadFile(file, data)) {
		if (!std::filesystem::exists(file)) {
			logger::info("Creating file: {}", file.string());
			data = json::array();
		} else { 
			logger::error("[SaveObjectState] Failed to read current list"); 
			return;
		}
	}

	auto encoded = json::array({ std::format("{:X}", object.formID), std::format("{:X}", object.cellID), object.refMaster, object.cellMaster });
	data.push_back(encoded);

	if (!WriteFile(file, data))
		logger::error("[SaveObjectState] Failed to save object to file: {}", file.string());
}

// drop the last element for a given file
void PopObjectFromFile(std::string saveFileName) {
    auto file = basePath / (saveFileName + ".json");

    json data;
    if (ReadFile(file, data)) {
        data.erase(data.size() - 1);
        if (!WriteFile(file, data)) 
			logger::error("Failed to pop object from file: {}", file.string());
    }
}

// Iterate a specific file and resolve each object to its runtime ids. The callback receives the resolved
// ref formID, the owning cell formID (0 for persistent refs / cells that no longer exist) and the live ref
// pointer, which is null when the object's cell isn't currently loaded.
void ForEachResolvedObject(std::string name, std::function<void(RE::FormID refID, RE::FormID cellID, RE::TESObjectREFR*)> callback) {
	auto file = basePath / (name + ".json");

	json jsonData;
	if (!ReadFile(file, jsonData))
		return;

	auto* dataHandler = RE::TESDataHandler::GetSingleton();
	if (!dataHandler) {
		logger::error("Invalid data handler");
		return;
	}

	for (const auto& fileObj : jsonData) {
		ObjectID object;
		if (!ObjectFromJson(fileObj, object))
			continue;

		auto refID = dataHandler->LookupFormID(object.formID, object.refMaster);
		if (!refID) {
			logger::debug("[ForEachResolvedObject] Invalid refID: {:x} | {}", object.formID, object.refMaster);
			continue;
		}

		RE::FormID cellID = 0; // persistent ref, or cell master missing -> treat as persistent
		if (!object.cellMaster.empty())
			cellID = dataHandler->LookupFormID(object.cellID, object.cellMaster);

		callback(refID, cellID, RE::TESForm::LookupByID<RE::TESObjectREFR>(refID));
	}
}

// Desired enabled state of a file's first object, read from the in-memory index so it works even when the
// object's cell isn't loaded. nullopt means the file is empty or its first object can't be resolved.
std::optional<bool> GetFileFirstState(std::string name) {
	auto file = basePath / (name + ".json");

	json jsonData;
	if (!ReadFile(file, jsonData) || jsonData.empty())
		return std::nullopt;

	auto* dataHandler = RE::TESDataHandler::GetSingleton();
	if (!dataHandler) {
		logger::error("Invalid data handler");
		return std::nullopt;
	}

	ObjectID object;
	if (!ObjectFromJson(jsonData[0], object))
		return std::nullopt;

	auto refID = dataHandler->LookupFormID(object.formID, object.refMaster);
	if (!refID)
		return std::nullopt;

	RE::FormID cellID = 0; // persistent ref -> sentinel 0
	if (!object.cellMaster.empty())
		cellID = dataHandler->LookupFormID(object.cellID, object.cellMaster);

	if (auto it = cellRefMap.find(cellID); it != cellRefMap.end())
		if (auto rit = it->second.find(refID); rit != it->second.end())
			return rit->second;

	return false; // tracked objects default to disabled
}

void ClearFile(std::string saveFileName) {
	auto file = basePath / (saveFileName + ".json");

	if (!std::filesystem::exists(file)) {
		logger::error("ClearObjectFile: file not found: {}", file.string());
		return;
	}

	// Re-enable every object in the file. Loaded refs are enabled immediately; refs in unloaded cells are
	// queued so they get enabled the next time their cell loads (BuildDatabase below would otherwise drop
	// them, leaving them disabled forever).
	ForEachResolvedObject(saveFileName, [](RE::FormID refID, RE::FormID cellID, RE::TESObjectREFR* ref) {
		if (ref)
			EnableObj(ref);
		else if (cellID)
			pendingEnable[cellID].push_back(refID);
	});

	if (!WriteFile(file, json::array()))
		logger::error("Failed to clear file: {}", file.string());
}

void ClearAllFiles() {
	std::error_code ec;
	for (const auto& file : std::filesystem::directory_iterator(basePath, ec)) {
		if (file.is_regular_file() && file.path().extension() == ".json") {
			ClearFile(file.path().stem().string());
		}
	}
}

// Combine json files into one place
void BuildDatabase() {
	cellRefMap.clear();
	auto* dataHandler = RE::TESDataHandler::GetSingleton();
	if (!dataHandler) { 
		logger::error("BuildDatabase: no data handler"); 
		return; 
	}

	std::error_code error;
	for (const auto& entry : std::filesystem::directory_iterator(basePath, error)) {
		if (!entry.is_regular_file() || entry.path().extension() != ".json") 
			continue;

		json data;
		if (!ReadFile(entry.path(), data)) 
			continue;

		for (const auto& item : data) {
			ObjectID object;
			if(!ObjectFromJson(item, object))
				continue;

			auto formID = dataHandler->LookupFormID(object.formID, object.refMaster);
			if (!formID){
				logger::error("[BuildDatabase] Invalid formID: {} | {}", object.formID, object.refMaster);
				continue;
			}

			// Persistent refs (empty cell master) use the sentinel cellID 0. A non-persistent ref whose
			// cell can't be resolved falls back to the same sentinel so it's still caught by the load sweep.
			RE::FormID cellID = 0;
			if (!object.cellMaster.empty()) {
				cellID = dataHandler->LookupFormID(object.cellID, object.cellMaster);
				if (!cellID)
					logger::warn("[BuildDatabase] Form {} has invalid cellID: {} | {} - treating as persistent ref", object.formID, object.cellID, object.cellMaster);
			}

			cellRefMap[cellID][formID] = false; // desired state defaults to disabled
		}
	}

	logger::info("[BuildDatabase] Processed {} cells", cellRefMap.size());
}

// Re-apply the stored desired state to every currently-loaded tracked ref, and flush any pending enables
// that have since become loaded. Does not change desired state, so it preserves active toggles across a
// fast travel / coc transfer.
void ApplyAllLoaded() {
	for (auto it = pendingEnable.begin(); it != pendingEnable.end();) {
		std::erase_if(it->second, [](RE::FormID refID) {
			if (auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(refID)) {
				EnableObj(ref);
				return true;
			}
			return false;
		});
		it = it->second.empty() ? pendingEnable.erase(it) : std::next(it);
	}

	for (auto& [cell, refs] : cellRefMap)
		for (auto& [refID, enabled] : refs)
			if (auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(refID))
				enabled ? EnableObj(ref) : DisableObj(ref);
}

// Set the desired state for every tracked ref, then apply it to the ones currently loaded. Unloaded refs
// pick up the new state from cellRefMap when their cell loads (see UpdatePerCell).
void SetAllObjectState(bool enable) {
	for (auto& [cell, refs] : cellRefMap)
		for (auto& [refID, enabled] : refs)
			enabled = enable;

	ApplyAllLoaded();
}

void UpdatePerCell(const RE::TESCellFullyLoadedEvent* event)
{
	auto cellID = event->cell->GetFormID();

	// Drain one-shot enables queued while this cell was unloaded (e.g. from a clear).
	if (auto it = pendingEnable.find(cellID); it != pendingEnable.end()) {
		for (auto refID : it->second)
			if (auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(refID))
				EnableObj(ref);
		pendingEnable.erase(it);
	}

	// Apply the desired state for the refs tracked in this cell.
	if (auto it = cellRefMap.find(cellID); it != cellRefMap.end()) {
		for (auto& [refID, enabled] : it->second){
			if (auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(refID)){
				enabled ? EnableObj(ref) : DisableObj(ref);
				logger::debug("[UpdatePerCell] {} ref: {} | {}", enabled ? "Enabled" : "Disabled", ref->GetLocalFormID(), ref->GetFile(0) ? std::string(ref->GetFile(0)->GetFilename()) : "Null");
			} else{
				logger::debug("[UpdatePerCell] Failed to set ref state for ref: {} | {}", refID, event->cell->GetLocalFormID());
			}
		}
	}
}
/////////////////////////////////////////////////////////////////////////////////////////



//// Core ///////////////////////////////////////////////////////////////////////////////

void RunCommands(std::vector<std::string>& args, std::string& command) {
    const auto& cmd = args[0];

	if (cmd == "p") {
		command = "disable";
		auto selectedObj = RE::Console::GetSelectedRef();
		if (!selectedObj) { 
			logger::warn("No object selected"); 
			return; 
		}

		auto fileName = defaultListName;
		if (args.size() >= 3 && args[1] == "n") 
			fileName = args[2];

		auto* basePlugin = selectedObj->GetFile(0);
		if (!basePlugin) { 
			logger::warn("Dynamic ref, skipping"); 
			command = fillerCommand; 
			return; 
		}

		auto* cell = selectedObj->GetParentCell();
		auto* cellPlugin = cell ? cell->GetFile(0) : nullptr;
		
		ObjectID object = { .formID = selectedObj->GetLocalFormID(), .refMaster = std::string(basePlugin->GetFilename()) };
		RE::FormID trackCellID = 0; // persistent refs are tracked under the sentinel cellID 0
		if (cellPlugin) { // non persistent ref
			object.cellID = cell->GetLocalFormID();
			object.cellMaster = std::string(cellPlugin->GetFilename());
			trackCellID = cell->GetFormID();
		} else { // persistent ref
			object.cellID = 0;
			object.cellMaster = "";
		}
		cellRefMap[trackCellID][selectedObj->formID] = false;
		SaveObjectState(object, fileName);

		if (cachedChanges.size() == 10)
			cachedChanges.erase(cachedChanges.begin());
		cachedChanges.push_back({ selectedObj->formID, trackCellID, fileName });
	}
    else if (cmd == "toggle") {
        command = fillerCommand;
        if (args.size() < 2) { // Toggle all
            allObjectsEnabled = !allObjectsEnabled;
			SetAllObjectState(allObjectsEnabled);
        } else { // Toggle specific file
            auto state = GetFileFirstState(args[1]);
            if (!state) {
                logger::error("Toggle: empty or unresolvable file: {}", args[1]);
                return;
            }

            bool enable = !*state; // flip the file's current desired state
            ForEachResolvedObject(args[1], [enable](RE::FormID refID, RE::FormID cellID, RE::TESObjectREFR* ref) {
                // Update the stored desired state so unloaded refs are corrected when their cell loads
                // (cellID 0 = persistent)...
                cellRefMap[cellID][refID] = enable;
                // ...and apply immediately to anything already loaded.
                if (ref)
                    enable ? EnableObj(ref) : DisableObj(ref);
            });
        }
    } 
	else if (cmd == "undo") {
		command = fillerCommand;
		if (cachedChanges.empty()) { 
			logger::error("Nothing to undo"); 
			return; 
		}

		auto item = cachedChanges.back();
		if (auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(item.refID))
			if (ref->IsDisabled()) 
				ref->Enable(false);

		if (auto it = cellRefMap.find(item.cellID); it != cellRefMap.end()) {
			it->second.erase(item.refID);
			if (it->second.empty())
				cellRefMap.erase(it);
		}

		if (!item.list.empty()) 
			PopObjectFromFile(item.list);
		cachedChanges.pop_back();
	}
	else if (cmd == "clear") {
		command = fillerCommand;
		if (args.size() < 2) { 
			BackupAllFiles(); 
			ClearAllFiles(); 
		}
		else
			ClearFile(args[1]);
		BuildDatabase();   // index now reflects the cleared files
	}
    else {
       // Dont set a command so we fall through to default game warning
        logger::error("Unknown command: {}", cmd);
    }
}

struct Hooks 
{
    struct CompileAndRun {
        static void thunk(RE::Script* script, uint64_t unk, uint32_t unk2, uint64_t unk3) {
            std::string command = script->text;

            if (command.starts_with("disable")) {
                // Parse commands
                std::vector<std::string> args;
                std::string commandString(command);
                std::istringstream iss(commandString);
                int i = 0;
                for (std::string tok; iss >> tok; i++) {
                    if (i == 0)  // ignore 'disable' keyword
                        continue;

                    if (tok.starts_with('-')) {
                        auto arg = tok.substr(1);
                        args.emplace_back(arg);  // flags
                    } else {
                        args.emplace_back(tok);  // not flags
                    }
                }
				if (args.empty()) {
					if (auto sel = RE::Console::GetSelectedRef()) {
						auto* cell = sel->GetParentCell();
						if (cachedChanges.size() == 10) 
							cachedChanges.erase(cachedChanges.begin());
						cachedChanges.push_back({ sel->formID, cell ? cell->GetFormID() : 0, "" });
					}
				} else {
                     RunCommands(args, command);
                 }

                 std::strcpy(script->text, command.c_str());
            }

            func(script, unk, unk2, unk3);

        };

        static inline REL::Relocation<decltype(thunk)> func;
    };

    static void Install()
    {  
        logger::info("Hooking CompileAndRun");
        stl::detour_thunk<CompileAndRun>(REL::RelocationID(21416, AE_CHECK(SKSE::RUNTIME_SSE_1_6_1130, 21890, 441582)));
    }
};
/////////////////////////////////////////////////////////////////////////////////////////



//// Event Sinks ////////////////////////////////////////////////////////////////////////

// PositionPlayerEventHandler fires for coc loads, kPostLoadGame does not fire
// for Normal loads kPostLoadGame fires but PositionPlayerEventHandler doesn't

std::atomic_bool inTransfer = true; //when fast traveling or coc

// When game is loading from coc or fast travel it will have INF loading screen if we set obj state in CellFullyLoadedEventHandler so do it here
class PositionPlayerEventHandler : public RE::BSTEventSink<RE::PositionPlayerEvent>
{
public:
	virtual RE::BSEventNotifyControl ProcessEvent(const RE::PositionPlayerEvent* a_event, RE::BSTEventSource<RE::PositionPlayerEvent>*) {
		//logger::info("Player Pos Event msg: {}", a_event->type.underlying());

		if (a_event && a_event->type == RE::PositionPlayerEvent::EVENT_TYPE::kPre) {
			inTransfer = true;
		}
		
		if (a_event && a_event->type == RE::PositionPlayerEvent::EVENT_TYPE::kFinish) { // first post cell loading
			logger::info("Game version: {}", REL::Module::get().version().string());
			logger::info("Game Loaded - via kFinish"); //fires for coc, fast travel
			inTransfer = false;
			ApplyAllLoaded(); // re-assert current desired states for the newly loaded cells (keeps toggles)
		}

		return RE::BSEventNotifyControl::kContinue;
	}

	static bool Register() {
		static PositionPlayerEventHandler sink;

		auto player = RE::PlayerCharacter::GetSingleton();
		player->AsPositionPlayerEventSource()->AddEventSink(&sink);

		logger::info("Registered {}", typeid(sink).name());

		return true;
	}
};

// Run object check here 
// Register for player position event here since before this the player is null and after is too late.
class CellFullyLoadedEventHandler : public RE::BSTEventSink<RE::TESCellFullyLoadedEvent>
{
public:
	virtual RE::BSEventNotifyControl ProcessEvent(const RE::TESCellFullyLoadedEvent* event, RE::BSTEventSource<RE::TESCellFullyLoadedEvent>*) {
		if (!event || !event->cell) 
			return RE::BSEventNotifyControl::kContinue;

		static bool first = true;
		if (first && RE::PlayerCharacter::GetSingleton()) {
			first = false;
			PositionPlayerEventHandler::Register(); 
		}

		if (inTransfer)
			return RE::BSEventNotifyControl::kContinue;

		UpdatePerCell(event);

		return RE::BSEventNotifyControl::kContinue;
	}

	static bool Register() {
		static CellFullyLoadedEventHandler singleton;

		RE::ScriptEventSourceHolder::GetSingleton()->AddEventSink<RE::TESCellFullyLoadedEvent>(&singleton);

		logger::info("Registered {}", typeid(singleton).name());

		return true;
	}
};
/////////////////////////////////////////////////////////////////////////////////////////



//// Setup //////////////////////////////////////////////////////////////////////////////

bool LoadDebugSetting() {
	auto path = std::filesystem::current_path() / "Data" / "SKSE" / "Plugins" / std::format("{}.ini", SKSE::PluginDeclaration::GetSingleton()->GetName());

	CSimpleIniA ini;
	ini.SetUnicode();
	ini.LoadFile(path.string().c_str());                 // missing file -> ini just stays empty

	bool debug = ini.GetBoolValue("Debug", "EnableDebugLog", false);

	// write the key back: creates the file with the default if absent, preserves user edits otherwise
	ini.SetBoolValue("Debug", "EnableDebugLog", debug);
	ini.SaveFile(path.string().c_str());

	return debug;
}

void SetupLogger() {
	auto path = *logger::log_directory();
	path /= std::format("{}.txt", SKSE::PluginDeclaration::GetSingleton()->GetName());

	auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path.string(), true);
	auto loggerPtr = std::make_shared<spdlog::logger>("log", std::move(sink));

	auto level = LoadDebugSetting() ? spdlog::level::debug : spdlog::level::info;
	loggerPtr->set_level(level);
	loggerPtr->flush_on(level);

	spdlog::set_default_logger(std::move(loggerPtr));
}


void MessageHandler(SKSE::MessagingInterface::Message* message)
{
	logger::info("Message: {}", message->data);

	if(message->type == SKSE::MessagingInterface::kDataLoaded){ // CAN BE DONE AT kPostPostLoad ??
		BuildDatabase();
	}
    else if (message->type == SKSE::MessagingInterface::kPostPostLoad) {
        logger::info("Game version: {}", REL::Module::get().version().string()); 

		// Setup base file path
		basePath = std::filesystem::current_path() / "Data" / "SKSE" / "Plugins" / "DisabledObjects";
		if (!std::filesystem::is_directory(basePath)) {
			std::filesystem::create_directories(basePath);
			logger::info("Creating base folder...");
		}

		logger::info("Base folder location: {}", basePath.lexically_relative(std::filesystem::current_path()).string());

		// Register here since player ptr will be valid
		CellFullyLoadedEventHandler::Register();	

        Hooks::Install();
    }
	else if (message->type == SKSE::MessagingInterface::kPostLoadGame) {
		logger::info("Game version: {}", REL::Module::get().version().string());
		logger::info("Game Loaded - via kPostLoadGame");
		inTransfer = false;
		allObjectsEnabled = false;
		pendingEnable.clear();
		SetAllObjectState(false);
	}
	else if(message->type == SKSE::MessagingInterface::kPreLoadGame || message->type == SKSE::MessagingInterface::kNewGame){
		inTransfer = true;
		allObjectsEnabled = false;
		pendingEnable.clear();
	}
}

SKSEPluginInfo(.Version = REL::Version{1,0,0}, .Name = "Object Disabler", .Author = "Dawntic", .StructCompatibility = SKSE::StructCompatibility::Independent, .RuntimeCompatibility = SKSE::VersionIndependence::AddressLibrary)

SKSEPluginLoad(const SKSE::LoadInterface* skse) 
{
    SetupLogger();

    SKSE::Init(skse);
   
    SKSE::GetMessagingInterface()->RegisterListener(MessageHandler);
    SKSE::AllocTrampoline(1 << 5);

    return true;
}
/////////////////////////////////////////////////////////////////////////////////////////
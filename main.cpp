
#include <spdlog/sinks/basic_file_sink.h>
#include <filesystem>
#include <fstream>

// Add a debug mode so logger::debug is written otherwise dont write it 
// CALLING CLEAR SHOULD reenable everything too

// disabled items in save file stayed disabled without needing to disable on player load..

std::string defaultListName = "Master";
// Cell formID, refs IDs in that cell
std::unordered_map<RE::FormID, std::vector<RE::FormID>> cellRefMap;
std::vector<RE::FormID> persistentRefs;   // always-loaded refs, disabled at load (no cell key possible)

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



std::filesystem::path basePath;

bool allObjectsEnabled = false;
std::string fillerCommand = "SSG"; // stripped game func - repurpose as inline nop

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
	std::from_chars(hexCellID.data(), hexCellID.data() + hexCellID.size(), object.formID, 16);

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

void ClearFile(std::string saveFileName) {
    auto file = basePath / (saveFileName + ".json");

    if (!std::filesystem::exists(file)) {
        logger::error("ClearObjectFile: file not found: {}", file.string());
        return;
    }

    if(!WriteFile(file, json::array()))
        logger::info("Failed to clear file: {}", file.string());
}

void ClearAllFiles() {
    std::error_code ec;
    for (const auto& file : std::filesystem::directory_iterator(basePath, ec)) {
        if (file.is_regular_file() && file.path().extension() == ".json") {
            ClearFile(file.path().stem().string());
        }
    }
}

void BackupAllFiles() {
    namespace fs = std::filesystem;

    auto filePath = basePath.parent_path() / ("DisabledObjects_Backup");

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
		if (!std::filesystem::exists(file)) 
			data = json::array();
		else { 
			logger::error("[SaveObjectState] Failed to read current list"); 
			return;
		}
	}

	// [refLocalHex, refMod, cellLocalHex, cellMod]
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

// Iterate a specific file and run callback for each valid object
void ForEachObjectInFile(std::string name, std::function<void(RE::TESObjectREFR*)> callback) {
   auto file = basePath / (name + ".json");
	
    json jsonData;
    if (ReadFile(file, jsonData)) {
		//logger::info("Read file");
        for (const auto& fileObj : jsonData) {
            auto* dataHandler = RE::TESDataHandler::GetSingleton();
            if (!dataHandler) {
                logger::error("Invalid data handler");
                return;
            }

			ObjectID object;
			if(!ObjectFromJson(fileObj, object))
				return;

            auto formID = dataHandler->LookupFormID(object.formID, object.cellMaster);

            if (auto* objRef = RE::TESForm::LookupByID<RE::TESObjectREFR>(formID)) {
				logger::debug("Found ref: {:x} | {}", object.formID, object.cellMaster);
                callback(objRef);
            } else {
				bool missingMaster = !(dataHandler->GetLoadedModIndex(object.refMaster).has_value() || dataHandler->GetLoadedLightModIndex(object.refMaster).has_value());            
				bool missingCellMaster = !(dataHandler->GetLoadedModIndex(object.cellMaster).has_value() || dataHandler->GetLoadedLightModIndex(object.cellMaster).has_value());
				auto comment = missingMaster ? "- Missing ref master file" : missingCellMaster ? "-  Missing cell master file" : "";
				logger::debug("No valid object ref found for: {:x} | {} | {} | {}  {}", object.formID, object.refMaster, object.cellID, object.cellMaster, comment);
            }
        }
    }
}

RE::TESObjectREFR* GetFirstObjectInFile(std::string name) {
    auto file = basePath / (name + ".json");

    json jsonData;
    if (ReadFile(file, jsonData)) {
		if(jsonData.empty())
			return nullptr;
      
        auto* dataHandler = RE::TESDataHandler::GetSingleton();
        if (!dataHandler) {
            logger::error("Invalid data handler");
            return nullptr;
        }
		
		ObjectID object;
		if(!ObjectFromJson(jsonData[0], object))
			return nullptr;
		
        auto formID = dataHandler->LookupFormID(object.formID, object.refMaster);

        auto* objRef = RE::TESForm::LookupByID<RE::TESObjectREFR>(formID);
        if (!objRef) {
			bool missingMaster = !(dataHandler->GetLoadedModIndex(object.refMaster).has_value() || dataHandler->GetLoadedLightModIndex(object.refMaster).has_value());
			bool missingCellMaster = !(dataHandler->GetLoadedModIndex(object.cellMaster).has_value() || dataHandler->GetLoadedLightModIndex(object.cellMaster).has_value());
			auto comment = missingMaster ? "- Missing ref master file" : missingCellMaster ? "-  Missing cell master file" : "";
			logger::debug("No valid object ref found for: {:x} | {} | {} | {}  {}", object.formID, object.refMaster, object.cellID, object.cellMaster, comment);
        }
        return objRef;
    }

    return nullptr;
}

// set state of all objects
void SetForEachFile(bool enable) {
    std::error_code ec;
    for (const auto& file : std::filesystem::directory_iterator(basePath, ec)) {
        if (file.is_regular_file() && file.path().extension() == ".json")
            ForEachObjectInFile(file.path().stem().string(), enable ? EnableObj : DisableObj);
    }
    allObjectsEnabled = enable;
}

// Combine json files into one place
void BuildDatabase() {
	cellRefMap.clear();
	persistentRefs.clear();
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
				return;

			auto formID = dataHandler->LookupFormID(object.formID, object.refMaster);
			if (!formID){
				logger::error("[BuildDatabase] Invalid formID: {} | {}", object.formID, object.refMaster);
				continue;
			}

			// persistent ref
			if (object.cellMaster.empty()) {  
				persistentRefs.push_back(formID);
				continue; 
			} 

			auto cellID = dataHandler->LookupFormID(object.cellID, object.cellMaster);
			if (cellID)
				cellRefMap[cellID].push_back(formID);
			else{
				logger::warn("[BuildDatabase] Form {} has invalid cellID: {} | {} - treating as persistent ref", object.formID, object.cellID, object.cellMaster);
				persistentRefs.push_back(formID);   // cell gone -> fall back to load sweep
			}
		}
	}

	logger::info("BuildDatabase: {} cells, {} persistent", cellRefMap.size(), persistentRefs.size());
}

// Disable every tracked ref currently in memory (used after a load/transfer completes).
void ApplyAllLoaded() {
	if (allObjectsEnabled) 
		return;

	for (auto& [cell, refs] : cellRefMap){
		for (auto refID : refs){
			if (auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(refID))
				ref->Disable();
		}
	}
	
	for (auto refID : persistentRefs){
		if (auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(refID))
			ref->Disable();
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
		if (cellPlugin) { // non persistent ref
			object.cellID = cell->GetLocalFormID();
			object.cellMaster = std::string(cellPlugin->GetFilename());
			cellRefMap[cell->GetFormID()].push_back(selectedObj->formID);
		} else { // persistent ref 
			object.cellID = 0;
			object.cellMaster = "";
			persistentRefs.push_back(selectedObj->formID);
		}
		SaveObjectState(object, fileName);

		if (cachedChanges.size() == 10) 
			cachedChanges.erase(cachedChanges.begin());
		cachedChanges.push_back({ selectedObj->formID, cell ? cell->GetFormID() : 0, fileName });
	}
    else if (cmd == "toggle") {
        command = fillerCommand;
        if (args.size() < 2) { // Toggle all
            allObjectsEnabled = !allObjectsEnabled;
            SetForEachFile(allObjectsEnabled);
        } else { // Toggle specific file
            if (auto first = GetFirstObjectInFile(args[1]))
                ForEachObjectInFile(args[1], first->IsDisabled() ? EnableObj : DisableObj);
            else
                logger::error("Invalid object 0");
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

		std::erase(persistentRefs, item.refID);
		if (auto it = cellRefMap.find(item.cellID); it != cellRefMap.end()) {
			auto& refList = it->second;
			refList.erase(std::remove(refList.begin(), refList.end(), item.refID), refList.end());
			if (refList.empty()) 
				cellRefMap.erase(it);
		}

		if (!item.list.empty()) 
			PopObjectFromFile(item.list);
		cachedChanges.pop_back();
	}
	else if (cmd == "clear") {
		command = fillerCommand;
		if (args.size() < 2) { BackupAllFiles(); ClearAllFiles(); }
		else ClearFile(args[1]);
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
		logger::info("Player Pos Event msg: {}", a_event->type.underlying());

		if (a_event && a_event->type == RE::PositionPlayerEvent::EVENT_TYPE::kPre) {
			inTransfer = true;
		}
		
		if (a_event && a_event->type == RE::PositionPlayerEvent::EVENT_TYPE::kFinish) { // firest post cell loading
			logger::info("Game Loaded"); //fires for coc, fast travel
			inTransfer = false;
			//SetForEachFile(false);
			ApplyAllLoaded();
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
	virtual RE::BSEventNotifyControl ProcessEvent(const RE::TESCellFullyLoadedEvent* a_event, RE::BSTEventSource<RE::TESCellFullyLoadedEvent>*) {
		if (!a_event || !a_event->cell) return RE::BSEventNotifyControl::kContinue;

		static bool init = true;
		if (init) {
			init = false; PositionPlayerEventHandler::Register(); 
		}

		if (inTransfer || allObjectsEnabled) 
			return RE::BSEventNotifyControl::kContinue;

		if (auto it = cellRefMap.find(a_event->cell->GetFormID()); it != cellRefMap.end()) {
			for (auto refID : it->second)
				if (auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(refID))
					ref->Disable();
		}

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

void SetupLogger() {
    auto path = *logger::log_directory();
    path /= std::format("{}.txt", SKSE::PluginDeclaration::GetSingleton()->GetName());

    auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path.string(), true);
    auto loggerPtr = std::make_shared<spdlog::logger>("log", std::move(sink));

    spdlog::set_level(spdlog::level::info);
    spdlog::flush_on(spdlog::level::info);
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

		CellFullyLoadedEventHandler::Register();	

        Hooks::Install();
    }
	else if (message->type == SKSE::MessagingInterface::kPostLoadGame) {
		logger::info("Game version: {}", REL::Module::get().version().string());
		logger::info("Game Loaded");
		SetForEachFile(false);
	} 
	else if(message->type == SKSE::MessagingInterface::kPreLoadGame || message->type == SKSE::MessagingInterface::kNewGame){
		inTransfer = true;
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
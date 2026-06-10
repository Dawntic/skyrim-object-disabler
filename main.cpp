
#include <spdlog/sinks/basic_file_sink.h>
#include <filesystem>
#include <fstream>

std::string defaultListName = "Master";
std::vector<std::pair<RE::FormID, std::string>> cachedChanges;

std::filesystem::path basePath;

bool allObjectsEnabled = false;
std::string fillerCommand = "SSG"; // stripped game func - repurpose as inline nop

//// File ops ////////////////////////////////////////////////////////////////////////////

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

void SaveObjectState(RE::FormID localID, std::string_view modName, std::string saveFileName) {
    auto file = basePath / (std::string(saveFileName) + ".json");

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

    // store [formID, modName] in array format
    data.push_back(json::array({std::format("{:X}", localID), std::string(modName)}));

    if (!WriteFile(file, data)) 
		logger::error("Failed to save object to file: {}", file.string());
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
        for (const auto& item : jsonData) {
            auto* dataHandler = RE::TESDataHandler::GetSingleton();
            if (!dataHandler) {
                logger::info("Invalid data handler");
                return;
            }
            RE::FormID localFormID;
            auto formIDHex = item[0].get<std::string>();
            std::from_chars(formIDHex.data(), formIDHex.data() + formIDHex.size(), localFormID, 16);

            auto modName = item[1].get<std::string>();
            auto formID = dataHandler->LookupFormID(localFormID, modName);

			//bool missingMasterA = !(dataHandler->GetLoadedModIndex(modName).has_value() || dataHandler->GetLoadedLightModIndex(modName).has_value());
			//logger::error("Missing Master: {}", missingMasterA ? modName : "False");

            if (auto* objRef = RE::TESForm::LookupByID<RE::TESObjectREFR>(formID)) {
				logger::info("Found ref: {:x} | {}", localFormID, modName);
                callback(objRef);
            } else {
				bool missingMaster = !(dataHandler->GetLoadedModIndex(modName).has_value() || dataHandler->GetLoadedLightModIndex(modName).has_value());            
				logger::error("No valid object ref found for: {:x} | {} {}", localFormID, modName, missingMaster ? "  - Missing master file" : "");
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
            logger::info("Invalid data handler");
            return nullptr;
        }
		
		auto& first = jsonData[0];
        RE::FormID localFormID;
        auto formIDHex = first[0].get<std::string>();
        std::from_chars(formIDHex.data(), formIDHex.data() + formIDHex.size(), localFormID, 16);

        auto modName = first[1].get<std::string>();
        auto formID = dataHandler->LookupFormID(localFormID, modName);

        auto* objRef = RE::TESForm::LookupByID<RE::TESObjectREFR>(formID);
        if (!objRef) {
            bool missingMaster = !(dataHandler->GetLoadedModIndex(modName).has_value() || dataHandler->GetLoadedLightModIndex(modName).has_value());
            logger::error("No valid object ref found for: {:x} | {} {}", localFormID, modName, missingMaster ? "  - Mod is missing a master" : "");
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

        auto* basePlugin = selectedObj->GetFile(0); // The base esm/esp/esl
        if (!basePlugin) {
            logger::warn("Dynamic ref, skipping"); // Does this happen?
            return;
        }

        SaveObjectState(selectedObj->GetLocalFormID(), basePlugin->GetFilename(), fileName);
        if (cachedChanges.size() == 10)
            cachedChanges.erase(cachedChanges.begin());
        cachedChanges.push_back({selectedObj->formID, fileName});
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
        if (auto* objRef = RE::TESForm::LookupByID<RE::TESObjectREFR>(item.first))
            if (objRef->IsDisabled())
                objRef->Enable(false);  
        if (!item.second.empty()) PopObjectFromFile(item.second);
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
                         if (cachedChanges.size() == 10) 
                             cachedChanges.erase(cachedChanges.begin());
                         cachedChanges.push_back({sel->formID, ""});
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
			SetForEachFile(false);
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
		logger::info("Cell fully loaded msg");
		
		static bool init = true;
		if (a_event && a_event->cell){ // && a_event->cell->GetRuntimeData().loadedData->refsFullyLoaded) {
			if(init) {
				init = false;
				logger::info("Register Position Player Event");
				PositionPlayerEventHandler::Register(); 
			} 
			if(!inTransfer) {
				logger::info("Setting base object state - CellFullyLoadedEventHandler");
				SetForEachFile(false);
			}
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
    if (message->type == SKSE::MessagingInterface::kPostPostLoad) {
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
	if (message->type == SKSE::MessagingInterface::kPostLoadGame) {
		logger::info("Game version: {}", REL::Module::get().version().string());
		logger::info("Game Loaded");
		SetForEachFile(false);
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
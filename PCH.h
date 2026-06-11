#pragma once

#include "RE/Skyrim.h"
#include "SKSE/SKSE.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <detours/detours.h>

#ifdef NDEBUG
#	include <spdlog/sinks/basic_file_sink.h>
#else
#	include <spdlog/sinks/msvc_sink.h>
#endif

//#include <ClibUtil/simpleINI.hpp>

#include <nlohmann/json.hpp>
using json = nlohmann::json;

namespace logger = SKSE::log;

using namespace std::literals;

#define DLLEXPORT __declspec(dllexport)

namespace stl {
    using namespace SKSE::stl;

    template <class T>
    void write_thunk_call(std::uintptr_t a_src) {
        auto& trampoline = SKSE::GetTrampoline();
        T::func = trampoline.write_call<5>(a_src, T::thunk);
    }

    template <class T>
    void detour_thunk(REL::RelocationID a_relId) {
        T::func = a_relId.address();
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(reinterpret_cast<PVOID*>(&T::func), reinterpret_cast<PVOID>(T::thunk));
        DetourTransactionCommit();
    }
}

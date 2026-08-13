#include "CommandSystem.hpp"
#include "VRMod.hpp"
#include <Windows.h>
#include <algorithm>
#include <sstream>
#include <fstream>

namespace bl1gotyvr {

CommandSystem& CommandSystem::Instance() {
    static CommandSystem sys;
    return sys;
}

void CommandSystem::RegisterCommand(const std::string& name, CommandHandler handler, const std::string& description) {
    if (name.empty() || !handler) return;

    // Convert name to lowercase for case-insensitive matching
    std::string lowerName = name;
    std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);

    m_commands[lowerName] = { handler, description };
    Log("[Command] Registered command: %s", name.c_str());
}

bool CommandSystem::Execute(const std::string& commandLine) {
    if (commandLine.empty()) return false;

    // Parse command name and arguments
    std::istringstream iss(commandLine);
    std::string name;
    iss >> name;

    // Convert to lowercase
    std::transform(name.begin(), name.end(), name.begin(), ::tolower);

    // Find command
    auto it = m_commands.find(name);
    if (it == m_commands.end()) {
        Log("[Command] Unknown command: %s", name.c_str());
        return false;
    }

    // Get remaining args
    std::string args;
    std::getline(iss, args);
    // Trim leading whitespace
    size_t start = args.find_first_not_of(" \t");
    if (start != std::string::npos) {
        args = args.substr(start);
    }

    // Execute
    try {
        it->second.handler(args);
        return true;
    } catch (const std::exception& e) {
        Log("[Command] Error executing %s: %s", name.c_str(), e.what());
        return false;
    }
}

bool CommandSystem::LoadCommandsFromFile(const std::string& filePath) {
    m_commandFilePath = filePath;
    std::ifstream file(filePath);
    if (!file.is_open()) {
        Log("[Command] Could not open command file: %s", filePath.c_str());
        return false;
    }

    Log("[Command] Loaded command file: %s", filePath.c_str());
    return true;
}

void CommandSystem::PollCommandFile() {
    if (m_commandFilePath.empty()) return;

    // Simple polling - check if file was modified
    static ULONGLONG lastCheckTime = 0;
    ULONGLONG currentTime = GetTickCount64();

    // Check every 100ms
    if (currentTime - lastCheckTime < 100) return;
    lastCheckTime = currentTime;

    WIN32_FILE_ATTRIBUTE_DATA attributes = {};
    if (!GetFileAttributesExA(m_commandFilePath.c_str(), GetFileExInfoStandard, &attributes)) {
        return;
    }

    static FILETIME lastWriteTime = {};
    if (CompareFileTime(&attributes.ftLastWriteTime, &lastWriteTime) == 0) {
        return; // No change
    }
    lastWriteTime = attributes.ftLastWriteTime;

    // Read and execute commands
    std::ifstream file(m_commandFilePath);
    if (!file.is_open()) return;

    std::string line;
    while (std::getline(file, line)) {
        // Skip empty lines and comments
        if (line.empty() || line[0] == '#') continue;

        // Execute command
        Execute(line);
    }

    Log("[Command] Processed command file update");
}

void CommandSystem::ListCommands() const {
    Log("[Command] Registered commands (%zu):", m_commands.size());
    for (const auto& [name, entry] : m_commands) {
        Log("[Command]   %s - %s", name.c_str(), entry.description.c_str());
    }
}

std::string CommandSystem::GetDescription(const std::string& name) const {
    std::string lowerName = name;
    std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);

    auto it = m_commands.find(lowerName);
    if (it != m_commands.end()) {
        return it->second.description;
    }
    return "";
}

} // namespace bl1gotyvr

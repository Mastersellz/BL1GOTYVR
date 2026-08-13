#pragma once
#include <functional>
#include <string>
#include <unordered_map>

namespace bl1gotyvr {

// Simple command system for runtime debugging and tuning
class CommandSystem {
public:
    static CommandSystem& Instance();

    // Register a command handler
    using CommandHandler = ::std::function<void(::std::string args)>;
    void RegisterCommand(const ::std::string& name, CommandHandler handler, const ::std::string& description = "");

    // Execute a command string (format: "commandname arg1 arg2 ...")
    bool Execute(const ::std::string& commandLine);

    // Load commands from a file (one command per line)
    bool LoadCommandsFromFile(const ::std::string& filePath);

    // Poll for command file changes (call periodically)
    void PollCommandFile();

    // List all registered commands
    void ListCommands() const;

    // Get command description
    ::std::string GetDescription(const ::std::string& name) const;

private:
    CommandSystem() = default;

    struct CommandEntry {
        CommandHandler handler;
        ::std::string description;
    };

    ::std::unordered_map<::std::string, CommandEntry> m_commands;
    ::std::string m_commandFilePath;
    double m_lastPollTime = 0.0;
};

} // namespace bl1gotyvr

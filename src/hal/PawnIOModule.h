#pragma once
#include <windows.h>
#include <string>
#include <cstdint>
#include <vector>

// PawnIO Module Loader - simplified API for pre-compiled modules
class PawnIOModule {
public:
    static PawnIOModule& Get();
    
    bool Initialize();
    bool IsInitialized() const { return m_initialized; }
    
    // Load a module from file
    bool LoadModule(const std::string& modulePath);
    
    // Execute a function in the loaded module
    bool ExecuteFunction(const std::string& functionName, const std::vector<int>& params, int& result);
    
    // High-level wrappers for common operations
    uint64_t ReadMSR(uint32_t msr);
    uint8_t ReadEC(uint8_t offset);
    void WriteEC(uint8_t offset, uint8_t value);
    
private:
    PawnIOModule();
    ~PawnIOModule();
    PawnIOModule(const PawnIOModule&) = delete;
    PawnIOModule& operator=(const PawnIOModule&) = delete;
    
    HANDLE m_hDriver = INVALID_HANDLE_VALUE;
    bool m_initialized = false;
    std::vector<uint8_t> m_moduleData;
};

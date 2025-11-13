#pragma once
#include <string>
#include <iostream>

// Simple async-like logger interface 
class AsyncLogger {
public:
    virtual ~AsyncLogger() = default;
    virtual void log(const std::string& msg) const {
        std::cout << "[LOG] " << msg << std::endl;
    }
};

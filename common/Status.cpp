/**
 * @file    Status.cpp
 * @brief   Status.h 的实现
 * @author  zzj
 * @date    2026-07-26
 */
#include "common/Status.h"

#include <utility>

Status Status::ok(){
    return Status();
}

Status Status::error(Code c, std::string msg){
    return Status(c, std::move(msg));
}

std::string Status::toString() const{
    if(msg_.empty()) return codeStr(code_);
    return std::string(codeStr(code_)) + ": " + msg_;
}

const char* codeStr(Code c){
    static const char* names[] = {
        "Ok", "InvalidArg", "IoError", "NetError", "Timeout", "Closed", "Internal"
    };
    int idx = static_cast<int>(c);
    if(idx < 0 || idx >= static_cast<int>(sizeof(names) / sizeof(names[0]))) return "Unknown";
    return names[idx];
}

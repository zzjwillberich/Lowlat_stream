/**
 * @file    Config.cpp
 * @brief   Config.h的实现
 * @author  zzj
 * @date    2026-07-23
 */

#include "common/Config.h"
#include <cstddef>
#include <cstdio>
#include <exception>
#include <string>
#include <fstream>

namespace {
    std::string trim(const std::string& s){
        size_t start = s.find_first_not_of(" \t\r");
        if(start == std::string::npos) return "";
        size_t end = s.find_last_not_of(" \t\r");
        return s.substr(start, end - start + 1);
    }

    bool isSupportedOption(const std::string& key) {
        return key == "log-level" || key == "listen" || key == "target";
    }
}

int Config::parseOne(const std::string& arg, const char* nextArg, std::string& key, std::string& value){
    // arg 字符串是否以 -- 开头
    if(arg[0] != '-' || arg[1] != '-') return 0;

    //去掉前面的 --
    std::string body = arg.substr(2);

    if(body.empty()) return 1;

    // 查找等号的位置
    size_t eqPos = body.find('=');

    if(eqPos != std::string::npos){
        // key=value格式
        key = body.substr(0,eqPos);
        value = body.substr(eqPos+1);
        return 1;
    }else{
        // key value格式
        key = body;

        if(nextArg && nextArg[0] != '\0' && !(nextArg[0] == '-' && nextArg[1] == '-')){
            value = nextArg;
            return 2;
        }else{
            value = "";
            return 1;
        }
    }
 }


bool Config::parse(int argc,char** argv){
    for(int i = 1; i < argc;/* 靠循环内的自增 */){
        if (std::string(argv[i]) == "--") {
            break;
        }

        std::string key,value;
        char* nextArg;
        nextArg = (i + 1 < argc) ? argv[i + 1] : nullptr;

        int consumed = parseOne(argv[i], nextArg, key, value);

        if(consumed == 0){
            // 无效参数
            std::fprintf(stderr, "Unknown argument %s\n", argv[i]);
            printUsage();
            return false;
        }

        // 处理内置指令
        if(key == "help"){
            printUsage();
            std::exit(0);
        }

        if(key == "config"){
            if(value.empty()){
                std::fprintf(stderr, "--config need a config file path\n");
                return false;
            }
            if(!loadFile(value)){
                std::fprintf(stderr, "Fail to load a config file %s\n", value.c_str());
                return false;
            }
        }else if (isSupportedOption(key)) {
            kv_[key] = value;
        }else {
            std::fprintf(stderr, "Unknown option --%s\n", key.c_str());
            printUsage();
            return false;
        }

        i += consumed;
    }

    return true;
}


bool Config::loadFile(const std::string& path){
    std::ifstream file(path);
    if(!file.is_open()){
        return false;
    }

    std::string line;
    int lineNo = 0;

    while(std::getline(file,line)){
        lineNo++;

        // 清楚首位空格
        line = trim(line);

        // 注释直接跳过
        if(line.empty() || line[0] == '#') continue;

        // 解析key value
        size_t eqPos = line.find('=');
        if(eqPos == std::string::npos){
            std::fprintf(stderr, "Config file %s line %d: missing a '=' in %s\n", path.c_str(), lineNo, line.c_str());
            continue;
        }

        std::string key = trim(line.substr(0,eqPos));
        std::string value = trim(line.substr(eqPos + 1));

        if(key.empty()){
            std::fprintf(stderr, "Config file %s line %d: empty key\n", path.c_str(), lineNo);
            continue;
        }

        kv_[key] = value;
    }

    return true;
}

std::set<std::string> Config::keys() const{
    std::set<std::string> result;
    for(auto& kv : kv_){
        result.insert(kv.first);
    }
    return result;
}

std::string Config::get(const std::string& key, const std::string& def) const{
    auto it = kv_.find(key);
    if(it == kv_.end()){
        return def;
    }
    return it->second;
}

int Config::getInt(const std::string& key,int def)const{
    auto it = kv_.find(key);
    if(it == kv_.end()){
        return def;
    }
    try{
        return std::stoi(it->second);
    }catch(const std::exception&){
        std::fprintf(stderr,"Config: %s is not a valid integer for --%s, using default %d\n", it->second.c_str(), key.c_str(), def);
        return def;
    }
}

void Config::printUsage() const {
    std::fprintf(stderr,
        "Usage: lowlat_<app> [options]\n"
        "Options:\n"
        "  --help                Show this help and exit\n"
        "  --config <file>       Load config file (key=value per line)\n"
        "  --log-level <level>   Log level: trace/debug/info/warn/error "
        "(default: info)\n"
        "  --listen <addr:port>  Listen address (default: 0.0.0.0:9000)\n"
        "  --target <addr:port>  Target address (default: 127.0.0.1:9000)\n"
    );
}

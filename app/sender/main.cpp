#include "common/Config.h"
#include "common/Logger.h"

int main(int argc, char** argv) {
    Config config;
    if (!config.parse(argc, argv)) {
        return 1;
    }

    Logger::instance().setLevel(parseLogLevel(config.get("log-level", "info")));

    LOG_INFO("sender", "sender starting");
    LOG_DEBUG("sender", "log-level=%s target=%s",
              config.get("log-level", "info").c_str(),
              config.get("target", "127.0.0.1:9000").c_str());
    return 0;
}

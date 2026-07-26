#include "common/Config.h"
#include "common/Logger.h"

int main(int argc, char** argv) {
    Config config;
    if (!config.parse(argc, argv)) {
        return 1;
    }

    Logger::instance().setLevel(parseLogLevel(config.get("log-level", "info")));

    LOG_INFO("receiver", "receiver starting");
    LOG_DEBUG("receiver", "log-level=%s listen=%s",
              config.get("log-level", "info").c_str(),
              config.get("listen", "0.0.0.0:9000").c_str());
    return 0;
}

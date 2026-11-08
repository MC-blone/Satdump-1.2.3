#include "core/plugin.h"
#include "logger.h"
#include "core/module.h"

#include "LoRa/module_lora_demod.h"

class LoRaSupport : public satdump::Plugin
{
public:
    std::string getID()
    {
        return "lora_support";
    }

    void init()
    {
        satdump::eventBus->register_handler<RegisterModulesEvent>(registerPluginsHandler);
    }

    static void registerPluginsHandler(const RegisterModulesEvent &evt)
    {
        // 注册你在 LoRa 模块中定义的解调器类
        REGISTER_MODULE_EXTERNAL(evt.modules_registry, fengyun4::emwin::LoRaDemodModule);
    }
};
PLUGIN_LOADER(LoRaSupport)
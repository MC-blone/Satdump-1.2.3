#include "core/plugin.h"
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
        REGISTER_MODULE_EXTERNAL(evt.modules_registry, fengyun4::emwin::LoRaDemodModule);
    }
};

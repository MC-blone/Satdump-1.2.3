#include "core/plugin.h"
#include "logger.h"
#include "core/module.h"

#include "noaa_apt/module_noaa_apt_demod.h"
#include "noaa_apt/module_noaa_apt_decoder.h"
#include "noaa_apt/noaa_apt_proj.h"

#include "generic/module_generic_analog_demod.h"

// ★ 引入 NOAA-2 模块
#include "noaa2_hrpt/module_noaa2_hrpt_demod.h"
#include "noaa2_hrpt/module_noaa2_hrpt_decoder.h"

class AnalogSupport : public satdump::Plugin
{
public:
    std::string getID() { return "analog_support"; }

    void init()
    {
        satdump::eventBus->register_handler<RegisterModulesEvent>(registerPluginsHandler);
        satdump::eventBus->register_handler<satdump::RequestSatProjEvent>(provideSatProjHandler);
    }

    static void registerPluginsHandler(const RegisterModulesEvent &evt)
    {
        REGISTER_MODULE_EXTERNAL(evt.modules_registry, noaa_apt::NOAAAPTDemodModule);
        REGISTER_MODULE_EXTERNAL(evt.modules_registry, noaa_apt::NOAAAPTDecoderModule);
        REGISTER_MODULE_EXTERNAL(evt.modules_registry, generic_analog::GenericAnalogDemodModule);
        
        // ★ 注册 NOAA-2 模块
        REGISTER_MODULE_EXTERNAL(evt.modules_registry, noaa2_hrpt::NOAA2HRPTDemodModule);
        REGISTER_MODULE_EXTERNAL(evt.modules_registry, noaa2_hrpt::NOAA2HRPTDecoderModule);
    }

    static void provideSatProjHandler(const satdump::RequestSatProjEvent &evt)
    {
        if (evt.id == "noaa_apt_single_line")
            evt.projs.push_back(std::make_shared<NOAA_APT_SatProj>(evt.cfg, evt.tle, evt.timestamps_raw));
    }
};

PLUGIN_LOADER(AnalogSupport)
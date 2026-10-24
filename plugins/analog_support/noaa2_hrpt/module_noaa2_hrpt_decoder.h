#pragma once

#include "core/module.h"
#include "imgui/imgui_image.h"
#include "common/image/image.h"
#include <vector>
#include <mutex>
#include <string>

namespace noaa2_hrpt
{
    class NOAA2HRPTDecoderModule : public ProcessingModule
    {
    protected:
        std::atomic<uint64_t> filesize;
        std::atomic<uint64_t> progress;

        int samples_per_line = 13107;
        int line_cnt = 0;

        std::vector<uint8_t> full_imagebuf;
        std::vector<uint8_t> current_line;
        
        // UI 缓存
        instrument_status_t decode_status = DECODING;
        bool has_to_update = false;
        unsigned int textureID = 0;
        uint32_t *textureBuffer = nullptr;
        
        int texture_width = 0;
        int texture_height = 0;

        float zoom = 1.0f;
        float aspect_ratio = 1.0f;

        void update_preview();

    public:
        NOAA2HRPTDecoderModule(std::string input_file, std::string output_file_hint, nlohmann::json parameters);
        ~NOAA2HRPTDecoderModule();
        void process();
        void drawUI(bool window);
        std::vector<ModuleDataType> getInputTypes() { return {DATA_FILE, DATA_STREAM}; }
        std::vector<ModuleDataType> getOutputTypes() { return {DATA_FILE}; }

    public:
        static std::string getID();
        virtual std::string getIDM() { return getID(); };
        static std::vector<std::string> getParameters();
        static std::shared_ptr<ProcessingModule> getInstance(std::string input_file, std::string output_file_hint, nlohmann::json parameters);
    };
}
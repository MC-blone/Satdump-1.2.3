#pragma once

#include "core/module.h"
#include "imgui/imgui_image.h"
#include "common/image/image.h"
#include <vector>
#include <string>
#include <mutex>

namespace noaa2_hrpt
{
    class NOAA2HRPTDecoderModule : public ProcessingModule
    {
    protected:
        // 预览纹理固定尺寸（防止内存随 line_cnt 增长失控）
        static constexpr int MAX_PREVIEW_W = 2048;
        static constexpr int MAX_PREVIEW_H = 4000;

        std::atomic<uint64_t> filesize;
        std::atomic<uint64_t> progress;

        int samples_per_line = 13107;
        int line_cnt = 0;

        // 原始全分辨率数据，仅用于最终保存
        std::vector<uint8_t> full_imagebuf;
        std::vector<uint8_t> current_line;
        std::mutex img_mtx;

        instrument_status_t decode_status = DECODING;
        bool has_to_update = false;

        // 预览纹理（固定 MAX_PREVIEW_W × MAX_PREVIEW_H）
        unsigned int textureID = 0;
        uint32_t *textureBuffer = nullptr;
        int preview_w = 0;
        int preview_h = 0;

        float zoom = 1.0f;
        float aspect_ratio = 1.0f;

        void update_preview();
        void apply_stretch_and_save(const std::vector<uint8_t>& raw_data, int w, int h,
                                    float p_low, float p_high, const std::string& filename);

    public:
        NOAA2HRPTDecoderModule(std::string input_file, std::string output_file_hint, nlohmann::json parameters);
        virtual ~NOAA2HRPTDecoderModule();
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
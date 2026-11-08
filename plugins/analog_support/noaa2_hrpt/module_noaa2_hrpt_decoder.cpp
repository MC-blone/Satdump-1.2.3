#include "module_noaa2_hrpt_decoder.h"
#include "logger.h"
#include "imgui/imgui.h"
#include "common/utils.h"
#include "common/image/io.h"
#include "common/image/processing.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <ctime>
#include <vector>

namespace noaa2_hrpt
{
    NOAA2HRPTDecoderModule::NOAA2HRPTDecoderModule(std::string input_file, std::string output_file_hint, nlohmann::json parameters)
        : ProcessingModule(input_file, output_file_hint, parameters)
    {
        if (parameters.contains("samples_per_line")) samples_per_line = parameters["samples_per_line"].get<int>();
        if (samples_per_line < 1) samples_per_line = 13107;
        full_imagebuf.reserve((size_t)samples_per_line * 5000);
        current_line.reserve(samples_per_line * 2);
        filesize = 0;
        progress = 0;
    }

    NOAA2HRPTDecoderModule::~NOAA2HRPTDecoderModule() {
        if (textureBuffer != nullptr) { delete[] textureBuffer; textureBuffer = nullptr; }
        if (textureID != 0) deleteImageTexture(textureID);
    }

    // 关键改动：
    // 1) 纹理固定 MAX_PREVIEW_W × MAX_PREVIEW_H，一次分配，永不重分配
    // 2) min/max 用采样估算（行抽样 + 列抽样），避免每次扫全图
    // 3) 只更新有效区域 [0, preview_h) × [0, preview_w)
    void NOAA2HRPTDecoderModule::update_preview() {
        std::lock_guard<std::mutex> lock(img_mtx);
        if (line_cnt <= 0 || full_imagebuf.empty()) return;

        preview_w = std::min(samples_per_line, MAX_PREVIEW_W);
        preview_h = std::min(line_cnt, MAX_PREVIEW_H);
        if (preview_w <= 0 || preview_h <= 0) return;

        aspect_ratio = (float)samples_per_line / (float)line_cnt;

        // 一次性分配固定尺寸纹理
        if (textureBuffer == nullptr) {
            textureBuffer = new uint32_t[(size_t)MAX_PREVIEW_W * MAX_PREVIEW_H];
            for (size_t i = 0; i < (size_t)MAX_PREVIEW_W * MAX_PREVIEW_H; ++i)
                textureBuffer[i] = 0xFF000000u;
            textureID = makeImageTexture();
        }

        // 采样估算 min/max（避免扫 2.6 亿像素）
        int vmin = 255, vmax = 0;
        int row_step = std::max(1, line_cnt / 512);
        int col_step = std::max(1, samples_per_line / 32);
        for (int y = 0; y < line_cnt; y += row_step) {
            const uint8_t *row = &full_imagebuf[(size_t)y * samples_per_line];
            for (int x = 0; x < samples_per_line; x += col_step) {
                uint8_t v = row[x];
                if (v < vmin) vmin = v;
                if (v > vmax) vmax = v;
            }
        }
        if (vmax == vmin) vmax = vmin + 1;

        // 采样填充预览（从 full_imagebuf 按比例取点）
        const uint32_t alpha = 255u << 24;
        for (int py = 0; py < preview_h; ++py) {
            int sy = (int)((int64_t)py * line_cnt / preview_h);
            const uint8_t *row = &full_imagebuf[(size_t)sy * samples_per_line];
            uint32_t *dst = &textureBuffer[(size_t)py * MAX_PREVIEW_W];
            for (int px = 0; px < preview_w; ++px) {
                int sx = (int)((int64_t)px * samples_per_line / preview_w);
                uint8_t stretched = (uint8_t)(((int)row[sx] - vmin) * 255 / (vmax - vmin));
                dst[px] = alpha | ((uint32_t)stretched << 16) | ((uint32_t)stretched << 8) | stretched;
            }
        }

        updateImageTexture(textureID, textureBuffer, MAX_PREVIEW_W, MAX_PREVIEW_H);
        has_to_update = false;
    }

    void NOAA2HRPTDecoderModule::apply_stretch_and_save(const std::vector<uint8_t>& raw_data, int w, int h, float p_low, float p_high, const std::string& filename) {
        // 性能优化：用采样估算百分位，避免对 2.6 亿像素做全排序（原实现 30s~3min）
        // 目标 ~100 万采样点，排序 < 1s，百分位精度足够（像素值仅 256 种）
        const size_t TARGET_SAMPLES = 1000000;
        size_t step = std::max<size_t>(1, raw_data.size() / TARGET_SAMPLES);

        std::vector<uint8_t> sample;
        sample.reserve(raw_data.size() / step + 1);
        for (size_t i = 0; i < raw_data.size(); i += step)
            sample.push_back(raw_data[i]);
        std::sort(sample.begin(), sample.end());

        int pmin = 0, pmax = 255;
        if (!sample.empty()) {
            size_t lo_idx = (size_t)(sample.size() * (p_low / 100.0));
            size_t hi_idx = (size_t)(sample.size() * (p_high / 100.0));
            if (lo_idx >= sample.size()) lo_idx = sample.size() - 1;
            if (hi_idx >= sample.size()) hi_idx = sample.size() - 1;
            pmin = sample[lo_idx];
            pmax = sample[hi_idx];
            if (pmax <= pmin) pmax = pmin + 1;
        }

        logger->debug("Stretch [%s]: raw=%zu, sample=%zu, step=%zu, pmin=%d, pmax=%d",
                      filename.c_str(), raw_data.size(), sample.size(), step, pmin, pmax);

        // 全量 stretch（O(N) 线性，非瓶颈）
        std::vector<uint8_t> stretched(raw_data.size());
        for (size_t i = 0; i < raw_data.size(); ++i) {
            stretched[i] = (uint8_t)std::clamp((raw_data[i] - pmin) * 255 / (pmax - pmin), 0, 255);
        }

        image::Image img(stretched.data(), 8, w, h, 1);
        if (w != samples_per_line) img.resize(samples_per_line, h);
        image::save_img(img, filename);
    }

    void NOAA2HRPTDecoderModule::process() {
        filesize = input_data_type == DATA_FILE ? getFilesize(d_input_file) : 0;
        progress = filesize.load();
        line_cnt = 0;
        full_imagebuf.clear();
        current_line.clear();
        decode_status = DECODING;

        std::string main_dir = d_output_file_hint;
        size_t slash = main_dir.find_last_of("/\\");
        if (slash != std::string::npos) main_dir.resize(slash);
        else main_dir = ".";

        std::ifstream in;
        bool is_u8 = false;
        int sp_out = samples_per_line * 2; // 120kHz 下的一行像素数

        if (input_data_type == DATA_FILE) {
            in.open(d_input_file, std::ios::binary);
            char header[44];
            in.read(header, 44);
            if (in.gcount() == 44 && std::memcmp(header, "RIFF", 4) == 0 && std::memcmp(header + 8, "WAVE", 4) == 0) {
                is_u8 = false;
            } else {
                in.seekg(0, std::ios::beg);
                is_u8 = true;
            }
        }

        std::vector<uint8_t> raw(65536);
        time_t last_log = 0;
        int last_preview_line = 0;

        while (input_data_type == DATA_FILE ? (bool)in : input_active.load()) {
            size_t bytes_read = 0;
            if (input_data_type == DATA_FILE) {
                in.read((char *)raw.data(), raw.size());
                bytes_read = (size_t)in.gcount();
                if (bytes_read == 0) break;
                progress = (uint64_t)in.tellg();
                if (filesize.load() > 0 && progress.load() > filesize.load()) progress = filesize.load();
            } else {
                if (!input_active.load()) break;
                if (input_fifo->read(raw.data(), raw.size()) < 0) break;
                bytes_read = raw.size();
            }

            {
                std::lock_guard<std::mutex> lock(img_mtx);
                if (is_u8) {
                    for (size_t i = 0; i < bytes_read; ++i) {
                        current_line.push_back(raw[i]);
                        if ((int)current_line.size() == sp_out) {
                            full_imagebuf.insert(full_imagebuf.end(), current_line.begin(), current_line.end());
                            current_line.clear(); ++line_cnt;
                        }
                    }
                } else {
                    size_t usable = bytes_read & ~(size_t)1;
                    for (size_t i = 0; i < usable; i += 2) {
                        int16_t sample;
                        std::memcpy(&sample, raw.data() + i, sizeof(sample));
                        int value = std::clamp(((int)sample >> 8) + 128, 0, 255);
                        current_line.push_back((uint8_t)value);
                        if ((int)current_line.size() == sp_out) {
                            full_imagebuf.insert(full_imagebuf.end(), current_line.begin(), current_line.end());
                            current_line.clear(); ++line_cnt;
                        }
                    }
                }
            }

            if (line_cnt - last_preview_line >= 10) {
                update_preview();
                last_preview_line = line_cnt;
                has_to_update = true;
            }

            time_t now = time(nullptr);
            if (now != last_log && now % 5 == 0) {
                last_log = now;
                double pct = filesize.load() > 0 ? std::min(100.0, (double)progress.load() * 100.0 / (double)filesize.load()) : 0.0;
                logger->info("Progress %.1f%% | Lines: %d", pct, line_cnt);
            }
        }

        if (in.is_open()) in.close();

        decode_status = PROCESSING;
        {
            std::lock_guard<std::mutex> lock(img_mtx);
            if (!current_line.empty()) {
                current_line.resize(sp_out, 0);
                full_imagebuf.insert(full_imagebuf.end(), current_line.begin(), current_line.end());
                current_line.clear(); ++line_cnt;
            }
        }

        if (line_cnt > 0) {
            update_preview();
            has_to_update = true;
            decode_status = SAVING;

            time_t now = time(nullptr);
            std::tm tm_now{};
#if defined(_WIN32)
            localtime_s(&tm_now, &now);
#else
            localtime_r(&now, &tm_now);
#endif

            std::stringstream ss;
            ss << std::setfill('0') << std::setw(4) << tm_now.tm_year + 1900 << "-" << std::setw(2) << tm_now.tm_mon + 1 << "-" << std::setw(2) << tm_now.tm_mday << "_" << std::setw(2) << tm_now.tm_hour << "-" << std::setw(2) << tm_now.tm_min << "-" << std::setw(2) << tm_now.tm_sec;

            std::string base_name = main_dir + "/" + ss.str() + "_noaa_2_hrpt_fm";

            logger->info("Applying percentiles stretch and saving...");
            {
                std::lock_guard<std::mutex> lock(img_mtx);
                apply_stretch_and_save(full_imagebuf, sp_out, line_cnt, 0.1f, 99.9f, base_name + "_raw.png");
                apply_stretch_and_save(full_imagebuf, sp_out, line_cnt, 2.0f, 98.0f, base_name + "_Wibt.png");
                std::vector<uint8_t>().swap(full_imagebuf);
            }
            logger->info("Decode complete and images saved.");
        } else {
            logger->error("No complete image lines decoded");
        }

        decode_status = DONE;
    }

    void NOAA2HRPTDecoderModule::drawUI(bool window) {
        ImGui::Begin("NOAA-2 HRPT Decoder", nullptr, window ? 0 : NOWINDOW_FLAGS);

        if (textureID == 0) {
            textureID = makeImageTexture();
            textureBuffer = new uint32_t[(size_t)MAX_PREVIEW_W * MAX_PREVIEW_H];
            for (size_t i = 0; i < (size_t)MAX_PREVIEW_W * MAX_PREVIEW_H; ++i)
                textureBuffer[i] = 0xFF000000u;
            updateImageTexture(textureID, textureBuffer, MAX_PREVIEW_W, MAX_PREVIEW_H);
        }

        if (has_to_update) update_preview();

        float avail_width = std::max(100.0f, ImGui::GetContentRegionAvail().x - 100 * ui_scale);
        float avail_height = 300 * ui_scale;
        ImGui::BeginChild("ImageScroll", ImVec2(avail_width, avail_height), true, ImGuiWindowFlags_HorizontalScrollbar);

        float ratio = aspect_ratio > 0 ? aspect_ratio : 1.0f;
        float display_height = avail_height * zoom;
        float display_width = display_height * ratio;
        if (display_width > avail_width * zoom) {
            display_width = avail_width * zoom;
            display_height = display_width / ratio;
        }

        if (textureID != 0 && textureBuffer != nullptr && preview_w > 0 && preview_h > 0) {
            // 只用纹理有效区域（UV 裁剪到 preview_w × preview_h）
            ImVec2 uv0(0.0f, 0.0f);
            ImVec2 uv1((float)preview_w / (float)MAX_PREVIEW_W,
                       (float)preview_h / (float)MAX_PREVIEW_H);
            ImGui::Image((void *)(intptr_t)textureID, ImVec2(display_width, display_height), uv0, uv1);
            if (ImGui::IsItemHovered() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                ImVec2 delta = ImGui::GetIO().MouseDelta;
                ImGui::SetScrollX(ImGui::GetScrollX() - delta.x);
                ImGui::SetScrollY(ImGui::GetScrollY() - delta.y);
            }
        }
        ImGui::EndChild();
        ImGui::SameLine();

        ImGui::BeginGroup();
        ImGui::Button("Controls", ImVec2(80 * ui_scale, 20 * ui_scale));
        if (ImGui::Button("Zoom +", ImVec2(80 * ui_scale, 30 * ui_scale))) zoom = std::min(zoom + 0.2f, 5.0f);
        if (ImGui::Button("Zoom -", ImVec2(80 * ui_scale, 30 * ui_scale))) zoom = std::max(zoom - 0.2f, 0.1f);
        if (ImGui::Button("Reset", ImVec2(80 * ui_scale, 30 * ui_scale))) { zoom = 1.0f; ImGui::SetScrollX(0); ImGui::SetScrollY(0); }
        ImGui::Spacing();
        ImGui::Button("Status", ImVec2(80 * ui_scale, 20 * ui_scale));
        drawStatus(decode_status);
        ImGui::Spacing();
        ImGui::Text("Lines : %d", line_cnt);
        ImGui::Text("Preview: %d x %d", preview_w, preview_h);
        ImGui::Text("Ratio : %.2f", aspect_ratio);
        ImGui::Text("Zoom  : %.1fx", zoom);
        ImGui::EndGroup();

        if (input_data_type == DATA_FILE) {
            float pct = filesize.load() > 0 ? std::min(1.0f, (float)progress.load() / (float)filesize.load()) : 0.0f;
            ImGui::ProgressBar(pct, ImVec2(ImGui::GetContentRegionAvail().x, 20 * ui_scale));
        }
        ImGui::End();
    }

    std::string NOAA2HRPTDecoderModule::getID() { return "noaa2_hrpt_decoder"; }
    std::vector<std::string> NOAA2HRPTDecoderModule::getParameters() { return {"samples_per_line"}; }
    std::shared_ptr<ProcessingModule> NOAA2HRPTDecoderModule::getInstance(std::string input_file, std::string output_file_hint, nlohmann::json parameters) { return std::make_shared<NOAA2HRPTDecoderModule>(input_file, output_file_hint, parameters); }
}
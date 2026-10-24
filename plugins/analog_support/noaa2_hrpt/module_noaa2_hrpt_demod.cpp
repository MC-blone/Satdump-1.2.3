#include "module_noaa2_hrpt_decoder.h"
#include "logger.h"
#include "imgui/imgui.h"
#include "common/utils.h"
#include "common/image/io.h"
#include "common/image/processing.h"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <ctime>
#include <vector>

namespace noaa2_hrpt
{
    static uint16_t read_u16(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
    static uint32_t read_u32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
    static std::string lower_ext(const std::string &path) {
        size_t p = path.find_last_of('.');
        if (p == std::string::npos) return "";
        std::string ext = path.substr(p);
        for (char &c : ext) c = (char)std::tolower((unsigned char)c);
        return ext;
    }

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

    void NOAA2HRPTDecoderModule::update_preview() {
        if (line_cnt <= 0 || full_imagebuf.size() < (size_t)samples_per_line * line_cnt) return;
        aspect_ratio = (float)samples_per_line / (float)line_cnt;
        const int max_size = 2500;
        int w, h;
        if (samples_per_line >= line_cnt) {
            w = std::min(samples_per_line, max_size);
            h = std::max(1, (int)(w / aspect_ratio));
        } else {
            h = std::min(line_cnt, max_size);
            w = std::max(1, (int)(h * aspect_ratio));
        }
        if (textureBuffer == nullptr || texture_width != w || texture_height != h) {
            delete[] textureBuffer;
            textureBuffer = new uint32_t[(size_t)w * h];
            texture_width = w;
            texture_height = h;
            if (textureID != 0) deleteImageTexture(textureID);
            textureID = makeImageTexture();
        }
        for (int y = 0; y < h; ++y) {
            int sy = (int)((int64_t)y * line_cnt / h);
            for (int x = 0; x < w; ++x) {
                int sx = (int)((int64_t)x * samples_per_line / w);
                uint8_t v = full_imagebuf[(size_t)sy * samples_per_line + sx];
                textureBuffer[(size_t)y * w + x] = (255u << 24) | ((uint32_t)v << 16) | ((uint32_t)v << 8) | v;
            }
        }
        updateImageTexture(textureID, textureBuffer, w, h);
        has_to_update = false;
    }

    void NOAA2HRPTDecoderModule::apply_stretch_and_save(const std::vector<uint8_t>& raw_data, int w, int h, float p_low, float p_high, const std::string& filename) {
        std::vector<uint8_t> sorted = raw_data;
        std::sort(sorted.begin(), sorted.end());
        int pmin_idx = std::clamp((int)(sorted.size() * (p_low / 100.0)), 0, (int)sorted.size() - 1);
        int pmax_idx = std::clamp((int)(sorted.size() * (p_high / 100.0)), 0, (int)sorted.size() - 1);
        
        int pmin = sorted[pmin_idx];
        int pmax = sorted[pmax_idx];
        if (pmax <= pmin) pmax = pmin + 1;

        std::vector<uint8_t> stretched(raw_data.size());
        for (size_t i = 0; i < raw_data.size(); ++i) {
            int val = raw_data[i];
            int mapped = std::clamp((val - pmin) * 255 / (pmax - pmin), 0, 255);
            stretched[i] = (uint8_t)mapped;
        }

        image::Image img(stretched.data(), 8, w, h, 1);
        if (w != samples_per_line) {
            img.resize(samples_per_line, h);
        }
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
        bool is_u8 = false, is_wav = false;
        uint16_t wav_bits = 16, wav_channels = 1, wav_format = 1;
        uint64_t data_start = 0, data_length = 0;

        if (input_data_type == DATA_FILE) {
            in.open(d_input_file, std::ios::binary);
            if (!in) { logger->error("Cannot open input file: " + d_input_file); decode_status = DONE; return; }

            std::string ext = lower_ext(d_input_file);
            is_u8 = ext == ".u8";
            is_wav = ext == ".wav";

            if (is_wav) {
                uint8_t riff[12];
                in.read((char *)riff, sizeof(riff));
                if (in.gcount() != sizeof(riff) || std::memcmp(riff, "RIFF", 4) != 0 || std::memcmp(riff + 8, "WAVE", 4) != 0) {
                    logger->error("Invalid WAV/RIFF header"); decode_status = DONE; return;
                }

                bool got_fmt = false, got_data = false;
                while (in && !got_data) {
                    uint8_t chunk[8];
                    in.read((char *)chunk, sizeof(chunk));
                    if (in.gcount() != sizeof(chunk)) break;
                    uint32_t chunk_size = read_u32(chunk + 4);
                    std::streampos chunk_data = in.tellg();

                    if (std::memcmp(chunk, "fmt ", 4) == 0) {
                        if (chunk_size < 16 || chunk_size > 1024 * 1024) break;
                        std::vector<uint8_t> fmt(chunk_size);
                        in.read((char *)fmt.data(), chunk_size);
                        if ((uint32_t)in.gcount() != chunk_size) break;
                        wav_format = read_u16(fmt.data());
                        wav_channels = read_u16(fmt.data() + 2);
                        wav_bits = read_u16(fmt.data() + 14);
                        got_fmt = true;
                    } else if (std::memcmp(chunk, "data", 4) == 0) {
                        data_start = (uint64_t)chunk_data;
                        data_length = chunk_size;
                        got_data = true;
                        break;
                    }
                    in.clear();
                    in.seekg(chunk_data + (std::streamoff)chunk_size + (std::streamoff)(chunk_size & 1));
                }
                if (!got_fmt || !got_data || wav_format != 1 || wav_channels != 1 || (wav_bits != 8 && wav_bits != 16)) {
                    logger->error("Unsupported WAV format"); decode_status = DONE; return;
                }
                is_u8 = wav_bits == 8;
                in.clear();
                in.seekg((std::streamoff)data_start);
            }
        }

        // 因为解调器输出的是 120kHz，一行对应 120k 采样点
        // 按照 13107 标准宽度，120k 下是 26214
        int sp_out = samples_per_line * 2; 

        std::vector<uint8_t> raw(65536);
        time_t last_log = 0;
        int last_preview_line = 0;
        uint64_t wav_read = 0;
        bool stream_finished = false;

        while (input_data_type == DATA_FILE ? (bool)in : input_active.load()) {
            size_t bytes_read = 0;
            if (input_data_type == DATA_FILE) {
                size_t want = raw.size();
                if (is_wav && data_length > 0) {
                    if (wav_read >= data_length) break;
                    want = (size_t)std::min<uint64_t>(want, data_length - wav_read);
                }
                in.read((char *)raw.data(), (std::streamsize)want);
                bytes_read = (size_t)in.gcount();
                if (bytes_read == 0) break;
                wav_read += bytes_read;
                progress = (uint64_t)in.tellg();
                if (is_wav) progress = data_start + wav_read;
                if (filesize.load() > 0 && progress.load() > filesize.load()) progress = filesize.load();
            } else {
                int read_ret = input_fifo->read(reinterpret_cast<uint8_t *>(raw.data()), raw.size());
                if (read_ret < 0) break;
                bytes_read = (size_t)read_ret;
            }

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
            if (input_data_type != DATA_FILE && !input_active.load()) stream_finished = true;
            if (stream_finished) break;
        }

        if (in.is_open()) in.close();
        if (!current_line.empty()) {
            current_line.resize(sp_out, 0);
            full_imagebuf.insert(full_imagebuf.end(), current_line.begin(), current_line.end());
            current_line.clear(); ++line_cnt;
        }

        // 保护性设计：避免卡死导致不做保存
        decode_status = PROCESSING;
        if (line_cnt > 0) {
            update_preview();
            has_to_update = true;
            decode_status = SAVING;

            time_t now = time(nullptr);
            std::tm tm_now{};
            localtime_r(&now, &tm_now);

            std::stringstream ss;
            ss << std::setfill('0') << std::setw(4) << tm_now.tm_year + 1900 << "-" << std::setw(2) << tm_now.tm_mon + 1 << "-" << std::setw(2) << tm_now.tm_mday << "_" << std::setw(2) << tm_now.tm_hour << "-" << std::setw(2) << tm_now.tm_min << "-" << std::setw(2) << tm_now.tm_sec;

            std::string base_name = main_dir + "/" + ss.str() + "_noaa_2_hrpt_fm";
            
            logger->info("Applying percentiles stretch and saving...");
            apply_stretch_and_save(full_imagebuf, sp_out, line_cnt, 0.1f, 99.9f, base_name + "_原始动态.png");
            apply_stretch_and_save(full_imagebuf, sp_out, line_cnt, 2.0f, 98.0f, base_name + "_增强对比.png");

            std::vector<uint8_t>().swap(full_imagebuf);
            logger->info("Decode complete and images saved.");
        } else {
            logger->error("No complete image lines decoded");
        }

        decode_status = DONE;
    }

    void NOAA2HRPTDecoderModule::drawUI(bool window) {
        ImGui::Begin("NOAA-2 HRPT Decoder", nullptr, window ? 0 : NOWINDOW_FLAGS);
        if (textureID == 0) textureID = makeImageTexture();

        float avail_width = std::max(100.0f, ImGui::GetContentRegionAvail().x - 100 * ui_scale);
        float avail_height = 300 * ui_scale;
        ImGui::BeginChild("ImageScroll", ImVec2(avail_width, avail_height), true, ImGuiWindowFlags_HorizontalScrollbar);

        float ratio = aspect_ratio > 0 ? aspect_ratio : 1.0f;
        float display_height = avail_height * zoom;
        float display_width = display_height * ratio;
        if (display_width > avail_width * zoom) { display_width = avail_width * zoom; display_height = display_width / ratio; }

        if (textureID != 0 && textureBuffer != nullptr) {
            ImGui::Image((void *)(intptr_t)textureID, ImVec2(display_width, display_height));
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
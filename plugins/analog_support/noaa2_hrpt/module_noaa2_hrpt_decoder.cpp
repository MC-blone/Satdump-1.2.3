#include "module_noaa2_hrpt_decoder.h"
#include "logger.h"
#include "imgui/imgui.h"
#include "common/utils.h"
#include "common/image/processing.h"
#include "common/image/io.h"
#include <iomanip>
#include <sstream>
#include <ctime>
#include <algorithm>

namespace noaa2_hrpt
{
    NOAA2HRPTDecoderModule::NOAA2HRPTDecoderModule(std::string input_file, std::string output_file_hint, nlohmann::json parameters)
        : ProcessingModule(input_file, output_file_hint, parameters)
    {
        if (parameters.count("samples_per_line") > 0)
            samples_per_line = parameters["samples_per_line"].get<int>();

        // 预留内存，防止频繁 realloc 导致卡顿 (预估大概 5000 行)
        full_imagebuf.reserve(samples_per_line * 5000);
        current_line.reserve(samples_per_line);
    }

    NOAA2HRPTDecoderModule::~NOAA2HRPTDecoderModule()
    {
        if (textureID != 0)
        {
            delete[] textureBuffer;
            // deleteImageTexture(textureID);
        }
    }

    void NOAA2HRPTDecoderModule::update_preview()
    {
        if (line_cnt == 0) return;

        // 计算自适应长宽比
        aspect_ratio = (float)samples_per_line / (float)line_cnt;

        // 限制预览纹理的最大分辨率 (比如 2500)，防止内存过大和渲染卡顿
        int max_preview_size = 2500;
        int p_width, p_height;

        if (samples_per_line > line_cnt)
        {
            p_width = std::min(samples_per_line, max_preview_size);
            p_height = p_width / aspect_ratio;
        }
        else
        {
            p_height = std::min(line_cnt, max_preview_size);
            p_width = p_height * aspect_ratio;
        }

        if (p_width < 1) p_width = 1;
        if (p_height < 1) p_height = 1;

        // 如果分配大小不对，重新分配
        static int last_w = 0, last_h = 0;
        if (textureBuffer == nullptr || last_w != p_width || last_h != p_height)
        {
            if (textureBuffer) delete[] textureBuffer;
            textureBuffer = new uint32_t[p_width * p_height];
            last_w = p_width;
            last_h = p_height;
            if (textureID != 0) deleteImageTexture(textureID);
            textureID = makeImageTexture();
        }

        // 缩略图渲染
        for (int y = 0; y < p_height; y++)
        {
            int src_y = (y * line_cnt) / p_height;
            for (int x = 0; x < p_width; x++)
            {
                int src_x = (x * samples_per_line) / p_width;
                uint8_t val = full_imagebuf[src_y * samples_per_line + src_x];
                
                // 转 RGBA
                uint32_t color = (255 << 24) | (val << 16) | (val << 8) | val;
                textureBuffer[y * p_width + x] = color;
            }
        }

        updateImageTexture(textureID, textureBuffer, p_width, p_height);
        has_to_update = false;
    }

    void NOAA2HRPTDecoderModule::process()
    {
        if (input_data_type == DATA_FILE)
            filesize = getFilesize(d_input_file);
        else
            filesize = 0;

        std::ifstream data_in;
        if (input_data_type == DATA_FILE)
            data_in = std::ifstream(d_input_file, std::ios::binary);

        std::string main_dir = d_output_file_hint.substr(0, d_output_file_hint.rfind('/'));

        logger->info("Decoding NOAA-2 HRPT Image...");

        time_t lastTime = 0;
        int16_t read_buf[4096];

        while (input_data_type == DATA_FILE ? !data_in.eof() : input_active.load())
        {
            int bytes_read = 0;
            if (input_data_type == DATA_FILE)
            {
                data_in.read((char *)read_buf, sizeof(read_buf));
                bytes_read = data_in.gcount();
            }
            else
            {
                input_fifo->read((uint8_t *)read_buf, sizeof(read_buf));
                bytes_read = sizeof(read_buf);
            }

            if (bytes_read <= 0) break;
            int samples_read = bytes_read / 2;

            for (int i = 0; i < samples_read; i++)
            {
                // 映射到 0-255 (使用 int 计算并截断，防止溢出)
                int val = (read_buf[i] / 256) + 128;
                if (val < 0) val = 0;
                if (val > 255) val = 255;
                
                current_line.push_back((uint8_t)val);
                
                if (current_line.size() == samples_per_line)
                {
                    // 一整行接收完毕，推入全图缓冲
                    full_imagebuf.insert(full_imagebuf.end(), current_line.begin(), current_line.end());
                    current_line.clear();
                    line_cnt++;
                }
            }

            // ★ 实时预览更新逻辑（每 10 行更新一次，节约 UI 性能）
            if (textureID != 0)
            {
                static int last_ui_line = 0;
                if (line_cnt - last_ui_line >= 10)
                {
                    update_preview();
                    last_ui_line = line_cnt;
                }
            }

            if (input_data_type == DATA_FILE)
                progress = data_in.tellg();

            if (time(NULL) % 10 == 0 && lastTime != time(NULL))
            {
                lastTime = time(NULL);
                logger->info("Progress " + std::to_string(round(((double)progress / (double)filesize) * 1000.0) / 10.0) + "%% | Lines: " + std::to_string(line_cnt));
            }
        }

        if (input_data_type == DATA_FILE)
            data_in.close();

        decode_status = PROCESSING;

        // 把最后没画完的行补齐
        if (current_line.size() > 0)
        {
            current_line.resize(samples_per_line, 0);
            full_imagebuf.insert(full_imagebuf.end(), current_line.begin(), current_line.end());
            line_cnt++;
        }

        if (line_cnt > 0)
        {
            decode_status = SAVING;
            
            // 生成时间戳
            time_t now = time(0);
            std::tm *ltm = localtime(&now);
            std::stringstream ss;
            ss << std::setfill('0') << std::setw(4) << (1900 + ltm->tm_year) << "-"
               << std::setw(2) << (1 + ltm->tm_mon) << "-"
               << std::setw(2) << ltm->tm_mday << "_"
               << std::setw(2) << ltm->tm_hour << "-"
               << std::setw(2) << ltm->tm_min << "-"
               << std::setw(2) << ltm->tm_sec;
               
            std::string filename = main_dir + "/" + ss.str() + "_noaa_2_hrpt_fm.png";
            logger->info("Saving full resolution image to: " + filename);
            
            // ★ 直接利用 vector 的底层指针生成 Image，瞬间完成，无需再次拷贝
            image::Image full_img(full_imagebuf.data(), 8, samples_per_line, line_cnt, 1);
            image::save_img(full_img, filename);

            // ★ 参考 Meteor/APT 的做法，保存后立刻释放内存防止 OOM
            std::vector<uint8_t>().swap(full_imagebuf);
        }

        decode_status = DONE;
    }

    void NOAA2HRPTDecoderModule::drawUI(bool window)
    {
        ImGui::Begin("NOAA-2 HRPT Decoder", NULL, window ? 0 : NOWINDOW_FLAGS);

        ImGui::BeginGroup();
        {
            if (textureID == 0)
            {
                textureID = makeImageTexture();
            }

            if (has_to_update)
            {
                // has_to_update 在 update_preview 中已处理
            }

            // ★ 图像显示区域，支持鼠标/触屏拖动
            float avail_width = ImGui::GetContentRegionAvail().x - 100 * ui_scale;
            float avail_height = 300 * ui_scale;

            ImGui::BeginChild("ImageScroll", ImVec2(avail_width, avail_height), true, ImGuiWindowFlags_HorizontalScrollbar);
            
            // 根据 aspect_ratio 和 zoom 计算显示尺寸
            float display_height = avail_height * zoom;
            float display_width = display_height * aspect_ratio;

            // 如果是超宽图，用宽度来算
            if (display_width > avail_width * zoom)
            {
                display_width = avail_width * zoom;
                display_height = display_width / aspect_ratio;
            }

            ImGui::Image((void *)(intptr_t)textureID, ImVec2(display_width, display_height));

            // 处理鼠标/触摸拖动 (原生支持)
            if (ImGui::IsItemHovered() && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
            {
                ImVec2 delta = ImGui::GetIO().MouseDelta;
                ImGui::SetScrollX(ImGui::GetScrollX() - delta.x);
                ImGui::SetScrollY(ImGui::GetScrollY() - delta.y);
            }

            ImGui::EndChild();
        }
        ImGui::EndGroup();

        ImGui::SameLine();

        // ★ 右侧控制面板
        ImGui::BeginGroup();
        {
            ImGui::Button("Controls", {80 * ui_scale, 20 * ui_scale});
            
            if (ImGui::Button("Zoom +", ImVec2(80 * ui_scale, 30 * ui_scale)))
            {
                zoom = std::min(zoom + 0.2f, 5.0f);
            }
            if (ImGui::Button("Zoom -", ImVec2(80 * ui_scale, 30 * ui_scale)))
            {
                zoom = std::max(zoom - 0.2f, 0.1f);
            }
            if (ImGui::Button("Reset", ImVec2(80 * ui_scale, 30 * ui_scale)))
            {
                zoom = 1.0f;
                // 重置滚动位置到左上角
                ImGui::SetScrollX(0);
                ImGui::SetScrollY(0);
            }

            ImGui::Spacing();
            ImGui::Button("Status", {80 * ui_scale, 20 * ui_scale});
            drawStatus(decode_status);
            
            ImGui::Spacing();
            ImGui::Text("Lines : %d", line_cnt);
            ImGui::Text("Ratio : %.2f", aspect_ratio);
            ImGui::Text("Zoom  : %.1fx", zoom);
        }
        ImGui::EndGroup();

        if (input_data_type == DATA_FILE)
            ImGui::ProgressBar((double)progress / (double)filesize, ImVec2(ImGui::GetContentRegionAvail().x, 20 * ui_scale));

        ImGui::End();
    }

    std::string NOAA2HRPTDecoderModule::getID() { return "noaa2_hrpt_decoder"; }
    std::vector<std::string> NOAA2HRPTDecoderModule::getParameters() { return {"samples_per_line"}; }
    std::shared_ptr<ProcessingModule> NOAA2HRPTDecoderModule::getInstance(std::string input_file, std::string output_file_hint, nlohmann::json parameters) { return std::make_shared<NOAA2HRPTDecoderModule>(input_file, output_file_hint, parameters); }
}
#include "module_svissr_image_decoder.h"
#include "dump_svissr_telemetry.h"
#include "logger.h"
#include "imgui/imgui.h"
#include "imgui/imgui_image.h"
#include <filesystem>
#include "resources.h"
#include "common/utils.h"
#include "common/image/io.h"
#include "utils/majority_law.h"

#define FRAME_SIZE 44356
#define SUBCOM_START_OFFSET 196
#define SUBCOM_GROUP_SIZE 2097

uint64_t getFilesize(std::string filepath);

std::map<fengyun_svissr::SVISSRSubCommunicationBlockType, fengyun_svissr::SVISSRSubcommunicationBlock> subcom_blocks = {
    {fengyun_svissr::Simplified_mapping, {0, 99}}, {fengyun_svissr::Orbit_and_attitude, {100, 227}}, {fengyun_svissr::MANAM, {228, 637}},
    {fengyun_svissr::Calibration_1, {638, 893}},   {fengyun_svissr::Calibration_2, {894, 1917}},     {fengyun_svissr::Spare, {1918, 2096}}};

namespace fengyun_svissr
{
    std::string SVISSRImageDecoderModule::getSvissrFilename(std::tm *timeReadable, std::string channel)
    {
        std::string utc_filename = sat_name + "_" + channel + "_" +
                                   std::to_string(timeReadable->tm_year + 1900) +
                                   (timeReadable->tm_mon + 1 > 9 ? std::to_string(timeReadable->tm_mon + 1) : "0" + std::to_string(timeReadable->tm_mon + 1)) +
                                   (timeReadable->tm_mday > 9 ? std::to_string(timeReadable->tm_mday) : "0" + std::to_string(timeReadable->tm_mday)) + "T" +
                                   (timeReadable->tm_hour > 9 ? std::to_string(timeReadable->tm_hour) : "0" + std::to_string(timeReadable->tm_hour)) +
                                   (timeReadable->tm_min > 9 ? std::to_string(timeReadable->tm_min) : "0" + std::to_string(timeReadable->tm_min)) +
                                   (timeReadable->tm_sec > 9 ? std::to_string(timeReadable->tm_sec) : "0" + std::to_string(timeReadable->tm_sec)) + "Z";
        return utc_filename;
    }

    std::vector<uint8_t> get_subcom_block(MinorFrame subcom_frame, SVISSRSubCommunicationBlockType block_type)
    {
        std::vector<uint8_t> output;
        for (int group = 0; group < 25; group++) {
            for (int byte = subcom_blocks[block_type].start_offset; byte <= subcom_blocks[block_type].end_offset; byte++) {
                output.push_back(subcom_frame[group * SUBCOM_GROUP_SIZE + byte]);
            }
        }
        return output;
    }

    int get_spare_errors(MinorFrame subcom_frame)
    {
        std::vector<uint8_t> spare = get_subcom_block(subcom_frame, Spare);
        int errors = 0;
        for (int byte = 0; byte < 4475; byte++) {
            uint8_t cur_byte = spare[byte];
            for (int bit = 0; bit < 8; bit++) if (((cur_byte >> bit) & 0x1) == 1) errors++;
        }
        return errors;
    }

    void SVISSRImageDecoderModule::write_MANAM(time_t unix_timestamp, std::vector<uint8_t> manam_data)
    {
        std::string manam_directory = d_output_file_hint.substr(0, d_output_file_hint.rfind('/')) + "/MANAM";
        if (!std::filesystem::exists(manam_directory)) std::filesystem::create_directory(manam_directory);

        const time_t timevalue = unix_timestamp;
        std::tm timeReadable = *gmtime(&timevalue);
        std::string timestamp = std::to_string(timeReadable.tm_year + 1900) + "-" + (timeReadable.tm_mon + 1 > 9 ? std::to_string(timeReadable.tm_mon + 1) : "0" + std::to_string(timeReadable.tm_mon + 1)) + "-" + (timeReadable.tm_mday > 9 ? std::to_string(timeReadable.tm_mday) : "0" + std::to_string(timeReadable.tm_mday)) + "_" + (timeReadable.tm_hour > 9 ? std::to_string(timeReadable.tm_hour) : "0" + std::to_string(timeReadable.tm_hour)) + "-" + (timeReadable.tm_min > 9 ? std::to_string(timeReadable.tm_min) : "0" + std::to_string(timeReadable.tm_min));

        std::string manam_path = manam_directory + "/MANAM_" + timestamp + ".txt";
        logger->info("Saving MANAM to " + manam_path + "...");
        std::ofstream outfile(manam_path, std::ios::out | std::ios::binary);
        outfile.write(reinterpret_cast<const char *>(&manam_data[0]), 10250);
        outfile.close();
    }

    void SVISSRImageDecoderModule::write_telemetry(time_t unix_timestamp, std::string telemetry_jsonc)
    {
        std::string telemetry_directory = d_output_file_hint.substr(0, d_output_file_hint.rfind('/')) + "/Telemetry";
        if (!std::filesystem::exists(telemetry_directory)) std::filesystem::create_directory(telemetry_directory);

        const time_t timevalue = unix_timestamp;
        std::tm timeReadable = *gmtime(&timevalue);
        std::string timestamp = std::to_string(timeReadable.tm_year + 1900) + "-" + (timeReadable.tm_mon + 1 > 9 ? std::to_string(timeReadable.tm_mon + 1) : "0" + std::to_string(timeReadable.tm_mon + 1)) + "-" + (timeReadable.tm_mday > 9 ? std::to_string(timeReadable.tm_mday) : "0" + std::to_string(timeReadable.tm_mday)) + "_" + (timeReadable.tm_hour > 9 ? std::to_string(timeReadable.tm_hour) : "0" + std::to_string(timeReadable.tm_hour)) + "-" + (timeReadable.tm_min > 9 ? std::to_string(timeReadable.tm_min) : "0" + std::to_string(timeReadable.tm_min));

        std::string telemetry_path = telemetry_directory + "/Telemetry_" + timestamp + ".jsonc";
        logger->info("Saving telemetry to " + telemetry_path + "...");
        std::ofstream outfile(telemetry_path, std::ios::out | std::ios::binary);
        outfile.write(telemetry_jsonc.c_str(), telemetry_jsonc.size());
        outfile.close();
    }

    void SVISSRImageDecoderModule::save_subcom_frame()
    {
        if (!group_retransmissions.empty() && current_subcom_frame.size() / SUBCOM_GROUP_SIZE == 24) {
            Group last_group = satdump::majority_law_vec(group_retransmissions);
            group_retransmissions.clear();
            current_subcom_frame.insert(current_subcom_frame.end(), last_group.begin(), last_group.end());
            logger->debug("Saved a subcom frame!");
            subcommunication_frames.push_back(current_subcom_frame);
            current_subcom_frame.clear();
        } else if (group_retransmissions.empty()) {
            current_subcom_frame.clear();
        } else {
            current_subcom_frame.clear();
        }
    }

    void SVISSRImageDecoderModule::writeImages(SVISSRBuffer &buffer)
    {
        writingImage = true;
        double unix_timestamp = 0;

        switch (buffer.scid) {
            case (40): sat_name = "FY-2H"; break;
            case (39): sat_name = "FY-2G"; break;
            case (38): sat_name = "FY-2F"; break;
            case (37): sat_name = "FY-2E"; break;
            case (36): sat_name = "FY-2D"; break;
            case (35): sat_name = "FY-2C"; break;
            default:   sat_name = "Unknown_FY-2"; break;
        }

        MinorFrame final_subcom_frame;
        bool process_subcom_data = false;

        if (!subcommunication_frames.empty()) {
            final_subcom_frame = satdump::majority_law_vec(subcommunication_frames);
            int errors = get_spare_errors(final_subcom_frame);
            logger->debug("Final subcom data errors: %d/35800", errors);
            if (errors < 32) process_subcom_data = true;
        }

        if (!process_subcom_data) {
            unix_timestamp = time(0);
        } else {
            std::vector<uint8_t> orbit_attitude_data_unstructured = get_subcom_block(final_subcom_frame, Orbit_and_attitude);
            OrbitAndAttitudeData *orbit_attitude_block = reinterpret_cast<OrbitAndAttitudeData *>(orbit_attitude_data_unstructured.data());

            AttitudePredictionSubBlock attitude_prediction_data;
            satdump::majority_law(orbit_attitude_block->ATTITUDE_PREDICTION_SUBBLOCKS, reinterpret_cast<uint8_t *>(&attitude_prediction_data));

            OrbitPredictionSubBlock orbit_prediction_data;
            satdump::majority_law(orbit_attitude_block->ORBIT_PREDICTION_SUBBLOCKS, reinterpret_cast<uint8_t *>(&orbit_prediction_data));

            unix_timestamp = ((orbit_attitude_block->IMAGE_START_TIME * 1e-8) - 40587) * 86400;

            write_MANAM(unix_timestamp, get_subcom_block(final_subcom_frame, MANAM));

            if (save_telemetry) {
                std::vector<uint8_t> Calib_2_block = get_subcom_block(final_subcom_frame, Calibration_2);
                std::string active_sensor = (Calib_2_block[10] & 0b1) == 1 ? "Primary" : "Backup";
                uint8_t active_detectors = satdump::majority_law_vec(detector_telemetry_stats)[0];
                std::string telemetry_data = SVISSRTelemetryDumper::dump_telemetry_to_JSON(*orbit_attitude_block, attitude_prediction_data, orbit_prediction_data, active_sensor, active_detectors);
                write_telemetry(unix_timestamp, telemetry_data);
            }
        }

        if (unix_timestamp < 946681200 || unix_timestamp > 2524604400) unix_timestamp = time(0);
        const time_t timevalue = unix_timestamp;
        std::tm timeReadable = *gmtime(&timevalue);
        
        std::string timestamp = std::to_string(timeReadable.tm_year + 1900) + "-" + (timeReadable.tm_mon + 1 > 9 ? std::to_string(timeReadable.tm_mon + 1) : "0" + std::to_string(timeReadable.tm_mon + 1)) + "-" + (timeReadable.tm_mday > 9 ? std::to_string(timeReadable.tm_mday) : "0" + std::to_string(timeReadable.tm_mday)) + "_" + (timeReadable.tm_hour > 9 ? std::to_string(timeReadable.tm_hour) : "0" + std::to_string(timeReadable.tm_hour)) + "-" + (timeReadable.tm_min > 9 ? std::to_string(timeReadable.tm_min) : "0" + std::to_string(timeReadable.tm_min));

        logger->info("Full disk finished, saving at " + timestamp + "...");
        std::filesystem::create_directory(buffer.directory + "/" + timestamp);
        std::string disk_folder = buffer.directory + "/" + timestamp;

        image::save_img(buffer.image1, std::string(disk_folder + "/" + getSvissrFilename(&timeReadable, "1")).c_str()); // VIS
        image::save_img(buffer.image2, std::string(disk_folder + "/" + getSvissrFilename(&timeReadable, "2")).c_str()); // IR4
        image::save_img(buffer.image3, std::string(disk_folder + "/" + getSvissrFilename(&timeReadable, "3")).c_str()); // IR3
        image::save_img(buffer.image4, std::string(disk_folder + "/" + getSvissrFilename(&timeReadable, "4")).c_str()); // IR1
        image::save_img(buffer.image5, std::string(disk_folder + "/" + getSvissrFilename(&timeReadable, "5")).c_str()); // IR2

        if (resources::resourceExists("fy2/svissr/lut.png")) {
            image::Image channel1(8, buffer.image1.width(), buffer.image1.height(), 1);
            for (size_t i = 0; i < channel1.width() * channel1.height(); i++) channel1.set(i, buffer.image1.get(i) / 255);
            image::Image channel4(8, buffer.image4.width(), buffer.image4.height(), 1);
            for (size_t i = 0; i < channel4.width() * channel4.height(); i++) channel4.set(i, buffer.image4.get(i) / 255);
            
            channel4.resize(channel1.width(), channel1.height());
            image::Image lutImage;
            image::load_png(lutImage, resources::getResourcePath("fy2/svissr/lut.png").c_str());
            lutImage = lutImage.to8bits();
            lutImage.resize(256, 256);
            image::Image compoImage(8, channel1.width(), channel1.height(), 3);
            
            for (size_t i = 0; i < channel1.width() * channel1.height(); i++) {
                uint8_t x = 255 - channel1.get(i);
                uint8_t y = channel4.get(i);
                for (int c = 0; c < 3; c++) compoImage.set(c * compoImage.width() * compoImage.height() + i, lutImage.get(c * lutImage.width() * lutImage.height() + x * lutImage.width() + y));
            }
            image::save_img(compoImage, std::string(disk_folder + "/" + getSvissrFilename(&timeReadable, "FC")).c_str());
        }

        buffer.image1.clear(); buffer.image2.clear(); buffer.image3.clear(); buffer.image4.clear(); buffer.image5.clear();
        subcommunication_frames.clear(); current_subcom_frame.clear(); group_retransmissions.clear(); detector_telemetry_stats.clear();
        writingImage = false;
    }

    void SVISSRImageDecoderModule::process()
    {
        if (input_data_type == DATA_FILE) filesize = getFilesize(d_input_file);
        else filesize = 0;
        if (input_data_type == DATA_FILE) data_in = std::ifstream(d_input_file, std::ios::binary);

        std::string directory = d_output_file_hint.substr(0, d_output_file_hint.rfind('/')) + "/IMAGE";
        if (!std::filesystem::exists(directory) && !no_save_img) std::filesystem::create_directory(directory);

        time_t lastTime = 0;
        uint8_t last_status[20]; memset(last_status, 0, 20);
        size_t last_group_id = 0;
        valid_lines = 0;

        bool is_live = input_data_type != DATA_FILE;
        if (is_live && !no_save_img) { // 预览模式不需要起线程保存
            images_thread_should_run = true;
            images_queue_thread = std::thread(&SVISSRImageDecoderModule::image_saving_thread_f, this);
        }

        while (input_data_type == DATA_FILE ? !data_in.eof() : input_active.load())
        {
            if (input_data_type == DATA_FILE) data_in.read((char *)frame, FRAME_SIZE);
            else input_fifo->read((uint8_t *)frame, FRAME_SIZE);

            // 【新增】将原始帧写入 .svissr 文件
            if (no_save_img && output_svissr.is_open()) {
                output_svissr.write((char *)frame, FRAME_SIZE);
            }

            int counter = (frame[67] << 8 | frame[68]) & 0x0fff;

            if (apply_correction) {
                if (!counter_locked) {
                    if (counter == global_counter + 1 && !backwardScan) counter_locked = true;
                    else global_counter = counter;
                }
                if (counter_locked) {
                    counter = global_counter + 1;
                    global_counter = counter;
                    frame[67] = (counter >> 8) & 0xFF;
                    frame[68] = counter & 0xFF;
                }
            }

            size_t group_id = frame[193] & 0x1f;

            if (group_id != last_group_id) {
                if ((group_id == 0 && last_group_id == 24) || (current_subcom_frame.size() / SUBCOM_GROUP_SIZE) == 24) save_subcom_frame();
                else if (group_retransmissions.size() < 9 && (current_subcom_frame.size() / SUBCOM_GROUP_SIZE) == group_id - 1) {
                    if (!group_retransmissions.empty()) {
                        Group group = satdump::majority_law_vec(group_retransmissions);
                        current_subcom_frame.insert(current_subcom_frame.end(), group.begin(), group.end());
                        group_retransmissions.clear();
                        Group current_retransmission(frame + SUBCOM_START_OFFSET, frame + SUBCOM_START_OFFSET + SUBCOM_GROUP_SIZE);
                        group_retransmissions.push_back(current_retransmission);
                    }
                }
            } else {
                if ((current_subcom_frame.size() / SUBCOM_GROUP_SIZE) == group_id) {
                    if (group_id < 25) {
                        Group current_retransmission(frame + SUBCOM_START_OFFSET, frame + SUBCOM_START_OFFSET + SUBCOM_GROUP_SIZE);
                        group_retransmissions.push_back(current_retransmission);
                    }
                } else group_retransmissions.clear();
            }

            if (current_subcom_frame.size() > SUBCOM_GROUP_SIZE * 24) current_subcom_frame.clear();
            if (group_retransmissions.size() > 8) group_retransmissions.clear();
            last_group_id = group_id;

            if (save_telemetry) {
                std::vector<uint8_t> detector_status; detector_status.push_back(frame[69]);
                detector_telemetry_stats.push_back(detector_status);
            }

            int scid = frame[91];
            scid_stats.push_back(scid);

            if (counter > 2500) { counter_locked = false; continue; }

            uint32_t sun_pulse_angle = frame[91] & 0xFFFFFF;
            sat_is_eclipsed = (sun_pulse_angle == 0xFFFFFF);
            backwardScan = (frame[3] & 0b11) == 0;

            memmove(last_status, &last_status[1], 19);
            last_status[19] = backwardScan;
            uint8_t backward_scanning = satdump::most_common(&last_status[0], &last_status[20], 0);

            if (backward_scanning && valid_lines < 3) counter_locked = false;

            if (backward_scanning && valid_lines > 40) {
                logger->info("Full disk end detected!");
                counter_locked = false;
                
                if (no_save_img) {
                    // 【新增】纯预览模式：清空图像不保存
                    logger->info("No Save Img is enabled. Clearing image buffers.");
                    vissrImageReader.reset();
                    valid_lines = 0;
                    continue;
                }

                std::shared_ptr<SVISSRBuffer> buffer = std::make_shared<SVISSRBuffer>();

                buffer->image1 = vissrImageReader.getImageVIS();
                buffer->image2 = vissrImageReader.getImageIR4();
                buffer->image3 = vissrImageReader.getImageIR3();
                buffer->image4 = vissrImageReader.getImageIR1();
                buffer->image5 = vissrImageReader.getImageIR2();
                buffer->scid = satdump::most_common(scid_stats.begin(), scid_stats.end(), 0);
                scid_stats.clear();
                buffer->directory = directory;

                if (is_live) { images_queue_mtx.lock(); images_queue.push_back(buffer); images_queue_mtx.unlock(); }
                else writeImages(*buffer);

                vissrImageReader.reset();
                valid_lines = 0;
            }

            if (backwardScan) continue;

            // 【修改】如果是 No Save Img，跳过可见光解码
            vissrImageReader.pushFrame(frame, !no_save_img); 
            valid_lines++;
            approx_progess = round(((float)counter / 2500.0f) * 1000.0f) / 10.0f;

            if (input_data_type == DATA_FILE) progress = data_in.tellg();
            module_stats["full_disk_progress"] = approx_progess;

            if (time(NULL) % 10 == 0 && lastTime != time(NULL)) {
                lastTime = time(NULL);
                logger->info("Progress " + std::to_string(round(((double)progress / (double)filesize) * 1000.0) / 10.0) + "%%, Full Disk Progress : " + std::to_string(round(((float)approx_progess / 100.0f) * 1000.0f) / 10.0f) + "%%");
            }
        }

        if (input_data_type == DATA_FILE) data_in.close();

        if (valid_lines > 2 && !no_save_img) {
            logger->info("Full disk end detected!");
            std::shared_ptr<SVISSRBuffer> buffer = std::make_shared<SVISSRBuffer>();
            buffer->image1 = vissrImageReader.getImageVIS();
            buffer->image2 = vissrImageReader.getImageIR4();
            buffer->image3 = vissrImageReader.getImageIR3();
            buffer->image4 = vissrImageReader.getImageIR1();
            buffer->image5 = vissrImageReader.getImageIR2();
            buffer->scid = satdump::most_common(scid_stats.begin(), scid_stats.end(), 0);
            scid_stats.clear();
            buffer->directory = directory;

            if (is_live) {
                images_queue_mtx.lock(); images_queue.push_back(buffer); images_queue_mtx.unlock();
                images_thread_should_run = false;
                if (images_queue_thread.joinable()) images_queue_thread.join();
            } else writeImages(*buffer);
        }
    }

    SVISSRImageDecoderModule::SVISSRImageDecoderModule(std::string input_file, std::string output_file_hint, nlohmann::json parameters) : ProcessingModule(input_file, output_file_hint, parameters) {
        frame = new uint8_t[FRAME_SIZE];
        counter_locked = false; global_counter = 0;
        apply_correction = parameters.contains("apply_correction") ? parameters["apply_correction"].get<bool>() : false;
        save_telemetry = parameters.contains("save_telemetry") ? parameters["save_telemetry"].get<bool>() : false;
        
        // 【新增】读取 No Save Img 和 默认通道
        no_save_img = parameters.contains("no_save_img") ? parameters["no_save_img"].get<bool>() : false;
        preview_ch = 2; 

        backwardScan = false;
        vissrImageReader.reset();

        // 【新增】如果是纯预览模式，直接打开 .svissr 文件流接收数据
        if (no_save_img) {
            output_svissr = std::ofstream(d_output_file_hint + ".svissr", std::ios::binary);
            d_output_files.push_back(d_output_file_hint + ".svissr");
            logger->info("No Save Img mode enabled, .svissr data will be saved to " + d_output_file_hint + ".svissr");
        }
    }

    SVISSRImageDecoderModule::~SVISSRImageDecoderModule() { 
        delete[] frame; 
        if (textureID != 0) delete[] textureBuffer; 
        if (images_queue_thread.joinable()) images_queue_thread.join(); 
        // 【新增】关闭文件
        if (output_svissr.is_open()) output_svissr.close();
    }

    std::vector<ModuleDataType> SVISSRImageDecoderModule::getInputTypes() { return {DATA_FILE, DATA_STREAM}; }
    std::vector<ModuleDataType> SVISSRImageDecoderModule::getOutputTypes() { return {DATA_FILE}; }

    void SVISSRImageDecoderModule::drawUI(bool window) {
        if (textureID == 0) { textureID = makeImageTexture(); textureBuffer = new uint32_t[2501 * 2291]; memset(textureBuffer, 0, sizeof(uint32_t) * 2501 * 2291); }
        ImGui::Begin("S-VISSR Image Decoder", NULL, window ? 0 : NOWINDOW_FLAGS);
        
        ImGui::BeginGroup();
        // 【新增】根据切换的通道显示画面 (Ch2 对应 IR4, Ch4 对应 IR1)
        if (preview_ch == 2) {
            ushort_to_rgba(vissrImageReader.imageBufferIR4, textureBuffer, 2501 * 2291);
            ImGui::Text("Preview: IR Ch2 (3.75 um)");
        } else {
            ushort_to_rgba(vissrImageReader.imageBufferIR1, textureBuffer, 2501 * 2291);
            ImGui::Text("Preview: IR Ch4 (10.8 um)");
        }
        
        updateImageTexture(textureID, textureBuffer, 2291, 2501);
        ImGui::Image((void *)(intptr_t)textureID, {200 * ui_scale, 200 * ui_scale});
        ImGui::EndGroup();
        
        ImGui::SameLine();
        ImGui::BeginGroup();
        
        // 【新增】通道切换按钮
        if (ImGui::Button("Switch to Ch2", {120 * ui_scale, 20 * ui_scale})) preview_ch = 2;
        if (ImGui::Button("Switch to Ch4", {120 * ui_scale, 20 * ui_scale})) preview_ch = 4;
        
        if (apply_correction) {
            ImGui::Button("Correction", {120 * ui_scale, 20 * ui_scale});
            ImGui::Text("Status: "); ImGui::SameLine();
            if (global_counter == 0) ImGui::TextColored(style::theme.red, "WAITING");
            else if (global_counter != 0 && !counter_locked) ImGui::TextColored(style::theme.orange, "LOCKING");
            else ImGui::TextColored(style::theme.green, "LOCKED");
        }
        ImGui::Button("Full Disk Progress", {120 * ui_scale, 20 * ui_scale});
        ImGui::ProgressBar((float)approx_progess / 100.0f, ImVec2(120 * ui_scale, 20 * ui_scale));
        ImGui::Text("State : "); ImGui::SameLine();
        if (sat_is_eclipsed) ImGui::TextColored(style::theme.red, "Eclipsed...");
        else if (writingImage) ImGui::TextColored(style::theme.green, "Writing...");
        else if (backwardScan) ImGui::TextColored(style::theme.red, "Rollback...");
        else ImGui::TextColored(style::theme.orange, "Receiving...");
        ImGui::EndGroup();
        
        if (!streamingInput) ImGui::ProgressBar((double)progress / (double)filesize, ImVec2(ImGui::GetContentRegionAvail().x, 20 * ui_scale));
        ImGui::End();
    }

    std::string SVISSRImageDecoderModule::getID() { return "fengyun_svissr_image_decoder"; }
    std::vector<std::string> SVISSRImageDecoderModule::getParameters() { return {"satname", "apply_correction", "save_telemetry", "no_save_img"}; }
    std::shared_ptr<ProcessingModule> SVISSRImageDecoderModule::getInstance(std::string input_file, std::string output_file_hint, nlohmann::json parameters) {
        return std::make_shared<SVISSRImageDecoderModule>(input_file, output_file_hint, parameters);
    }
}
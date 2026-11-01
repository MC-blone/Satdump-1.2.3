#pragma once

#include "core/module.h"
#include <complex>
#include <fstream>
#include "image/svissr_reader.h"
#include <thread>
#include <map>

namespace fengyun_svissr
{
    enum SVISSRSubCommunicationBlockType
    {
        Simplified_mapping,
        Orbit_and_attitude,
        MANAM,
        Calibration_1,
        Calibration_2,
        Spare
    };

    struct SVISSRSubcommunicationBlock
    {
        int start_offset;
        int end_offset;
    };

    typedef std::vector<uint8_t> Group;
    typedef std::vector<uint8_t> MinorFrame;

    class SVISSRImageDecoderModule : public ProcessingModule
    {
    protected:
        // Settings
        std::string sat_name;

        // Read buffer
        uint8_t *frame;

        std::ifstream data_in;
        std::atomic<uint64_t> filesize;
        std::atomic<uint64_t> progress;

        // Utils values
        bool backwardScan;
        bool writingImage = false;
        int valid_lines;
        float approx_progess;
        
        bool apply_correction;
        bool save_telemetry;
        bool no_save_img;      
        int preview_ch;        
        std::ofstream output_svissr; 
        int global_counter;
        bool counter_locked = false;

        struct SVISSRBuffer
        {
            int scid;
            double timestamp;
            image::Image image1;
            image::Image image2;
            image::Image image3;
            image::Image image4;
            image::Image image5;
            std::string directory;
        };

        // Image readers
        SVISSRReader vissrImageReader;

        // Saving is multithreaded
        std::mutex images_queue_mtx;
        std::vector<std::shared_ptr<SVISSRBuffer>> images_queue;
        std::thread images_queue_thread;

        bool images_thread_should_run;
        void image_saving_thread_f()
        {
            while (images_thread_should_run || images_queue.size() > 0)
            {
                images_queue_mtx.lock();
                bool has_images = images_queue.size() > 0;
                images_queue_mtx.unlock();

                if (has_images)
                {
                    images_queue_mtx.lock();
                    auto img = images_queue[0];
                    images_queue.erase(images_queue.begin());
                    images_queue_mtx.unlock();
                    writeImages(*img);
                    continue;
                }
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }

        std::string getSvissrFilename(std::tm *timeReadable, std::string channel);
        void writeImages(SVISSRBuffer &buffer);

        // Stats
        std::vector<int> scid_stats;
        std::vector<std::vector<uint8_t>> detector_telemetry_stats;

        // Subcommunication block handling
        void save_subcom_frame();
        std::vector<MinorFrame> subcommunication_frames;
        MinorFrame current_subcom_frame;
        std::vector<Group> group_retransmissions;
        void write_MANAM(time_t unix_timestamp, std::vector<uint8_t> manam_data);
        void write_telemetry(time_t unix_timestamp, std::string telemetry_jsonc);

        // UI Stuff
        bool sat_is_eclipsed = false;
        unsigned int textureID = 0;
        uint32_t *textureBuffer;

    public:
        SVISSRImageDecoderModule(std::string input_file, std::string output_file_hint, nlohmann::json parameters);
        ~SVISSRImageDecoderModule();
        void process();
        void drawUI(bool window);
        std::vector<ModuleDataType> getInputTypes();
        std::vector<ModuleDataType> getOutputTypes();

    public:
        static std::string getID();
        virtual std::string getIDM() { return getID(); };
        static std::vector<std::string> getParameters();
        static std::shared_ptr<ProcessingModule> getInstance(std::string input_file, std::string output_file_hint, nlohmann::json parameters);
    };
}
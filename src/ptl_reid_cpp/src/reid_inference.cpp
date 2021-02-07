#include "ptl_reid_cpp/reid_inference.h"
#include <ros/package.h>

namespace ptl
{
    namespace reid
    {
        void ReidInference::init()
        {
            std::string root_path = ros::package::getPath("roslib");
            std::string engine_path = root_path + "/" + reid_param_.engine_file_name;
            std::string onnx_path = root_path + "/" + reid_param_.onnx_file_name;
            if (!load_engine(engine_path))
            {
                if (!parse_onnx_model(onnx_path))
                {
                    std::cout << "Fail to load the model. Kill the node!" << std::endl;
                    std::abort();
                }
                save_engine(engine_path);
            }
        }

        std::vector<float> ReidInference::do_inference_real_time(const cv::Mat &image, const std::vector<cv::Rect2d> &bboxes)
        {
            //create buffer and allocate memory in gpu
            std::vector<void *> buffers(engine->getNbBindings()); // buffers for input and output data
            cudaMalloc(&buffers[0], getSizeByDim(engine->getBindingDimensions(0)) * reid_param_.inference_real_time_batch_size * sizeof(float));
            cudaMalloc(&buffers[1], getSizeByDim(engine->getBindingDimensions(1)) * reid_param_.inference_real_time_batch_size * sizeof(float));

            data_preprocesss(image, bboxes, (float *)buffers[0]);
            inference(buffers, true);
            std::vector<float> result;
            result_postprocess((float *)buffers[1], result);
            return result;
        }

        std::vector<float> ReidInference::do_inference_offline(const std::vector<cv::Mat> &images)
        {
            std::vector<void *> buffers(engine->getNbBindings()); // buffers for input and output data
            cudaMalloc(&buffers[0], getSizeByDim(engine->getBindingDimensions(0)) * reid_param_.inference_real_time_batch_size * sizeof(float));
            cudaMalloc(&buffers[1], getSizeByDim(engine->getBindingDimensions(1)) * reid_param_.inference_real_time_batch_size * sizeof(float));

            data_preprocesss(images, (float *)buffers[0]);
            inference(buffers, false);
            std::vector<float> result;
            result_postprocess((float *)buffers[1], result);
            return result;
        }

        bool ReidInference::load_engine(const std::string &engine_path)
        {
            std::ifstream engine_file(engine_path, std::ios::binary);
            if (!engine_file)
            {
                std::cout << "Error loading engine file: " << engine_path << std::endl;
                return false;
            }

            engine_file.seekg(0, engine_file.end);
            long int fsize = engine_file.tellg();
            engine_file.seekg(0, engine_file.beg);

            std::vector<char> engine_data(fsize);
            engine_file.read(engine_data.data(), fsize);
            if (!engine_file)
            {
                std::cout << "Error loading engine file: " << engine_path << std::endl;
                return false;
            }
            TRTUniquePtr<nvinfer1::IRuntime> runtime{nvinfer1::createInferRuntime(gLogger)};
            engine.reset(runtime->deserializeCudaEngine(engine_data.data(), fsize, nullptr));
            engine_file.close();

            context_real_time.reset(engine->createExecutionContext());
            context_real_time->setOptimizationProfileAsync(0, 0);
            context_real_time->setBindingDimensions(0, nvinfer1::Dims4(reid_param_.inference_real_time_batch_size, 3, 256, 128));

            context_offline.reset(engine->createExecutionContext());
            context_offline->setOptimizationProfileAsync(1, 0);
            context_offline->setBindingDimensions(0, nvinfer1::Dims4(reid_param_.inference_offline_batch_size, 3, 256, 128));
            std::cout << "Load engine file successfully!" << std::endl;

            return true;
        }

        // initialize TensorRT engine and parse ONNX model --------------------------------------------------------------------
        bool ReidInference::parse_onnx_model(const std::string &model_path)
        {
            TRTUniquePtr<nvinfer1::IBuilder> builder{nvinfer1::createInferBuilder(gLogger)};
            const auto explicit_batch = 1U << static_cast<uint32_t>(nvinfer1::NetworkDefinitionCreationFlag::kEXPLICIT_BATCH);
            TRTUniquePtr<nvinfer1::INetworkDefinition> network{builder->createNetworkV2(explicit_batch)};
            TRTUniquePtr<nvonnxparser::IParser> parser{nvonnxparser::createParser(*network, gLogger)};
            TRTUniquePtr<nvinfer1::IBuilderConfig> config{builder->createBuilderConfig()};
            // parse ONNX
            if (!parser->parseFromFile(model_path.c_str(), static_cast<int>(nvinfer1::ILogger::Severity::kINFO)))
            {
                std::cerr << "ERROR: could not parse the model." << std::endl;
                return;
            }

            // allow TensorRT to use up to 512Mb of GPU memory for tactic selection.
            config->setMaxWorkspaceSize(1ULL << 29);
            // use FP16 mode if possible
            if (builder->platformHasFastFp16())
            {
                config->setFlag(nvinfer1::BuilderFlag::kFP16);
            }

            auto profile0 = builder->createOptimizationProfile();
            profile0->setDimensions("batched_inputs", nvinfer1::OptProfileSelector::kMIN, nvinfer1::Dims4(1, 3, 256, 128)); //TODO hard code in here
            profile0->setDimensions("batched_inputs", nvinfer1::OptProfileSelector::kOPT, nvinfer1::Dims4(reid_param_.inference_real_time_batch_size, 3, 256, 128));
            profile0->setDimensions("batched_inputs", nvinfer1::OptProfileSelector::kMAX, nvinfer1::Dims4(reid_param_.inference_real_time_batch_size, 3, 256, 128));

            auto profile1 = builder->createOptimizationProfile();
            profile1->setDimensions("batched_inputs", nvinfer1::OptProfileSelector::kMIN, nvinfer1::Dims4(1, 3, 256, 128));
            profile1->setDimensions("batched_inputs", nvinfer1::OptProfileSelector::kOPT, nvinfer1::Dims4(reid_param_.inference_offline_batch_size, 3, 256, 128));
            profile1->setDimensions("batched_inputs", nvinfer1::OptProfileSelector::kMAX, nvinfer1::Dims4(reid_param_.inference_offline_batch_size, 3, 256, 128));

            config->addOptimizationProfile(profile0);
            config->addOptimizationProfile(profile1);
            // generate TensorRT engine optimized for the target platform
            engine.reset(builder->buildEngineWithConfig(*network, *config));
            context_real_time.reset(engine->createExecutionContext());
            context_offline.reset(engine->createExecutionContext());
        }

        void ReidInference::save_engine(const std::string &engine_path)
        {
            std::ofstream engine_file(engine_path, std::ios::binary);
            if (!engine_file)
            {
                std::cerr << "Cannot open engine file: " << engine_path << std::endl;
            }

            TRTUniquePtr<nvinfer1::IHostMemory> serialized_engine{engine->serialize()};
            if (serialized_engine == nullptr)
            {
                std::cerr << "Engine serialization failed!" << std::endl;
            }

            engine_file.write(static_cast<char *>(serialized_engine->data()), serialized_engine->size());
            engine_file.close();
            std::cout << "Save engine successfully!" << std::endl;
        }

        void ReidInference::data_preprocesss(const cv::Mat &image, const std::vector<cv::Rect2d> &bboxes, float *gpu_input)
        {
            const int input_width = 128;
            const int input_height = 256;
            const int channels = 3;

            cv::cuda::GpuMat gpu_frame;
            cv::cuda::GpuMat resized;
            cv::cuda::GpuMat flt_image;

            int image_num = 0;
            for (auto bbox : bboxes)
            {
                // upload image to GPU
                gpu_frame.upload(image(bbox));
                auto input_size = cv::Size(input_width, input_height);
                // resize

                cv::cuda::resize(gpu_frame, resized, input_size, 0, 0, cv::INTER_NEAREST);
                // normalize
                resized.convertTo(flt_image, CV_32FC3, 1.f / 255.f);

                //image in is bgr
                //image net mean(rgb)：0.485，0.456，0.406
                //image net mean(std)：0.229，0.224，0.225
                cv::cuda::subtract(flt_image, cv::Scalar(0.485f, 0.456f, 0.406f), flt_image, cv::noArray(), -1);
                cv::cuda::divide(flt_image, cv::Scalar(0.229f, 0.224f, 0.225f), flt_image, 1, -1);

                // to tensor
                std::vector<cv::cuda::GpuMat> chw;
                for (size_t i = 0; i < channels; ++i)
                {
                    chw.emplace_back(cv::cuda::GpuMat(input_size, CV_32FC1, gpu_input + (i + image_num * 3) * input_width * input_height));
                }
                cv::cuda::split(flt_image, chw);
                image_num++;
            }
        }

        void ReidInference::data_preprocesss(const std::vector<cv::Mat> &images, float *gpu_input)
        {
            const int input_width = 128;
            const int input_height = 256;
            const int channels = 3;

            cv::cuda::GpuMat gpu_frame;
            cv::cuda::GpuMat resized;
            cv::cuda::GpuMat flt_image;

            int image_num = 0;
            for (auto img : images)
            {
                // upload image to GPU
                gpu_frame.upload(img);
                auto input_size = cv::Size(input_width, input_height);
                // resize

                cv::cuda::resize(gpu_frame, resized, input_size, 0, 0, cv::INTER_NEAREST);
                // normalize
                resized.convertTo(flt_image, CV_32FC3, 1.f / 255.f);

                //image in is bgr
                //image net mean(rgb)：0.485，0.456，0.406
                //image net mean(std)：0.229，0.224，0.225
                cv::cuda::subtract(flt_image, cv::Scalar(0.485f, 0.456f, 0.406f), flt_image, cv::noArray(), -1);
                cv::cuda::divide(flt_image, cv::Scalar(0.229f, 0.224f, 0.225f), flt_image, 1, -1);

                // to tensor
                std::vector<cv::cuda::GpuMat> chw;
                for (size_t i = 0; i < channels; ++i)
                {
                    chw.emplace_back(cv::cuda::GpuMat(input_size, CV_32FC1, gpu_input + (i + image_num * 3) * input_width * input_height));
                }
                cv::cuda::split(flt_image, chw);
                image_num++;
            }
        }

        void ReidInference::inference(std::vector<void *> &buffers, bool is_real_time)
        {
            if (is_real_time)
            {
                context_real_time->enqueueV2(buffers.data(), 0, nullptr);
            }
            else
            {
                context_offline->enqueueV2(buffers.data(), 0, nullptr);
            }
        }

        //TODO unify c array and c++ vector
        void ReidInference::result_postprocess(float *gpu_output, std::vector<float> &cpu_output)
        {
            cudaMemcpyAsync(cpu_output.data(), gpu_output, cpu_output.size() * sizeof(float), cudaMemcpyDeviceToHost);
        }

        // post-processing stage ----------------------------------------------------------------------------------------------
        void postprocessResults(float *gpu_output, const nvinfer1::Dims &dims, int batch_size)
        {
            // copy results from GPU to CPU
            std::vector<float> cpu_output(getSizeByDim(dims) * batch_size);
            cudaMemcpyAsync(cpu_output.data(), gpu_output, cpu_output.size() * sizeof(float), cudaMemcpyDeviceToHost);
        }

    } // namespace reid
} // namespace ptl
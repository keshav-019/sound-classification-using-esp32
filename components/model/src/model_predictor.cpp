/**
* @file model_inference.cpp
* @brief TensorFlow Lite Micro implementation for audio classification
* 
* This file contains the implementation for:
* - Loading a pre-trained TFLite model
* - Setting up the interpreter with required operations
* - Running inference on input MFCC features
* - Returning the predicted class
*/

#include "model.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/schema/schema_generated.h"
#include "esp_heap_caps.h"  // For ESP32-specific memory allocation
#include <cstdio>

#define INPUT_SIZE 1024
#define OUTPUT_SIZE 6
#define TENSOR_ARENA_SIZE (60*1024)

// Use ESP32's aligned memory allocation
static uint8_t* tensor_arena = nullptr;
static tflite::MicroInterpreter* interpreter = nullptr;
static TfLiteTensor* input = nullptr;
static TfLiteTensor* output = nullptr;

extern "C" int predict_class(float* input_data) {
    static bool initialized = false;

    if (!initialized) {
        // Allocate aligned memory using ESP32's memory manager
        tensor_arena = (uint8_t*)heap_caps_aligned_alloc(16, TENSOR_ARENA_SIZE, MALLOC_CAP_8BIT);
        if (tensor_arena == nullptr) {
            printf("Failed to allocate tensor arena\n");
            return -1;
        }

        const tflite::Model* model = tflite::GetModel(mfcc_model_tflite);
        if (model->version() != TFLITE_SCHEMA_VERSION) {
            printf("Model schema mismatch\n");
            heap_caps_free(tensor_arena);
            return -1;
        }

        static tflite::MicroMutableOpResolver<16> resolver;
        resolver.AddConv2D();
        resolver.AddMaxPool2D();
        resolver.AddRelu();
        resolver.AddFullyConnected();
        resolver.AddReshape();
        resolver.AddSoftmax();
        resolver.AddQuantize();
        resolver.AddDequantize();
        resolver.AddExpandDims();
        resolver.AddDepthwiseConv2D();
        resolver.AddShape();
        resolver.AddStridedSlice();
        resolver.AddPack();

        static tflite::MicroInterpreter static_interpreter(
            model, resolver, tensor_arena, TENSOR_ARENA_SIZE);

        interpreter = &static_interpreter;

        if (interpreter->AllocateTensors() != kTfLiteOk) {
            printf("Failed to allocate tensors\n");
            heap_caps_free(tensor_arena);
            return -1;
        }

        input = interpreter->input(0);
        output = interpreter->output(0);

        printf("Input dimensions: ");
        for (int i = 0; i < input->dims->size; ++i) {
            printf("%d ", input->dims->data[i]);
        }
        printf("\n");

        initialized = true;
    }

    // Input processing
    if (input->type == kTfLiteInt8) {
        float input_scale = input->params.scale;
        int32_t input_zero_point = input->params.zero_point;
        int8_t* input_buffer = input->data.int8;

        for (int i = 0; i < INPUT_SIZE; ++i) {
            int32_t quantized = static_cast<int32_t>(round(input_data[i] / input_scale) + input_zero_point);
            quantized = quantized < -128 ? -128 : (quantized > 127 ? 127 : quantized);
            input_buffer[i] = static_cast<int8_t>(quantized);
        }
    }
    else if (input->type == kTfLiteFloat32) {
        float* input_buffer = input->data.f;
        for (int i = 0; i < INPUT_SIZE; ++i) {
            input_buffer[i] = input_data[i];
        }
    }
    else {
        printf("Unsupported input type: %d\n", input->type);
        return -1;
    }

    // Run inference
    if (interpreter->Invoke() != kTfLiteOk) {
        printf("Inference failed\n");
        return -1;
    }

    // Process output
    if (output->type == kTfLiteInt8) {
        int8_t* output_buffer = output->data.int8;
        float output_scale = output->params.scale;
        int32_t output_zero_point = output->params.zero_point;

        printf("Raw outputs: ");
        int max_index = 0;
        float max_value = (output_buffer[0] - output_zero_point) * output_scale;
        
        for (int i = 0; i < OUTPUT_SIZE; ++i) {
            float value = (output_buffer[i] - output_zero_point) * output_scale;
            printf("%f ", value);
            if (value > max_value) {
                max_value = value;
                max_index = i;
            }
        }
        printf("\n");
        return max_index;
    }
    else if (output->type == kTfLiteFloat32) {
        float* output_buffer = output->data.f;
        int max_index = 0;
        for (int i = 1; i < OUTPUT_SIZE; ++i) {
            if (output_buffer[i] > output_buffer[max_index]) {
                max_index = i;
            }
        }
        return max_index;
    }

    printf("Unsupported output type\n");
    return -1;
}
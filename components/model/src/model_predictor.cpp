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
#include <stdio.h>

// Model configuration constants
#define INPUT_SIZE 15      ///< Number of MFCC features per input frame
#define OUTPUT_SIZE 6      ///< Number of output classes
#define TENSOR_ARENA_SIZE (10 * 1024) ///< Memory allocation for TensorFlow tensors

// Static variables for the TensorFlow Lite Micro interpreter
static tflite::MicroInterpreter* interpreter = nullptr; ///< TFLite Micro interpreter instance
static TfLiteTensor* input = nullptr;                  ///< Pointer to input tensor
static TfLiteTensor* output = nullptr;                 ///< Pointer to output tensor
static uint8_t tensor_arena[TENSOR_ARENA_SIZE];        ///< Memory arena for tensor storage

/**
* @brief Runs inference on input MFCC features and returns predicted class
* @param input_data Pointer to array of MFCC features (size INPUT_SIZE)
* @return Predicted class index (0-5), or -1 on error
* 
* @note This function handles:
* 1. One-time initialization of the TFLite interpreter
* 2. Input data copying
* 3. Model inference execution
* 4. Output processing (argmax)
* 
* @example
* float mfcc_features[15] = {...};
* int predicted_class = predict_class(mfcc_features);
*/
extern "C" int predict_class(const float* input_data) {
    static bool initialized = false;

    if (!initialized) {
        // Step 1: Load the model
        const tflite::Model* model = tflite::GetModel(model_tflite);
        if (model == nullptr || model->subgraphs() == nullptr) {
            printf("Invalid model!\n");
            return -1;
        }

        // Step 2: Register operations - REMOVED AddFlatten()
        static tflite::MicroMutableOpResolver<7> resolver;  // Reduced from 8 to 7
        resolver.AddConv2D();
        resolver.AddDepthwiseConv2D();  // For depthwise separable conv
        resolver.AddMaxPool2D();
        resolver.AddRelu();
        resolver.AddSoftmax();
        resolver.AddFullyConnected();
        resolver.AddReshape();         // Flatten operations are often converted to Reshape
        // Keep Quantize/Dequantize if using quantized model

        // Rest of the initialization remains the same
        static tflite::MicroInterpreter static_interpreter(
            model,
            resolver,
            tensor_arena,
            TENSOR_ARENA_SIZE
        );

        interpreter = &static_interpreter;

        if (interpreter->AllocateTensors() != kTfLiteOk) {
            printf("Failed to allocate tensors!\n");
            return -1;
        }

        input = interpreter->input(0);
        output = interpreter->output(0);
        initialized = true;
        printf("Model initialized successfully\n");
    }

    // Rest of the function remains unchanged
    for (int i = 0; i < INPUT_SIZE; ++i) {
        input->data.f[i] = input_data[i];
    }

    if (interpreter->Invoke() != kTfLiteOk) {
        printf("Model invoke failed!\n");
        return -1;
    }

    int max_index = 0;
    float max_value = output->data.f[0];
    for (int i = 1; i < OUTPUT_SIZE; ++i) {
        if (output->data.f[i] > max_value) {
            max_value = output->data.f[i];
            max_index = i;
        }
    }

    return max_index;
}
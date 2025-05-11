// /**
// * @file model_inference.cpp
// * @brief TensorFlow Lite Micro implementation for audio classification
// * 
// * This file contains the implementation for:
// * - Loading a pre-trained TFLite model
// * - Setting up the interpreter with required operations
// * - Running inference on input MFCC features
// * - Returning the predicted class
// */

#include "model.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/schema/schema_generated.h"
#include <stdio.h>

// Model configuration constants
#define INPUT_SIZE 1024      ///< Number of MFCC features per input frame
#define OUTPUT_SIZE 6      ///< Number of output classes
#define TENSOR_ARENA_SIZE (60*1024) ///< Memory allocation for TensorFlow tensors

// Static variables for the TensorFlow Lite Micro interpreter
static tflite::MicroInterpreter* interpreter = nullptr; ///< TFLite Micro interpreter instance
static TfLiteTensor* input = nullptr;                  ///< Pointer to input tensor
static TfLiteTensor* output = nullptr;                 ///< Pointer to output tensor
static uint8_t tensor_arena[TENSOR_ARENA_SIZE];        ///< Memory arena for tensor storage
// /**
// * @brief Runs inference on input MFCC features and returns predicted class
// * @param input_data Pointer to array of MFCC features (size INPUT_SIZE)
// * @return Predicted class index (0-5), or -1 on error
// * 
// * @note This function handles:
// * 1. One-time initialization of the TFLite interpreter
// * 2. Input data copying
// * 3. Model inference execution
// * 4. Output processing (argmax)
// * 
// * @example
// * float mfcc_features[15] = {...};
// * int predicted_class = predict_class(mfcc_features);
// */
// extern "C" int predict_class(float* input_data) {
//     static bool initialized = false;

//     // One-time initialization block
//     if (!initialized) {
//         // Step 1: Load the model from embedded array
//         const tflite::Model* model = tflite::GetModel(mfcc_model_tflite);
//         if (model == nullptr || model->subgraphs() == nullptr) {
//             printf("Invalid model!\n");
//             return -1;
//         }

//         // Step 2: Register operations needed by the model
//         static tflite::MicroMutableOpResolver<15> resolver;
//         resolver.AddConv2D();          // Required for convolutional layers
//         resolver.AddMaxPool2D();       // Required for pooling layers
//         resolver.AddRelu();            // Required for ReLU activation
//         resolver.AddSoftmax();         // Required for output layer
//         resolver.AddFullyConnected();   // Required for dense layers
//         resolver.AddReshape();          // Required for tensor reshaping
//         resolver.AddQuantize();         // Required if using quantized models
//         resolver.AddDequantize();       // Required if using quantized models
//         resolver.AddExpandDims();
//         resolver.AddDepthwiseConv2D();
//         resolver.AddShape();
//         resolver.AddStridedSlice();
//         resolver.AddPack();

//         // Step 3: Initialize interpreter with model and resolver
//         static tflite::MicroInterpreter static_interpreter(
//             model,              // The loaded model
//             resolver,           // Operation resolver
//             tensor_arena,       // Pre-allocated memory buffer
//             TENSOR_ARENA_SIZE   // Size of memory buffer
//         );

//         interpreter = &static_interpreter;

//         // Step 4: Allocate tensors in the memory arena
//         if (interpreter->AllocateTensors() != kTfLiteOk) {
//             printf("Failed to allocate tensors!\n");
//             return -1;
//         }

//         // Cache pointers to input and output tensors
//         input = interpreter->input(0);
//         output = interpreter->output(0);
//         initialized = true;
        
//         printf("Model initialized successfully\n");
//     }

//     // Step 1: Convert float32 input to int8 for quantized model
//     float scale = input->params.scale;
//     int zero_point = input->params.zero_point;
//     int8_t* input_buffer = input->data.int8;

//     // Convert each input value from float32 to int8
//     for (int i = 0; i < INPUT_SIZE; ++i) {
//         // Normalize input value (assumed to be in range [-1, 1])
//         float value = input_data[i];
//         int32_t quantized = static_cast<int32_t>(round(value / scale) + zero_point);

//         // Clamp to the range of int8 [-128, 127]
//         if (quantized > 127) quantized = 127;
//         if (quantized < -128) quantized = -128;

//         input_buffer[i] = static_cast<int8_t>(quantized);
//     }
//     // Step 5: Copy input data to model tensor
//     // Note: Assumes input tensor is float32 type with size INPUT_SIZE
//     for (int i = 0; i < INPUT_SIZE; ++i) {
//         input->data.f[i] = input_data[i];
//     }

//     // Step 6: Run inference
//     if (interpreter->Invoke() != kTfLiteOk) {
//         printf("Model invoke failed!\n");
//         return -1;
//     }

//     // Step 3: Convert int8 output to float32 and process it
//     float output_scale = output->params.scale;
//     int output_zero_point = output->params.zero_point;

//     // Step 7: Process output - find class with highest probability
//     int max_index = 0;
//     float max_value = output->data.f[0];
//     for (int i = 1; i < OUTPUT_SIZE; ++i) {
//         if (output->data.f[i] > max_value) {
//             max_value = output->data.f[i];
//             max_index = i;
//         }
//     }

//     return max_index;
// }

extern "C" int predict_class(float* input_data) {
    static bool initialized = false;

    if (!initialized) {
        const tflite::Model* model = tflite::GetModel(mfcc_model_tflite);
        if (model == nullptr || model->subgraphs() == nullptr) {
            printf("Invalid model!\n");
            return -1;
        }

        static tflite::MicroMutableOpResolver<15> resolver;
        resolver.AddConv2D();
        resolver.AddMaxPool2D();
        resolver.AddRelu();
        resolver.AddSoftmax();
        resolver.AddFullyConnected();
        resolver.AddReshape();
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
            printf("Failed to allocate tensors!\n");
            return -1;
        }

        input = interpreter->input(0);
        output = interpreter->output(0);
        initialized = true;

        printf("Model initialized successfully\n");
    }

    // Quantize input from float32 to int8
    // float input_scale = input->params.scale;
    // printf("input scale : %f",&input_scale);
    // int input_zero_point = input->params.zero_point;
    // printf("zero point : %d",)
    float input_scale = 0.003922;
    int input_zero_point = -128;
    int8_t* input_buffer = input->data.int8;

    for (int i = 0; i < INPUT_SIZE; ++i) {
        int32_t quantized = static_cast<int32_t>(round(input_data[i] / input_scale) + input_zero_point);
        if (quantized > 127) quantized = 127;
        if (quantized < -128) quantized = -128;
        input_buffer[i] = static_cast<int8_t>(quantized);
    }
    // taking a look at the quantized data
    printf("Quantized data for inference :\n");
    for(int i=0;i<1024;i++)
    {
        printf("%d ",input_buffer[i]);
    }
    // Run inference
    if (interpreter->Invoke() != kTfLiteOk) {
        printf("Model invoke failed!\n");
        return -1;
    }

    

    // Dequantize output and find the class with the highest probability
    // float output_scale = output->params.scale;
    // int output_zero_point = output->params.zero_point;
    float output_scale = 0.003906;
    int output_zero_point = -128;
    int8_t* output_buffer = output->data.int8;
    printf("output scale : %f",output_scale);
    printf("output zero point : %d",output_zero_point);

    // Print the raw int8 values
for (int i = 0; i < 6; ++i) {
    int8_t output_value = output_buffer[i];
    printf("Raw Output[%d]: %d\n", i, output_value);  // Print the raw int8 value
}

    int max_index = 0;
    float max_value = (output_buffer[0] - output_zero_point) * output_scale;

    for (int i = 1; i < OUTPUT_SIZE; ++i) {
        float value = (output_buffer[i] - output_zero_point) * output_scale;
        if (value > max_value) {
            max_value = value;
            max_index = i;
        }
    }

    return max_index;
}

#include "model.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/schema/schema_generated.h"

#include <stdio.h>

#define INPUT_SIZE 15
#define OUTPUT_SIZE 6
#define TENSOR_ARENA_SIZE (10 * 1024)

static tflite::MicroInterpreter* interpreter = nullptr;
static TfLiteTensor* input = nullptr;
static TfLiteTensor* output = nullptr;
static uint8_t tensor_arena[TENSOR_ARENA_SIZE];

extern "C" int predict_class(const float* input_data) {
    static bool initialized = false;

    if (!initialized) {
        const tflite::Model* model = tflite::GetModel(mfcc_model_tflite);
        if (model == nullptr || model->subgraphs() == nullptr) {
            printf("Invalid model!\n");
            return -1;
        }

        // Declare supported ops
        static tflite::MicroMutableOpResolver<6> resolver;
        resolver.AddConv2D();
        // resolver.AddDepthwiseConv2D();
        resolver.AddMaxPool2D();
        resolver.AddRelu();
        resolver.AddSoftmax();
        resolver.AddFullyConnected();
        resolver.AddReshape();
        resolver.AddQuantize();
        resolver.AddDequantize();

        // Construct interpreter without error reporter
        static tflite::MicroInterpreter static_interpreter(
            model,
            resolver,
            tensor_arena,
            TENSOR_ARENA_SIZE
            // Remaining args use defaults (resource vars, profiler, use_allocated_arena)
        );

        interpreter = &static_interpreter;

        if (interpreter->AllocateTensors() != kTfLiteOk) {
            printf("Failed to allocate tensors!\n");
            return -1;
        }

        input = interpreter->input(0);
        output = interpreter->output(0);
        initialized = true;
    }

    // Copy input data
    for (int i = 0; i < INPUT_SIZE; ++i) {
        input->data.f[i] = input_data[i];
    }

    // Run inference
    if (interpreter->Invoke() != kTfLiteOk) {
        printf("Model invoke failed!\n");
        return -1;
    }

    // Argmax of output
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

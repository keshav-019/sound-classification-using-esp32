#ifndef MODEL_PREDICTOR_H
#define MODEL_PREDICTOR_H

#ifdef __cplusplus
extern "C" {
#endif

int predict_class(const float *input_data);  // Returns class 0-5

#ifdef __cplusplus
}
#endif

#endif // MODEL_PREDICTOR_H

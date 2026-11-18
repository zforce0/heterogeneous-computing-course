#pragma once

#include "libirtemp.h"

#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif

#include "frame.h"
#include "libirprocess.h"

#include <cstdint>

#define NUCT_LEN 8192
#define TEMPERATURE_CORRECT_TABLE_WORDS (4 * 14 * 64 + 128)

struct TempCalInfo_t {
    EnvParam_t* org_env_param;
    EnvParam_t* new_env_param;
    uint8_t gain_flag;
    NucFactor_t* nuc_factor;
    EnvFactor_t* org_env_factor;
    EnvFactor_t* new_env_factor;
    uint16_t* nuc_table;
};

extern TempCalInfo_t temp_cal_info;
extern uint16_t correct_table[TEMPERATURE_CORRECT_TABLE_WORDS];

TempCalInfo_t* get_temp_cal_info();
void print_cali_info(TempCalInfo_t* calibration);

float temp_value_converter(uint16_t temp_value);
int calculate_new_env_cali_parameter(
    uint16_t* correction_table, double emissivity,
    double atmospheric_temperature, double reflected_temperature,
    double distance, double humidity);
int reverse_temp_frame_to_nuc(
    uint16_t* original_temperature, NucFactor_t* factor,
    int pixel_count, uint16_t* nuc_data);
int temp_calc_with_new_env_calibration(
    TempCalInfo_t* calibration, double original_temperature,
    double* new_temperature);
int temp_calc_without_any_correct(
    TempCalInfo_t* calibration, double original_temperature,
    double* new_temperature);

bool point_temperature(
    const TemperatureFrame& frame, int x, int y,
    float& celsius);
bool line_temperature(
    const TemperatureFrame& frame, const Line_t& line,
    TempInfo_t& temperature_info);
bool rect_temperature(
    const TemperatureFrame& frame, const Area_t& rectangle,
    TempInfo_t& temperature_info);
bool full_frame_temperature(
    const TemperatureFrame& frame, TempInfo_t& temperature_info);

void point_temp_demo(uint16_t* temperature_data,
                     TempDataRes_t resolution);
void line_temp_demo(uint16_t* temperature_data,
                    TempDataRes_t resolution);
void rect_temp_demo(uint16_t* temperature_data,
                    TempDataRes_t resolution);

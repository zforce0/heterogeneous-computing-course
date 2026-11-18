#include "temperature.h"

#include <cstdio>

namespace {

EnvParam_t original_environment = {0, 0, 0, 0};
EnvParam_t new_environment = {0, 0, 0, 0};
NucFactor_t nuc_factor = {0, 0, 0};
EnvFactor_t original_environment_factor = {0};
EnvFactor_t new_environment_factor = {0};
uint16_t nuc_table[NUCT_LEN] = {0};

TempDataRes_t resolution_of(const TemperatureFrame& frame)
{
    TempDataRes_t resolution = {
        static_cast<uint16_t>(frame.width),
        static_cast<uint16_t>(frame.height)
    };
    return resolution;
}

} // namespace

uint16_t correct_table[TEMPERATURE_CORRECT_TABLE_WORDS] = {0};

TempCalInfo_t temp_cal_info = {
    &original_environment,
    &new_environment,
    1,
    &nuc_factor,
    &original_environment_factor,
    &new_environment_factor,
    nuc_table
};

TempCalInfo_t* get_temp_cal_info()
{
    return &temp_cal_info;
}

float temp_value_converter(uint16_t temp_value)
{
    return static_cast<float>(
        static_cast<double>(temp_value) / 64.0 - 273.15);
}

int calculate_new_env_cali_parameter(
    uint16_t* correction_table, double emissivity,
    double atmospheric_temperature, double reflected_temperature,
    double distance, double humidity)
{
    if (correction_table == NULL) {
        return -1;
    }

    uint16_t tau = 0;
    if (read_tau(correction_table, humidity,
                 atmospheric_temperature, distance,
                 &tau) != IRTEMP_SUCCESS) {
        std::fprintf(stderr, "read_tau failed\n");
        return -1;
    }

    new_environment.EMS =
        static_cast<int>(emissivity * (1 << 14));
    new_environment.TAU = tau;
    new_environment.Ta = static_cast<int>(
        (atmospheric_temperature + 273.15) * (1 << 4));
    new_environment.Tu = static_cast<int>(
        (reflected_temperature + 273.15) * (1 << 4));

    if (calculate_new_KE_and_BE_with_nuc_t(
            &new_environment, nuc_table,
            temp_cal_info.gain_flag,
            &new_environment_factor) != IRTEMP_SUCCESS) {
        std::fprintf(stderr,
                     "calculate_new_KE_and_BE_with_nuc_t failed\n");
        return -1;
    }
    return 0;
}

int reverse_temp_frame_to_nuc(
    uint16_t* original_temperature, NucFactor_t* factor,
    int pixel_count, uint16_t* nuc_data)
{
    if (original_temperature == NULL || factor == NULL ||
        nuc_data == NULL || pixel_count < 0) {
        return -1;
    }

    for (int i = 0; i < pixel_count; ++i) {
        const double kelvin_temperature =
            original_temperature[i] / 16.0;
        if (reverse_calc_NUC_with_env_correct(
                factor, kelvin_temperature,
                &nuc_data[i]) != IRTEMP_SUCCESS) {
            std::fprintf(stderr,
                         "reverse_calc_NUC_with_env_correct failed\n");
            return -1;
        }
    }
    return 0;
}

int temp_calc_with_new_env_calibration(
    TempCalInfo_t* calibration, double original_temperature,
    double* new_temperature)
{
    if (calibration == NULL || new_temperature == NULL) {
        return -1;
    }

    uint16_t calibrated_nuc = 0;
    uint16_t original_nuc = 0;
    uint16_t temperature_data = 0;
    if (reverse_calc_NUC_with_nuc_t(
            calibration->nuc_table,
            original_temperature - 273.15,
            &calibrated_nuc) != IRTEMP_SUCCESS) {
        std::fprintf(stderr,
                     "reverse_calc_NUC_with_nuc_t failed\n");
        return -1;
    }
    if (reverse_calc_NUC_without_env_correct(
            calibration->org_env_factor, calibrated_nuc,
            &original_nuc) != IRTEMP_SUCCESS) {
        std::fprintf(stderr,
                     "reverse_calc_NUC_without_env_correct failed\n");
        return -1;
    }
    if (recalc_NUC_with_env_correct(
            calibration->new_env_factor, original_nuc,
            &calibrated_nuc) != IRTEMP_SUCCESS) {
        std::fprintf(stderr,
                     "recalc_NUC_with_env_correct failed\n");
        return -1;
    }
    if (remap_temp(calibration->nuc_table, calibrated_nuc,
                   &temperature_data) != IRTEMP_SUCCESS) {
        std::fprintf(stderr, "remap_temp failed\n");
        return -1;
    }

    *new_temperature = temperature_data / 16.0;
    return 0;
}

int temp_calc_without_any_correct(
    TempCalInfo_t* calibration, double original_temperature,
    double* new_temperature)
{
    if (calibration == NULL || new_temperature == NULL) {
        return -1;
    }

    uint16_t calibrated_nuc = 0;
    uint16_t original_nuc = 0;
    uint16_t temperature_data = 0;
    if (reverse_calc_NUC_with_nuc_t(
            calibration->nuc_table,
            original_temperature - 273.15,
            &calibrated_nuc) != IRTEMP_SUCCESS) {
        std::fprintf(stderr,
                     "reverse_calc_NUC_with_nuc_t failed\n");
        return -1;
    }
    if (reverse_calc_NUC_without_env_correct(
            calibration->org_env_factor, calibrated_nuc,
            &original_nuc) != IRTEMP_SUCCESS) {
        std::fprintf(stderr,
                     "reverse_calc_NUC_without_env_correct failed\n");
        return -1;
    }
    if (remap_temp(calibration->nuc_table, original_nuc,
                   &temperature_data) != IRTEMP_SUCCESS) {
        std::fprintf(stderr, "remap_temp failed\n");
        return -1;
    }

    *new_temperature = temperature_data / 16.0;
    return 0;
}

void print_cali_info(TempCalInfo_t* calibration)
{
    if (calibration == NULL) {
        return;
    }
    std::printf("origin ems = %d\n",
                calibration->org_env_param->EMS);
    std::printf("origin tau = %d\n",
                calibration->org_env_param->TAU);
    std::printf("origin ta = %d\n",
                calibration->org_env_param->Ta);
    std::printf("origin tu = %d\n",
                calibration->org_env_param->Tu);
    std::printf("origin K_E = %d\n",
                calibration->org_env_factor->K_E);
    std::printf("origin B_E = %d\n",
                calibration->org_env_factor->B_E);
    std::printf("nuc P0 = %d\n", calibration->nuc_factor->P0);
    std::printf("nuc P1 = %d\n", calibration->nuc_factor->P1);
    std::printf("nuc P2 = %d\n", calibration->nuc_factor->P2);
    std::printf("nuc_table[0] = %u\n",
                calibration->nuc_table[0]);
    std::printf("new ems = %d\n",
                calibration->new_env_param->EMS);
    std::printf("new tau = %d\n",
                calibration->new_env_param->TAU);
    std::printf("new ta = %d\n",
                calibration->new_env_param->Ta);
    std::printf("new tu = %d\n",
                calibration->new_env_param->Tu);
    std::printf("new K_E = %d\n",
                calibration->new_env_factor->K_E);
    std::printf("new B_E = %d\n",
                calibration->new_env_factor->B_E);
}

bool point_temperature(
    const TemperatureFrame& frame, int x, int y,
    float& celsius)
{
    if (!frame.valid() || x < 0 || y < 0 ||
        x >= frame.width || y >= frame.height) {
        return false;
    }

    Dot_t point = {x, y};
    uint16_t value = 0;
    if (get_point_temp(
            const_cast<uint16_t*>(frame.data.data()),
            resolution_of(frame), point,
            &value) != IRTEMP_SUCCESS) {
        return false;
    }
    celsius = temp_value_converter(value);
    return true;
}

bool line_temperature(
    const TemperatureFrame& frame, const Line_t& line,
    TempInfo_t& temperature_info)
{
    if (!frame.valid()) {
        return false;
    }
    return get_line_temp(
               const_cast<uint16_t*>(frame.data.data()),
               resolution_of(frame), line,
               &temperature_info) == IRTEMP_SUCCESS;
}

bool rect_temperature(
    const TemperatureFrame& frame, const Area_t& rectangle,
    TempInfo_t& temperature_info)
{
    if (!frame.valid()) {
        return false;
    }
    return get_rect_temp(
               const_cast<uint16_t*>(frame.data.data()),
               resolution_of(frame), rectangle,
               &temperature_info) == IRTEMP_SUCCESS;
}

bool full_frame_temperature(
    const TemperatureFrame& frame, TempInfo_t& temperature_info)
{
    if (!frame.valid()) {
        return false;
    }
    Area_t rectangle = {0, 0, frame.width, frame.height};
    return rect_temperature(frame, rectangle, temperature_info);
}

void point_temp_demo(uint16_t* temperature_data,
                     TempDataRes_t resolution)
{
    if (temperature_data == NULL ||
        resolution.width == 0 || resolution.height == 0) {
        return;
    }

    Dot_t point = {
        static_cast<int>(resolution.width / 2),
        static_cast<int>(resolution.height / 2)
    };
    uint16_t temperature = 0;
    if (get_point_temp(temperature_data, resolution, point,
                       &temperature) == IRTEMP_SUCCESS) {
        std::printf("point(%d,%d) temp: %.2f C\n",
                    point.x, point.y,
                    temp_value_converter(temperature));
    }
}

void line_temp_demo(uint16_t* temperature_data,
                    TempDataRes_t resolution)
{
    if (temperature_data == NULL ||
        resolution.width == 0 || resolution.height == 0) {
        return;
    }

    const int center_x = resolution.width / 2;
    Line_t line = {
        center_x,
        static_cast<int>(resolution.height) - 1,
        center_x,
        0
    };
    TempInfo_t information = {0};
    if (get_line_temp(temperature_data, resolution, line,
                      &information) == IRTEMP_SUCCESS) {
        std::printf(
            "line temp: max=%.2f C, min=%.2f C, avg=%.2f C\n",
            temp_value_converter(information.max_temp),
            temp_value_converter(information.min_temp),
            temp_value_converter(information.avr_temp));
    }
}

void rect_temp_demo(uint16_t* temperature_data,
                    TempDataRes_t resolution)
{
    if (temperature_data == NULL ||
        resolution.width == 0 || resolution.height == 0) {
        return;
    }

    Area_t rectangle = {
        static_cast<int>(resolution.width / 4),
        static_cast<int>(resolution.height / 4),
        static_cast<int>(resolution.width / 2),
        static_cast<int>(resolution.height / 2)
    };
    TempInfo_t information = {0};
    if (get_rect_temp(temperature_data, resolution, rectangle,
                      &information) == IRTEMP_SUCCESS) {
        std::printf(
            "rectangle temp: max=%.2f C, min=%.2f C, avg=%.2f C\n",
            temp_value_converter(information.max_temp),
            temp_value_converter(information.min_temp),
            temp_value_converter(information.avr_temp));
    }
}

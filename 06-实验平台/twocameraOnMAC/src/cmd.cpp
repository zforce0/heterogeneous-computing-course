#include "cmd.h"

#include "libirtemp.h"
#include "libiruvc.h"
#include "sample_config.h"
#include "temperature.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <poll.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

const int kSuccess = 0;
const int kFailure = -1;

uint16_t new_nuc_table[NUC_T_SIZE] = {0};
uint16_t new_kt[KT_SIZE] = {0};
int16_t new_bt[BT_SIZE] = {0};
uint16_t kt_array[KT_SIZE] = {0};
int16_t bt_array[BT_SIZE] = {0};
uint16_t nuct_array[NUC_T_SIZE] = {0};
TempCalibParam_t temperature_calibration = {0};

long file_length(const char* path)
{
    if (path == NULL) {
        return -1;
    }
    FILE* file = std::fopen(path, "rb");
    if (file == NULL) {
        return -1;
    }
    if (std::fseek(file, 0, SEEK_END) != 0) {
        std::fclose(file);
        return -1;
    }
    const long length = std::ftell(file);
    std::fclose(file);
    return length;
}

bool read_binary_file(const char* path, void* destination,
                      size_t capacity, size_t& bytes_read)
{
    bytes_read = 0;
    if (path == NULL || destination == NULL) {
        return false;
    }

    const long length = file_length(path);
    if (length <= 0 ||
        static_cast<unsigned long>(length) > capacity) {
        std::fprintf(stderr,
                     "Invalid file size for %s: %ld (max %zu)\n",
                     path, length, capacity);
        return false;
    }

    FILE* file = std::fopen(path, "rb");
    if (file == NULL) {
        std::fprintf(stderr, "Cannot open %s: %s\n",
                     path, std::strerror(errno));
        return false;
    }
    bytes_read = std::fread(destination, 1,
                            static_cast<size_t>(length), file);
    const bool ok =
        bytes_read == static_cast<size_t>(length) &&
        std::ferror(file) == 0;
    std::fclose(file);
    if (!ok) {
        std::fprintf(stderr, "Cannot read complete file %s\n",
                     path);
    }
    return ok;
}

int read_nuc_parameter(TempCalInfo_t* calibration)
{
    if (calibration == NULL ||
        calibration->nuc_table == NULL) {
        return kFailure;
    }

    uint8_t data[0x4000] = {0};
    if (spi_read(0xda000, sizeof(data), data) !=
        IRUVC_SUCCESS) {
        std::fprintf(stderr, "get NUC-T table failed\n");
        return kFailure;
    }
    for (size_t i = 0; i + 1 < sizeof(data); i += 2) {
        const uint8_t temporary = data[i];
        data[i] = data[i + 1];
        data[i + 1] = temporary;
    }
    std::memcpy(calibration->nuc_table, data, sizeof(data));
    return kSuccess;
}

int calculate_original_environment(TempCalInfo_t* calibration)
{
    if (calibration == NULL) {
        return kFailure;
    }
    if (get_prop_tpd_params(
            TPD_PROP_EMS,
            reinterpret_cast<uint16_t*>(
                &calibration->org_env_param->EMS)) !=
        IRUVC_SUCCESS) {
        std::fprintf(stderr, "get EMS failed\n");
        return kFailure;
    }
    if (get_prop_tpd_params(
            TPD_PROP_TAU,
            reinterpret_cast<uint16_t*>(
                &calibration->org_env_param->TAU)) !=
        IRUVC_SUCCESS) {
        std::fprintf(stderr, "get TAU failed\n");
        return kFailure;
    }
    if (get_prop_tpd_params(
            TPD_PROP_TA,
            reinterpret_cast<uint16_t*>(
                &calibration->org_env_param->Ta)) !=
        IRUVC_SUCCESS) {
        std::fprintf(stderr, "get TA failed\n");
        return kFailure;
    }
    if (get_prop_tpd_params(
            TPD_PROP_TU,
            reinterpret_cast<uint16_t*>(
                &calibration->org_env_param->Tu)) !=
        IRUVC_SUCCESS) {
        std::fprintf(stderr, "get TU failed\n");
        return kFailure;
    }
    if (calculate_org_KE_and_BE_with_nuc_t(
            calibration->org_env_param,
            calibration->nuc_table,
            calibration->gain_flag,
            calibration->org_env_factor) != IRTEMP_SUCCESS) {
        std::fprintf(
            stderr,
            "calculate_org_KE_and_BE_with_nuc_t failed\n");
        return kFailure;
    }
    return kSuccess;
}

int compatible_correction_table(
    uint16_t* table, uint16_t** valid_table)
{
    if (table == NULL || valid_table == NULL) {
        return kFailure;
    }
    if (table[2] == 0xffff && table[3] == 0xffff) {
        *valid_table = table + HEAD_SIZE;
    } else {
        *valid_table = table;
    }
    return kSuccess;
}

void multi_point_calibration(
    uint16_t* correction_table, uint16_t* default_nuc_table,
    uint16_t* original_kt, int16_t* original_bt,
    TempCalibParam_t* calibration)
{
    if (correction_table == NULL ||
        default_nuc_table == NULL ||
        original_kt == NULL || original_bt == NULL ||
        calibration == NULL) {
        return;
    }

    second_calibration_param_t second_calibration = {
        calibration->Ktemp,
        calibration->Btemp,
        calibration->AddressCA,
        KT_SIZE,
        BT_SIZE,
        NUC_T_SIZE
    };
    second_calibration.setting_temp_high = 415.9f;
    second_calibration.setting_temp_low = 123.9f;
    second_calibration.high_vtemp = 8400;
    second_calibration.low_vtemp = 8400;
    second_calibration.object_temp_high = 341.15f;
    second_calibration.object_temp_low = 104.5f;

    const float emissivity = 0.95f;
    const float humidity = 0.5f;
    const float distance = 0.25f;
    const float atmospheric_temperature = 25.0f;
    const float reflected_temperature = 25.0f;
    EnvCorrectParam environment[] = {
        {distance, emissivity, humidity,
         atmospheric_temperature, reflected_temperature},
        {distance, emissivity, humidity,
         atmospheric_temperature, reflected_temperature},
        {distance, emissivity, humidity,
         atmospheric_temperature, reflected_temperature}
    };
    MultiPointCalibTemp_t temperatures[] = {
        {6042.0f / 16.0f - 273.15f, 123.9f},
        {7776.5f / 16.0f - 273.15f, 259.4f},
        {9828.8f / 16.0f - 273.15f, 415.9f}
    };
    MultiPointCalibNuc_t nuc_points[] = {
        {0, 0}, {0, 0}, {0, 0}
    };

    MultiPointCalibArray_t arrays = {
        original_kt,
        original_bt,
        default_nuc_table,
        correction_table
    };
    TwoPointCalibResult_t result = {new_kt, new_bt};
    MultiPointCalibParam_t parameters = {
        environment[0], second_calibration, 8400
    };
    if (new_ktbt_recal_double_point_calculate(
            &parameters, 0, &arrays,
            &result) != IRTEMP_SUCCESS) {
        std::fprintf(
            stderr,
            "new_ktbt_recal_double_point_calculate failed\n");
        return;
    }
    std::printf("new_kt[0]=%u, new_bt[0]=%d\n",
                result.new_kt_array[0],
                result.new_bt_array[0]);

    const uint16_t point_count = static_cast<uint16_t>(
        sizeof(temperatures) / sizeof(temperatures[0]));
    for (uint16_t i = 0; i < point_count; ++i) {
        MultiPointCalibParam_t point_parameters = {
            environment[i], second_calibration, 8400
        };
        if (multi_point_calc_user_defined_nuc(
                &point_parameters, 0, &arrays, &result,
                &temperatures[i],
                &nuc_points[i]) != IRTEMP_SUCCESS) {
            std::fprintf(
                stderr,
                "multi_point_calc_user_defined_nuc failed at %u\n",
                i);
            return;
        }
        std::printf("setting_nuc=%u, output_nuc=%u\n",
                    nuc_points[i].setting_nuc / 2,
                    nuc_points[i].output_nuc / 2);
    }

    if (multi_point_calc_new_nuc_table(
            default_nuc_table, nuc_points, point_count,
            new_nuc_table) != IRTEMP_SUCCESS) {
        std::fprintf(
            stderr,
            "multi_point_calc_new_nuc_table failed\n");
        return;
    }
    std::printf("new_nuc_table[0]=%u\n",
                new_nuc_table[0]);
}

void print_command_help()
{
    std::puts(
        "IR commands: 0 info, 1 shutter, 2 shutter background, "
        "3/4 zoom, 10 rectangle temp, 11 vtemp, 13 SPI, "
        "14/15 Y16 start/stop, 16-18 temp correction, "
        "19/20 gain, 23 DPC, 24 restore, 25/26 point zoom, "
        "27/28 mirror/flip, 29-31 calibration");
}

} // namespace

int command_init()
{
    const int wait_result =
        vdcmd_set_polling_wait_time(10000);
    if (wait_result != IRUVC_SUCCESS) {
        std::fprintf(stderr,
                     "vdcmd_set_polling_wait_time failed: %d\n",
                     wait_result);
        return wait_result;
    }
    const int result = vdcmd_init();
    if (result != IRUVC_SUCCESS) {
        std::fprintf(stderr, "vdcmd_init failed: %d\n",
                     result);
    }
    return result;
}

bool update_fw_cmd(const char* file_path)
{
    const long length = file_length(file_path);
    if (length <= 0) {
        std::fprintf(stderr,
                     "Invalid firmware file: %s\n",
                     file_path != NULL ? file_path : "(null)");
        return false;
    }

    std::vector<uint8_t> firmware(
        static_cast<size_t>(length));
    size_t bytes_read = 0;
    if (!read_binary_file(file_path, firmware.data(),
                          firmware.size(), bytes_read)) {
        return false;
    }
    const int result = update_fw(
        firmware.data(), static_cast<int>(bytes_read));
    if (result != IRUVC_SUCCESS) {
        std::fprintf(stderr, "update_fw failed: %d\n",
                     result);
        return false;
    }
    std::printf("Firmware updated from %s\n", file_path);
    return true;
}

void command_sel(int command_type)
{
    uint8_t id_data[8] = {0};
    uint8_t flash_data[0x30] = {0};
    uint16_t temperature_data = 0;
    IruvcRect_t rectangle = {100, 100, 300, 300};
    TpdLineRectTempInfo_t rectangle_temperature = {0};
    IruvcPoint_t position = {80, 60};
    TempCalInfo_t* calibration = get_temp_cal_info();
    uint16_t* current_correction_table = NULL;
    size_t bytes_read = 0;

    switch (command_type) {
    case 0:
        if (get_device_info(
                DEV_INFO_PROJECT_INFO,
                id_data) == IRUVC_SUCCESS) {
            std::printf("device info:");
            for (size_t i = 0; i < sizeof(id_data); ++i) {
                std::printf(" 0x%x", id_data[i]);
            }
            std::printf("\n");
        }
        break;
    case 1:
        shutter_sta_set(SHUTTER_CTL_EN);
        ooc_b_update(B_UPDATE);
        std::puts("shutter");
        break;
    case 2:
        shutter_sta_set(SHUTTER_CTL_DIS);
        shutter_manual_switch(SHUTTER_OPEN);
        ooc_b_update(B_UPDATE);
        shutter_sta_set(SHUTTER_CTL_EN);
        std::puts("shutter background");
        break;
    case 3:
        zoom_center_up(PREVIEW_PATH0, ZOOM_STEP4);
        std::puts("zoom_center_up");
        break;
    case 4:
        zoom_center_down(PREVIEW_PATH0, ZOOM_STEP4);
        std::puts("zoom_center_down");
        break;
    case 5:
        if (get_device_info(
                DEV_INFO_PROJECT_INFO,
                id_data) == IRUVC_SUCCESS) {
            std::printf("project info:");
            for (int i = 0; i < 4; ++i) {
                std::printf(" 0x%x", id_data[i]);
            }
            std::printf("\n");
        }
        break;
    case 10:
        if (tpd_get_rect_temp_info(
                rectangle, &rectangle_temperature) ==
            IRUVC_SUCCESS) {
            std::printf(
                "rect temp: min(%d,%d)=%u, max(%d,%d)=%u\n",
                rectangle_temperature.min_temp_point.x,
                rectangle_temperature.min_temp_point.y,
                rectangle_temperature.temp_info_value.min_temp,
                rectangle_temperature.max_temp_point.x,
                rectangle_temperature.max_temp_point.y,
                rectangle_temperature.temp_info_value.max_temp);
        }
        break;
    case 11:
        if (cur_vtemp_get(&temperature_data) ==
            IRUVC_SUCCESS) {
            std::printf("current vtemp: %u\n",
                        temperature_data);
        }
        break;
    case 13:
        if (spi_read(0x28e000, sizeof(flash_data),
                     flash_data) == IRUVC_SUCCESS) {
            for (size_t i = 0;
                 i < sizeof(flash_data); ++i) {
                std::printf("0x%x ", flash_data[i]);
            }
            std::printf("\n");
        }
        break;
    case 14:
        y16_preview_start(PREVIEW_PATH0, Y16_MODE_SNR);
        std::puts("y16_preview_start SNR");
        break;
    case 15:
        y16_preview_stop(PREVIEW_PATH0);
        std::puts("y16_preview_stop");
        break;
    case 16:
        if (read_nuc_parameter(calibration) == kSuccess &&
            calculate_original_environment(calibration) ==
                kSuccess) {
            print_cali_info(calibration);
        }
        break;
    case 17: {
        if (!read_binary_file(
                HIGH_GAIN_TAU_FILE_PATH, correct_table,
                sizeof(correct_table), bytes_read)) {
            break;
        }
        if (calculate_new_env_cali_parameter(
                correct_table, 1.0, 27.0, 27.0,
                0.25, 1.0) != 0) {
            break;
        }
        uint16_t device_value = 0;
        if (tpd_get_point_temp_info(position,
                                    &device_value) !=
            IRUVC_SUCCESS) {
            break;
        }
        const double original_temperature =
            device_value / 16.0;
        double corrected_temperature = 0.0;
        if (temp_calc_with_new_env_calibration(
                calibration, original_temperature,
                &corrected_temperature) == 0) {
            std::printf(
                "original=%.2f C, corrected=%.2f C\n",
                original_temperature - 273.15,
                corrected_temperature - 273.15);
        }
        break;
    }
    case 18: {
        if (!read_binary_file(
                NEW_HIGH_GAIN_TAU_FILE_PATH, correct_table,
                sizeof(correct_table), bytes_read) ||
            compatible_correction_table(
                correct_table,
                &current_correction_table) != kSuccess) {
            break;
        }
        uint16_t device_value = 0;
        if (tpd_get_point_temp_info(position,
                                    &device_value) !=
            IRUVC_SUCCESS) {
            break;
        }
        const double device_temperature =
            device_value / 16.0;
        double uncorrected_kelvin = 0.0;
        if (temp_calc_without_any_correct(
                calibration, device_temperature,
                &uncorrected_kelvin) != 0) {
            break;
        }
        const float uncorrected_celsius =
            static_cast<float>(uncorrected_kelvin - 273.15);
        const float distance = 5.0f;
        const float emissivity = 1.0f;
        const float atmospheric_temperature = 25.0f;
        uint16_t tau = 0;
        float first_correction = 0.0f;
        float second_correction = 0.0f;
        if (read_tau_with_target_temp_and_dist(
                current_correction_table,
                uncorrected_celsius, distance,
                &tau) != IRTEMP_SUCCESS ||
            temp_correct(
                emissivity, tau,
                atmospheric_temperature,
                uncorrected_celsius,
                &first_correction) != IRTEMP_SUCCESS ||
            read_tau_with_target_temp_and_dist(
                current_correction_table,
                first_correction, distance,
                &tau) != IRTEMP_SUCCESS ||
            temp_correct(
                emissivity, tau,
                atmospheric_temperature,
                uncorrected_celsius,
                &second_correction) != IRTEMP_SUCCESS) {
            std::fprintf(stderr,
                         "iterative temperature correction failed\n");
            break;
        }
        std::printf(
            "device=%.2f C, uncorrected=%.2f C, corrected=%.2f C\n",
            device_temperature - 273.15,
            uncorrected_celsius, second_correction);
        break;
    }
    case 19:
        set_prop_tpd_params(TPD_PROP_GAIN_SEL, 0);
        std::puts("low gain");
        break;
    case 20:
        set_prop_tpd_params(TPD_PROP_GAIN_SEL, 1);
        std::puts("high gain");
        break;
    case 23:
        dpc_auto_calibration(30);
        std::puts("dpc_auto_calibration completed");
        break;
    case 24:
        restore_default_cfg(DEF_CFG_TPD);
        std::puts("restore TPD defaults completed");
        break;
    case 25:
        zoom_position_up(PREVIEW_PATH0, ZOOM_STEP4,
                         position);
        std::puts("zoom_position_up completed");
        break;
    case 26:
        zoom_position_down(PREVIEW_PATH0, ZOOM_STEP4,
                           position);
        std::puts("zoom_position_down completed");
        break;
    case 27:
        set_prop_image_params(
            IMAGE_PROP_SEL_MIRROR_FLIP, 3);
        std::puts("camera mirror+flip enabled");
        break;
    case 28:
        set_prop_image_params(
            IMAGE_PROP_SEL_MIRROR_FLIP, 2);
        std::puts("camera flip enabled");
        break;
    case 29:
        set_prop_tpd_params(TPD_PROP_GAIN_SEL, 0);
        restore_default_cfg(DEF_CFG_TPD);
        if (get_tpd_kt_array(
                reinterpret_cast<uint8_t*>(kt_array)) !=
                IRUVC_SUCCESS ||
            get_tpd_bt_array(
                reinterpret_cast<uint8_t*>(bt_array)) !=
                IRUVC_SUCCESS ||
            get_tpd_nuc_t_array(
                reinterpret_cast<uint8_t*>(nuct_array)) !=
                IRUVC_SUCCESS ||
            get_tpd_calib_param(
                &temperature_calibration) !=
                IRUVC_SUCCESS) {
            std::fprintf(stderr,
                         "read TPD calibration arrays failed\n");
            break;
        }
        std::printf(
            "KT[0]=%u KT[1200]=%u, "
            "BT[0]=%d BT[1200]=%d, "
            "NUC[0]=%u NUC[8191]=%u, "
            "Ktemp=%d Btemp=%d AddressCA=%u\n",
            kt_array[0], kt_array[KT_SIZE - 1],
            bt_array[0], bt_array[BT_SIZE - 1],
            nuct_array[0], nuct_array[NUC_T_SIZE - 1],
            temperature_calibration.Ktemp,
            temperature_calibration.Btemp,
            temperature_calibration.AddressCA);
        break;
    case 30:
        if (read_binary_file(
                LOW_GAIN_TAU_FILE_PATH, correct_table,
                sizeof(correct_table), bytes_read)) {
            multi_point_calibration(
                correct_table, nuct_array, kt_array,
                bt_array, &temperature_calibration);
        }
        break;
    case 31:
        if (set_tpd_kt_array(
                reinterpret_cast<uint8_t*>(new_kt)) !=
                IRUVC_SUCCESS ||
            set_tpd_bt_array(
                reinterpret_cast<uint8_t*>(new_bt)) !=
                IRUVC_SUCCESS ||
            set_tpd_nuc_t_array(
                reinterpret_cast<uint8_t*>(
                    new_nuc_table)) != IRUVC_SUCCESS) {
            std::fprintf(stderr,
                         "write TPD calibration arrays failed\n");
            break;
        }
        std::puts("TPD calibration arrays written");
        break;
    default:
        print_command_help();
        break;
    }
}

CommandConsole::CommandConsole()
    : running_(false)
{
}

CommandConsole::~CommandConsole()
{
    stop();
}

bool CommandConsole::start()
{
    if (running_.exchange(true)) {
        return false;
    }
    thread_ = std::thread(&CommandConsole::run, this);
    return true;
}

void CommandConsole::stop()
{
    running_.store(false);
    if (thread_.joinable()) {
        thread_.join();
    }
}

void CommandConsole::run()
{
    print_command_help();
    pollfd input = {};
    input.fd = STDIN_FILENO;
    input.events = POLLIN;
    char line[64] = {0};

    while (running_.load()) {
        input.revents = 0;
        const int result = poll(&input, 1, 200);
        if (!running_.load()) {
            break;
        }
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::fprintf(stderr, "command poll failed: %s\n",
                         std::strerror(errno));
            break;
        }
        if (result == 0) {
            continue;
        }
        if ((input.revents & POLLIN) != 0) {
            if (std::fgets(line, sizeof(line), stdin) == NULL) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(200));
                continue;
            }
            char* end = NULL;
            const long command = std::strtol(line, &end, 10);
            if (end == line) {
                print_command_help();
                continue;
            }
            command_sel(static_cast<int>(command));
        } else if ((input.revents &
                    (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(200));
        }
    }
}

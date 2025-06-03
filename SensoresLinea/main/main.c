// Lectura sensores de linea

// Librerias
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <sys/_intsup.h>
#include <sys/unistd.h>
#include "driver/i2c.h"
#include "driver/mcpwm_prelude.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "driver/pulse_cnt.h"

// Sintonia
#define vd 0.15f

#define k1 0.18f
#define k2 2.25f
#define k3 1.0f
#define k4 1.5f
#define kp 0.12f
#define kv 0.015f

// Compensacion angulo alpha
#define calpha  -0.4f

//Parametros
#define Ts      0.01f
#define ppr     12.0f
#define Ra      3.0f
#define NR      44.0f
#define R       0.034f
#define km      0.0008f
#define b       0.0965f
#define c1 0.995f

// Conversiones
#define deg_2_rad          0.0174533 
#define rad_2_deg          57.29578
#define accel_div_factor   16384.0
#define gyro_div_factor    131.0
#define accel_factor       0.0000610352
#define gyro_factor        0.0076336
#define pi_     3.141593f
#define pi_s2   1.570796f

// Constantes para calculos de control
#define c2                 (1.0f - c1)                    // 0.005f
#define iTs                (1.0f/Ts)                      // 100.0f
#define esc                (pi_/(2.0f*ppr*NR))           // 0.003581f
#define escs               (255.0f/uM)                    // 24.171f
#define Rasnkm             (Ra/(NR*km))                   // 85227.27f
#define nkm                (NR*km)                        // 0.0352f
#define v2tauM             (2.0f*tauM)                    // 0.6f
#define mv                 (255.0f/(2.0f*omegaM*R))      // 234.007f
#define mtheta             (255.0f/(2.0f*0.3f))          // 425.0f
#define malpha             (255.0f/(2.0f*alphaM))        // 91.071f
#define momega             (255.0f/(2.0f*omegaM))        // 7.968f
#define mu                 (255.0f/(2.0f*uM))            // 12.085f
#define deg_2_rad_gyro     (deg_2_rad * gyro_factor)     // 0.000133f
#define accel_pi_s2        (accel_factor * pi_s2)        // 0.0000958f
#define pwm_scale          (20000.0f / 255.0f)           // 78.431f
#define two_b              (2.0f * b)                     // 0.193f
#define R_over_two_b       (R / two_b)                    // 0.176f
#define two_b_over_R       (two_b / R)                    // 5.676f

// Angulos maximos
#define tauM    0.3f
#define alphaM  1.4f
#define omegaM  16.0f

// Voltajes maximos
#define uM      10.55f
#define uNM     10.00f

// Pines ESP32-MPU6050
#define SDA_PIN 21
#define SCL_PIN 22

// Pines ESP32 - PH
#define MOTOR_R_POS 12
#define MOTOR_R_NEG 14
#define MOTOR_L_POS 26
#define MOTOR_L_NEG 27

// Pines ESP32 - PWM
#define PWM_R_PIN 13
#define PWM_L_PIN 25

//Pines ESP32 - Encoders
#define ENC_R_A 18
#define ENC_R_B 19
#define ENC_L_A 4
#define ENC_L_B 5

//Pines ESP32 - Sensores de linea
#define ADC_R 36
#define ADC_L 39

// Constantes para sensores de línea
#define ADC_MAX_VALUE 4095.0f
#define LINE_SENSOR_THRESHOLD ADC_MAX_VALUE*0.85  // 50% del rango ADC
#define LINE_SENSOR_SCALE (-0.3f / LINE_SENSOR_THRESHOLD)  // Pre-calculado

// Registros MPU6050
#define MPU6050_ADDR    0x68
#define PWR_MGMT_1      0x6B
#define CONFIG_R        0x1A
#define GYRO_CONFIG     0x1B
#define ACCEL_CONFIG    0x1C
#define ACCEL_XOUT_H    0x3B
#define ACCEL_XOUT_L    0x3C
#define GYRO_YOUT_H     0x45
#define GYRO_YOUT_L     0x46

// Constantes para encoders
#define PCNT_H_LIM 32767
#define PCNT_L_LIM -32768
#define ENCODER_OVERFLOW_THRESHOLD 16383
#define ENCODER_RANGE 65536

// Parametros I2C
#define I2C_MASTER_NUM  I2C_NUM_0
#define I2C_FREQ_HZ     400000
#define I2C_TIMEOUT_MS  1000

// Periodo de muestreo
#define TIMER_PERIOD_US 10000

//static const char *TAG = "i2c-simple-example";

// Variables para guardar datos del MPU6050
int16_t Ax, Gy;
uint8_t A_data_x[14];
uint8_t G_data_y[14];

// Variables de control del angulo para balanceo
float Xa = 0.0f, Yg = 0.0f, alpha = 0.0f;
float accelx = 0.15f, angulox = 0.15f, angulox_1 = 0.0f;

// tiempo
float t = 0.0f;

// variables para control de velocidad
float omegar = 0.0f, omegal = 0.0f;

// control de ruedas
float ur = 0.0f, ul = 0.0f;
int8_t uWr,uWl;

// Variables seguidor de linea
float Sr,Sl;

// variables de angulos vertical y horizontal, velicidad, par
float v = 0.0f, alpha_1 = 0.0f, theta = 0.0f, thetad = -0.03f, thetatil = 0.0f, taua = 0.0f, u = 0.0f;
float alphap = 0.0f, vtil = 0.0f, thetap = 0.0f, taur = 0.0f, taul = 0.0f;
float intvtil = 0.0f;


// Variables para motores
int16_t posr = 0, posl = 0;        // Posiciones acumuladas
int16_t incr = 0, incl = 0;        // Incrementos de encoders
int16_t last_count_r = 0, last_count_l = 0;

// Variables para componentes
//static bool uart_working = true;
adc_oneshot_unit_handle_t adc1_handle;
mcpwm_timer_handle_t mcpwm_timer = NULL;
mcpwm_cmpr_handle_t comparator_a = NULL;
mcpwm_cmpr_handle_t comparator_b = NULL;
mcpwm_gen_handle_t generator_a = NULL;
mcpwm_gen_handle_t generator_b = NULL;
mcpwm_oper_handle_t operator = NULL;

//Calibracion del ADC
static void adc_calibration_deinit(adc_cali_handle_t handle);
adc_cali_handle_t adc1_cali_chan0_handle = NULL;
adc_cali_handle_t adc1_cali_chan3_handle = NULL;
bool do_calibration1_chan0 = 0;
bool do_calibration1_chan3 = 0;

// Contador de pulsos del encoder
pcnt_unit_handle_t pcnt_unit_r = NULL;
pcnt_unit_handle_t pcnt_unit_l = NULL;


// Variables para el timer - VOLÁTILES por ISR
volatile SemaphoreHandle_t timer_semaphore;
esp_timer_handle_t periodic_timer;

// Calibracion del ADC
static bool adc_calibration_init(adc_unit_t unit, adc_channel_t channel, adc_atten_t atten, adc_cali_handle_t *out_handle)
{
    adc_cali_handle_t handle = NULL;
    esp_err_t ret = ESP_FAIL;
    bool calibrated = false;
    if (!calibrated) {
        
        adc_cali_line_fitting_config_t cali_config = {
            .unit_id = ADC_UNIT_1,
            .atten = ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_12,
        };
        ret = adc_cali_create_scheme_line_fitting(&cali_config, &handle);
        if (ret == ESP_OK) {
            calibrated = true;
        }
    }

    *out_handle = handle;

    return calibrated;
}

//Inicializar ADC
void adc_init(void) {
	
	// Seleccionar ADC
    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = ADC_UNIT_1,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &adc1_handle));
    
    // Configuracion de resolucion
    adc_oneshot_chan_cfg_t config = {
        .bitwidth = ADC_BITWIDTH_12,
        .atten = ADC_ATTEN_DB_12,
    };
    
    // Inicializar canales
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, ADC_CHANNEL_0, &config));
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, ADC_CHANNEL_3, &config));
    
    // Anadir calibracion al ADC
    do_calibration1_chan0 = adc_calibration_init(ADC_UNIT_1, ADC_CHANNEL_0, ADC_ATTEN_DB_12, &adc1_cali_chan0_handle);
    do_calibration1_chan3 = adc_calibration_init(ADC_UNIT_1, ADC_CHANNEL_3, ADC_ATTEN_DB_12, &adc1_cali_chan3_handle);
}

// Cerrar calibracion
static void adc_calibration_deinit(adc_cali_handle_t handle)
{
    ESP_ERROR_CHECK(adc_cali_delete_scheme_line_fitting(handle));

}

void read_line_sensors(void){
	int adc_reading_r = 0, adc_reading_l = 0, voltage_reading_r=0, voltage_reading_l=0;
    
    // Leer ambos canales consecutivamente para mejor performance
    //Porque una matriz?
    ESP_ERROR_CHECK(adc_oneshot_read(adc1_handle, ADC_CHANNEL_0, &adc_reading_r));
    
    if (do_calibration1_chan0) {
            ESP_ERROR_CHECK(adc_cali_raw_to_voltage(adc1_cali_chan0_handle, adc_reading_r, &voltage_reading_r));
        }
    usleep(15);    
    ESP_ERROR_CHECK(adc_oneshot_read(adc1_handle, ADC_CHANNEL_3, &adc_reading_l));
    if (do_calibration1_chan3) {
            ESP_ERROR_CHECK(adc_cali_raw_to_voltage(adc1_cali_chan3_handle, adc_reading_l, &voltage_reading_l));
        }
    
    printf("R: %d L: %d VR: %d  VL: %d ",adc_reading_r,adc_reading_l, voltage_reading_r, voltage_reading_l);
    
    // OPTIMIZACIÓN: comparación directa sin conversión a float
    Sr = (adc_reading_r > 4095) ? 4095 : (int16_t)adc_reading_r;
    Sl = (adc_reading_l > 4095) ? 4095 : (int16_t)adc_reading_l;
    
    printf("R: %f L: %f ",Sr,Sl);
    usleep(15);
}

void app_main(void)
{
	adc_init();
    while (true) {
        read_line_sensors();
        
        // Cálculo de theta con constante pre-calculada
        float theta_diff = (float)(Sr - Sl);
        
        printf("thetha_diff: %f ",theta_diff);
            
        if (theta_diff > LINE_SENSOR_THRESHOLD) {
            theta_diff = LINE_SENSOR_THRESHOLD;
        }
        if (theta_diff < -LINE_SENSOR_THRESHOLD) {
            theta_diff = -LINE_SENSOR_THRESHOLD;
        }
            
       	// OPTIMIZACIÓN: usar constante pre-calculada
       	theta = theta_diff * LINE_SENSOR_SCALE;
       	printf("thetha_diff: %f \n",theta_diff);
       	// OPTIMIZACIÓN: usar constantes pre-calculadas
        omegar = (float)incr * esc * iTs;
        omegal = (float)incl * esc * iTs;
        
        // OPTIMIZACIÓN: usar constantes pre-calculadas
        v = (omegar + omegal) * R * 0.5f;
        thetap = (omegar - omegal) * R_over_two_b;  // OPTIMIZACIÓN
        
        thetatil = theta - thetad;
            
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    
    //Tear Down Calibracion
    ESP_ERROR_CHECK(adc_oneshot_del_unit(adc1_handle));
    if (do_calibration1_chan0) {
        adc_calibration_deinit(adc1_cali_chan0_handle);
    }
    if (do_calibration1_chan3) {
        adc_calibration_deinit(adc1_cali_chan3_handle);
    }
}

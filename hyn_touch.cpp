#include "Arduino.h"

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "hyn_core.h"

#define CONFIG_EXAMPLE_TOUCH_I2C_SDA_PIN 13
#define CONFIG_EXAMPLE_TOUCH_I2C_SCL_PIN 14
#define CONFIG_EXAMPLE_TOUCH_RST_PIN     45
#define CONFIG_EXAMPLE_TOUCH_INT_PIN     12

#define ARRAY_SIZE(a) (sizeof(a) / sizeof(a[0]))

const static char *TAG = "[HYN]";
static struct hyn_ts_data *hyn_data;
static xQueueHandle gpio_evt_queue;
static bool touch_press_flag = false;

static void IRAM_ATTR gpio_isr_handler(void *arg)
{
    uint32_t gpio_num = (uint32_t)arg;
    xQueueSendFromISR(gpio_evt_queue, &gpio_num, (BaseType_t *)NULL);
    touch_press_flag = true;
}

extern "C" uint8_t hyn_touch_get_point(int16_t *x_array, int16_t *y_array, uint8_t get_point)
{
    if(touch_press_flag) {
        touch_press_flag = false;
    } else {
        return 0;
    }

    int ret;
    hyn_data->hyn_irq_flg = 1;
    if (hyn_data->work_mode < DIFF_MODE)
    {
        ret = hyn_data->hyn_fuc_used->tp_report(); // Read point

        for (u8 i = 0; i < hyn_data->rp_buf.rep_num; i++)
        { // Modify the coordinate origin according to the configuration
            if (hyn_data->plat_data.swap_xy)
            {
                u16 tmp = hyn_data->rp_buf.pos_info[i].pos_x;
                hyn_data->rp_buf.pos_info[i].pos_x = hyn_data->rp_buf.pos_info[i].pos_y;
                hyn_data->rp_buf.pos_info[i].pos_y = tmp;
            }
            if (hyn_data->plat_data.reverse_x)
                hyn_data->rp_buf.pos_info[i].pos_x = hyn_data->plat_data.x_resolution - hyn_data->rp_buf.pos_info[i].pos_x;
            if (hyn_data->plat_data.reverse_y)
                hyn_data->rp_buf.pos_info[i].pos_y = hyn_data->plat_data.y_resolution - hyn_data->rp_buf.pos_info[i].pos_y;
        }
    }
    hyn_data->rp_buf.report_need = REPORT_NONE;

    return hyn_data->rp_buf.rep_num;
}

extern "C" int hyn_touch_init(void)
{
    int ret = 0;
    static struct hyn_ts_data ts_data;
    memset((void *)&ts_data, 0, sizeof(ts_data));
    hyn_data = &ts_data;
    ESP_LOGI(TAG, HYN_DRIVER_VERSION);

    struct hyn_ts_fuc *support_touch_list[] = {
        (struct hyn_ts_fuc *)&cst66xx_fuc,
        (struct hyn_ts_fuc *)&cst3xx_fuc,
        (struct hyn_ts_fuc *)&cst226se_fuc};

    hyn_data->hyn_fuc_used = &cst66xx_fuc;
    hyn_data->plat_data.max_touch_num = MAX_POINTS_REPORT;
    hyn_data->plat_data.irq_gpio = CONFIG_EXAMPLE_TOUCH_INT_PIN;
    hyn_data->plat_data.reset_gpio = CONFIG_EXAMPLE_TOUCH_RST_PIN;

    // Configure reset pin as push-pull output, output HIGH
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << hyn_data->plat_data.reset_gpio);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_config(&io_conf);

    // Initialize I2C master
    hyn_i2c_init(CONFIG_EXAMPLE_TOUCH_I2C_SDA_PIN, CONFIG_EXAMPLE_TOUCH_I2C_SCL_PIN);

    // Touch chip initialization - try each supported chip
    for (int i = 0; i < ARRAY_SIZE(support_touch_list); i++)
    {
        hyn_data->hyn_fuc_used = support_touch_list[i];
        ret = hyn_data->hyn_fuc_used->tp_chip_init(hyn_data);
        if (!ret)
        {
            ESP_LOGI(TAG, "Touch init SUCCEED");
            ESP_LOGI(TAG, "IC_info fw_project_id:%lx", hyn_data->hw_info.fw_project_id);
            ESP_LOGI(TAG, "ictype:[%lx]", hyn_data->hw_info.fw_chip_type);
            ESP_LOGI(TAG, "fw_ver:%lx", hyn_data->hw_info.fw_ver);
            break;
        }
    }
    if (ret)
    {
        ESP_LOGE(TAG, "Touch init FAILED - I2C NAK");
    }

    // Configure interrupt pin: input pull-up, falling edge interrupt
    io_conf.intr_type = GPIO_INTR_NEGEDGE;
    io_conf.pin_bit_mask = (1ULL << hyn_data->plat_data.irq_gpio);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io_conf);

    // Create queue for GPIO events from ISR
    gpio_evt_queue = xQueueCreate(10, sizeof(uint32_t));

    // Install GPIO ISR service and hook handler
    gpio_install_isr_service(0);
    gpio_isr_handler_add((gpio_num_t)hyn_data->plat_data.irq_gpio, gpio_isr_handler, (void *)hyn_data->plat_data.irq_gpio);

    return !ret;
}

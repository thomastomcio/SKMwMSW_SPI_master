#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h> // for rand

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"
#include "lwip/igmp.h"

#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "soc/rtc_periph.h"
#include "esp_log.h"
#include "esp_spi_flash.h"
#include "driver/gpio.h"

#include "http_server.h"
#include "SPI_task.h"

// wykorzystane piny do transmisji
#define GPIO_MOSI_M 12
#define GPIO_MISO_M 13
#define GPIO_SCLK_M 14
#define GPIO_CS_M 15

#define GPIO_MOSI_S 19
#define GPIO_MISO_S 23
#define GPIO_SCLK_S 18
#define GPIO_CS_S 5

#define GPIO_HANDSHAKE_SLAVE 2 
#define GPIO_HANDSHAKE_MASTER 4

#ifdef CONFIG_IDF_TARGET_ESP32
#define SENDER_HOST    HSPI_HOST
#define RCV_HOST       VSPI_HOST
#define DMA_CHAN_S    1
#define DMA_CHAN_M    2
#endif

// extern xQueueHandle rdySem;
extern xQueueHandle rdySem;

extern int pomiar;

//Main application
void app_main(void)
{
    // SPI slave
    esp_err_t ret;

    //Configuration for the SPI bus
    spi_bus_config_t buscfg_slave={
        .mosi_io_num=GPIO_MOSI_S,
        .miso_io_num=GPIO_MISO_S,
        .sclk_io_num=GPIO_SCLK_S,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };

    //Configuration for the SPI slave interface
    spi_slave_interface_config_t slvcfg={
        .mode=0,
        .spics_io_num=GPIO_CS_S,
        .queue_size=3,
        .flags=0,
        .post_setup_cb=my_post_setup_cb,
        .post_trans_cb=my_post_trans_cb
    };

    //Configuration for the handshake line
    gpio_config_t io_conf_slave={
        .intr_type=GPIO_INTR_DISABLE,
        .mode=GPIO_MODE_OUTPUT,
        .pin_bit_mask=(1<<GPIO_HANDSHAKE_SLAVE)
    };

    //Configure handshake line as output
    gpio_config(&io_conf_slave);
    //Enable pull-ups on SPI lines so we don't detect rogue pulses when no master is connected.
    gpio_set_pull_mode(GPIO_MOSI_S, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(GPIO_SCLK_S, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(GPIO_CS_S, GPIO_PULLUP_ONLY);

    //Initialize SPI slave interface
    ret=spi_slave_initialize(RCV_HOST, &buscfg_slave, &slvcfg, DMA_CHAN_S);
    assert(ret==ESP_OK);


    // SPI master
    esp_err_t ret_m;
    static spi_device_handle_t handle;

    //Configuration for the SPI bus
    spi_bus_config_t buscfg_master={
        .mosi_io_num=GPIO_MOSI_M,
        .miso_io_num=GPIO_MISO_M,
        .sclk_io_num=GPIO_SCLK_M,
        .quadwp_io_num=-1,
        .quadhd_io_num=-1
    };

    //Configuration for the SPI device on the other side of the bus
    spi_device_interface_config_t devcfg={
        .command_bits=0,
        .address_bits=0,
        .dummy_bits=0,
        .clock_speed_hz=5000000,
        .duty_cycle_pos=128,        //50% duty cycle
        .mode=0,
        .spics_io_num=GPIO_CS_M,
        .cs_ena_posttrans=3,        //Keep the CS low 3 cycles after transaction, to stop slave from missing the last bit when CS has less propagation delay than CLK
        .queue_size=3
    };

    //GPIO config for the handshake line.
    gpio_config_t io_conf_master={
        .intr_type=GPIO_PIN_INTR_POSEDGE,
        .mode=GPIO_MODE_INPUT,
        .pull_up_en=1,
        .pin_bit_mask=(1<<GPIO_HANDSHAKE_MASTER)
    };

    //Set up handshake line interrupt.
    gpio_config(&io_conf_master);
    gpio_install_isr_service(0);
    gpio_set_intr_type(GPIO_HANDSHAKE_MASTER, GPIO_PIN_INTR_POSEDGE);
    gpio_isr_handler_add(GPIO_HANDSHAKE_MASTER, gpio_handshake_isr_handler, NULL);

    //Initialize the SPI bus and add the device we want to send stuff to.
    ret_m=spi_bus_initialize(SENDER_HOST, &buscfg_master, DMA_CHAN_M);
    assert(ret_m==ESP_OK);
    ret_m=spi_bus_add_device(SENDER_HOST, &devcfg, &handle);
    assert(ret_m==ESP_OK);

    //Create the semaphore.
    rdySem=xSemaphoreCreateBinary();

    //Assume the slave is ready for the first transmission: if the slave started up before us, we will not detect
    //positive edge on the handshake line.
    xSemaphoreGive(rdySem);

    // 3 tasks for scheduling
    xTaskCreate(spi_slave_task, "SPI_slave", 8192, NULL, 0, NULL);
    xTaskCreate(spi_master_task, "SPI_master", 8192, &handle, 0, NULL);
    xTaskCreate(http_server_task, "HTTP_server", 8192, NULL, 0, NULL);

}

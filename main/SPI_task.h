#ifndef SPI_TASK_H
#define SPI_TASK_H

#include <string.h>

#include "driver/spi_slave.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#define GPIO_HANDSHAKE_SLAVE 2 
#define GPIO_HANDSHAKE_MASTER 4

#ifdef CONFIG_IDF_TARGET_ESP32
    #define SENDER_HOST    HSPI_HOST
    #define RCV_HOST       VSPI_HOST
#endif

extern int pomiar;

void IRAM_ATTR gpio_handshake_isr_handler(void* arg);
void my_post_setup_cb(spi_slave_transaction_t *trans);
void my_post_trans_cb(spi_slave_transaction_t *trans);
void spi_master_task( void * handle_ptr );
void spi_slave_task( void * spi_s_t );

#endif
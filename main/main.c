/* SPI Slave example, receiver (uses SPI Slave driver to communicate with sender)

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

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
#include "driver/spi_slave.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_spi_flash.h"
#include "driver/gpio.h"




/*
SPI receiver (slave) example.

This example is supposed to work together with the SPI sender. It uses the standard SPI pins (MISO, MOSI, SCLK, CS) to
transmit data over in a full-duplex fashion, that is, while the master puts data on the MOSI pin, the slave puts its own
data on the MISO pin.

This example uses one extra pin: GPIO_HANDSHAKE is used as a handshake pin. After a transmission has been set up and we're
ready to send/receive data, this code uses a callback to set the handshake pin high. The sender will detect this and start
sending a transaction. As soon as the transaction is done, the line gets set low again.
*/

/*
Pins in use. The SPI Master can use the GPIO mux, so feel free to change these if needed.
*/
// #define GPIO_HANDSHAKE 2
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
#define RCV_HOST VSPI_HOST
#define DMA_CHAN_S    1
#define DMA_CHAN_M    2
#endif

// #elif defined CONFIG_IDF_TARGET_ESP32S2
// #define RCV_HOST    SPI2_HOST
// #define DMA_CHAN    RCV_HOST
// #endif

// //The semaphore indicating the slave is ready to receive stuff.
static xQueueHandle rdySem;

/*
This ISR is called when the handshake line goes high.
*/
static void IRAM_ATTR gpio_handshake_isr_handler(void* arg)
{
    //Sometimes due to interference or ringing or something, we get two irqs after eachother. This is solved by
    //looking at the time between interrupts and refusing any interrupt too close to another one.
    static uint32_t lasthandshaketime;
    uint32_t currtime=xthal_get_ccount();
    uint32_t diff=currtime-lasthandshaketime;
    if (diff<240000) return; //ignore everything <1ms after an earlier irq
    lasthandshaketime=currtime;

    //Give the semaphore.
    BaseType_t mustYield=false;
    xSemaphoreGiveFromISR(rdySem, &mustYield);
    if (mustYield) portYIELD_FROM_ISR();
}

//Called after a transaction is queued and ready for pickup by master. We use this to set the handshake line high.
void my_post_setup_cb(spi_slave_transaction_t *trans) {
    WRITE_PERI_REG(GPIO_OUT_W1TS_REG, (1<<GPIO_HANDSHAKE_SLAVE));
}

//Called after transaction is sent/received. We use this to set the handshake line low.
void my_post_trans_cb(spi_slave_transaction_t *trans) {
    WRITE_PERI_REG(GPIO_OUT_W1TC_REG, (1<<GPIO_HANDSHAKE_SLAVE));
}

// SPI master task
void spi_master_task( void * handle_ptr )
{
    static int n;
    static char sendbuf_master[128];
    static char recvbuf_master[128];
    static spi_transaction_t t_master;

    for( ;; )
    {
        // SPI master
        esp_err_t ret_m;
        int res = snprintf(sendbuf_master, sizeof(sendbuf_master),
                "Sender, transmission no. %04i. Last time, I received: \"%s\"", n, recvbuf_master);
        if (res >= sizeof(sendbuf_master)) {
            printf("Data truncated\n");
        }
        t_master.length=sizeof(sendbuf_master)*8;
        t_master.tx_buffer=sendbuf_master;
        t_master.rx_buffer=recvbuf_master;
        //Wait for slave to be ready for next byte before sending
        xSemaphoreTake(rdySem, portMAX_DELAY); //Wait until slave is ready
        ret_m=spi_device_transmit(*((spi_device_handle_t*)handle_ptr), &t_master);
        if(ret_m==ESP_OK) printf("udana\n");
        printf("Received(master): %s\n", recvbuf_master);
        n++;
        vTaskDelay(100);
    }
}

// SPI slave task
void spi_slave_task( void * spi_s_t )
{
    static int n;
    static char sendbuf_slave[129];
    static char recvbuf_slave[129];
    static spi_transaction_t t_slave;
    for( ;; )
    {
        // SPI slave
        esp_err_t ret;
        //Clear receive buffer, set send buffer to something sane
        memset(recvbuf_slave, 0xA5, 129);
        sprintf(sendbuf_slave, "This is the receiver, sending data for transmission number %04d.", n);

        //Set up a transaction of 128 bytes to send/receive
        t_slave.length=128*8;
        t_slave.tx_buffer=sendbuf_slave;
        t_slave.rx_buffer=recvbuf_slave;
        /* This call enables the SPI slave interface to send/receive to the sendbuf and recvbuf. The transaction is
        initialized by the SPI master, however, so it will not actually happen until the master starts a hardware transaction
        by pulling CS low and pulsing the clock etc. In this specific example, we use the handshake line, pulled up by the
        .post_setup_cb callback that is called as soon as a transaction is ready, to let the master know it is free to transfer
        data.
        */
        printf("odbior slave'a\n");
        ret=spi_slave_transmit(RCV_HOST, &t_slave, portMAX_DELAY);
        if(ret==ESP_OK) printf("udana\n");
        //spi_slave_transmit does not return until the master has done a transmission, so by here we have sent our data and
        //received data from the master. Print it.
        printf("Received(slave): %s\n", recvbuf_slave);
        n++;
        vTaskDelay(100);
    }
}

// typedef struct SPI_slave_transmission
// {
//     WORD_ALIGNED_ATTR char sendbuf_slave[129];
//       WORD_ALIGNED_ATTR char recvbuf_slave[129]=;

// }SPI_slave_transmission;

// typedef struct SPI_master_transmission
// {
//     WORD_ALIGNED_ATTR char sendbuf_master[128];
//       WORD_ALIGNED_ATTR char recvbuf_master[128]=;

// }SPI_slave_transmission;

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

    // WORD_ALIGNED_ATTR char sendbuf_slave[129]="";
    // WORD_ALIGNED_ATTR char recvbuf_slave[129]="";
    // memset(recvbuf_slave, 0, 33);
    // spi_slave_transaction_t t_slave;
    // memset(&t_slave, 0, sizeof(t_slave));

    // SPI master
    esp_err_t ret_m;
    spi_device_handle_t handle;

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

    // int n=0;
    // char sendbuf_master[128] = {0};
    // char recvbuf_master[128] = {0};
    // spi_transaction_t t_master;
    // memset(&t_master, 0, sizeof(t_master));

    //Create the semaphore.
    rdySem=xSemaphoreCreateBinary();

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

    //Assume the slave is ready for the first transmission: if the slave started up before us, we will not detect
    //positive edge on the handshake line.
    xSemaphoreGive(rdySem);

    printf("dotychczas ok\n");

    xTaskCreate(spi_master_task, "SPI_master", 8192, (void*)&handle, 0, NULL);
    xTaskCreate(spi_slave_task, "SPI_slave", 8192, NULL, 0, NULL);

}

//     while(1) {

//         // SPI master
//         int res = snprintf(sendbuf_master, sizeof(sendbuf_master),
//                 "Sender, transmission no. %04i. Last time, I received: \"%s\"", n, recvbuf_master);
//         if (res >= sizeof(sendbuf_master)) {
//             printf("Data truncated\n");
//         }
//         t_master.length=sizeof(sendbuf_master)*8;
//         t_master.tx_buffer=sendbuf_master;
//         t_master.rx_buffer=recvbuf_master;
//         //Wait for slave to be ready for next byte before sending
//         xSemaphoreTake(rdySem, portMAX_DELAY); //Wait until slave is ready
//         ret_m=spi_device_transmit(handle, &t_master);
//         if(ret_m==ESP_OK) printf("udana\n");
//         printf("Received(master): %s\n", recvbuf_master);

//         // SPI slave
//         //Clear receive buffer, set send buffer to something sane
//         memset(recvbuf_slave, 0xA5, 129);
//         sprintf(sendbuf_slave, "This is the receiver, sending data for transmission number %04d.", n);

//         //Set up a transaction of 128 bytes to send/receive
//         t_slave.length=128*8;
//         t_slave.tx_buffer=sendbuf_slave;
//         t_slave.rx_buffer=recvbuf_slave;
//         /* This call enables the SPI slave interface to send/receive to the sendbuf and recvbuf. The transaction is
//         initialized by the SPI master, however, so it will not actually happen until the master starts a hardware transaction
//         by pulling CS low and pulsing the clock etc. In this specific example, we use the handshake line, pulled up by the
//         .post_setup_cb callback that is called as soon as a transaction is ready, to let the master know it is free to transfer
//         data.
//         */
//         printf("odbior slave'a\n");
//         ret=spi_slave_transmit(RCV_HOST, &t_slave, portMAX_DELAY);
//         if(ret==ESP_OK) printf("udana\n");
//         //spi_slave_transmit does not return until the master has done a transmission, so by here we have sent our data and
//         //received data from the master. Print it.
//         printf("Received(slave): %s\n", recvbuf_slave);
//         n++;
//     }

//     //Never reached.
//     ret=spi_bus_remove_device(handle);
//     assert(ret==ESP_OK);


// }



// // ----------------------------------------------------------------

// // /* SPI Slave example, sender (uses SPI master driver)

// //    This example code is in the Public Domain (or CC0 licensed, at your option.)

// //    Unless required by applicable law or agreed to in writing, this
// //    software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
// //    CONDITIONS OF ANY KIND, either express or implied.
// // */
// // #include <stdio.h>
// // #include <stdint.h>
// // #include <stddef.h>
// // #include <string.h>

// // #include "freertos/FreeRTOS.h"
// // #include "freertos/task.h"
// // #include "freertos/semphr.h"
// // #include "freertos/queue.h"

// // #include "lwip/sockets.h"
// // #include "lwip/dns.h"
// // #include "lwip/netdb.h"
// // #include "lwip/igmp.h"

// // #include "esp_wifi.h"
// // #include "esp_system.h"
// // #include "esp_event.h"
// // #include "nvs_flash.h"
// // #include "soc/rtc_periph.h"
// // #include "driver/spi_master.h"
// // #include "esp_log.h"
// // #include "esp_spi_flash.h"

// // #include "driver/gpio.h"
// // #include "esp_intr_alloc.h"


// /*
// SPI sender (master) example.

// This example is supposed to work together with the SPI receiver. It uses the standard SPI pins (MISO, MOSI, SCLK, CS) to
// transmit data over in a full-duplex fashion, that is, while the master puts data on the MOSI pin, the slave puts its own
// data on the MISO pin.

// This example uses one extra pin: GPIO_HANDSHAKE is used as a handshake pin. The slave makes this pin high as soon as it is
// ready to receive/send data. This code connects this line to a GPIO interrupt which gives the rdySem semaphore. The main
// task waits for this semaphore to be given before queueing a transmission.
// */


// /*
// Pins in use. The SPI Master can use the GPIO mux, so feel free to change these if needed.
// */
// // #define GPIO_HANDSHAKE 2
// // #define GPIO_MOSI 12
// // #define GPIO_MISO 13
// // #define GPIO_SCLK 15
// // #define GPIO_CS 14

// // #ifdef CONFIG_IDF_TARGET_ESP32
// // #define SENDER_HOST HSPI_HOST
// // #define DMA_CHAN    2

// // #elif defined CONFIG_IDF_TARGET_ESP32S2
// // #define SENDER_HOST SPI2_HOST
// // #define DMA_CHAN    SENDER_HOST

// // #endif


// //The semaphore indicating the slave is ready to receive stuff.
// static xQueueHandle rdySem;

// /*
// This ISR is called when the handshake line goes high.
// */
// static void IRAM_ATTR gpio_handshake_isr_handler(void* arg)
// {
//     //Sometimes due to interference or ringing or something, we get two irqs after eachother. This is solved by
//     //looking at the time between interrupts and refusing any interrupt too close to another one.
//     static uint32_t lasthandshaketime;
//     uint32_t currtime=xthal_get_ccount();
//     uint32_t diff=currtime-lasthandshaketime;
//     if (diff<240000) return; //ignore everything <1ms after an earlier irq
//     lasthandshaketime=currtime;

//     //Give the semaphore.
//     BaseType_t mustYield=false;
//     xSemaphoreGiveFromISR(rdySem, &mustYield);
//     if (mustYield) portYIELD_FROM_ISR();
// }

// //Main application
// void app_main(void)
// {
//     // SPI master
//     esp_err_t ret;
//     spi_device_handle_t handle;

//     //Configuration for the SPI bus
//     spi_bus_config_t buscfg={
//         .mosi_io_num=GPIO_MOSI,
//         .miso_io_num=GPIO_MISO,
//         .sclk_io_num=GPIO_SCLK,
//         .quadwp_io_num=-1,
//         .quadhd_io_num=-1
//     };

//     //Configuration for the SPI device on the other side of the bus
//     spi_device_interface_config_t devcfg={
//         .command_bits=0,
//         .address_bits=0,
//         .dummy_bits=0,
//         .clock_speed_hz=5000000,
//         .duty_cycle_pos=128,        //50% duty cycle
//         .mode=0,
//         .spics_io_num=GPIO_CS,
//         .cs_ena_posttrans=3,        //Keep the CS low 3 cycles after transaction, to stop slave from missing the last bit when CS has less propagation delay than CLK
//         .queue_size=3
//     };

//     //GPIO config for the handshake line.
//     gpio_config_t io_conf={
//         .intr_type=GPIO_PIN_INTR_POSEDGE,
//         .mode=GPIO_MODE_INPUT,
//         .pull_up_en=1,
//         .pin_bit_mask=(1<<GPIO_HANDSHAKE)
//     };

//     int n=0;
//     char sendbuf[128] = {0};
//     char recvbuf[128] = {0};
//     spi_transaction_t t;
//     memset(&t, 0, sizeof(t));

//     //Create the semaphore.
//     rdySem=xSemaphoreCreateBinary();

//     //Set up handshake line interrupt.
//     gpio_config(&io_conf);
//     gpio_install_isr_service(0);
//     gpio_set_intr_type(GPIO_HANDSHAKE, GPIO_PIN_INTR_POSEDGE);
//     gpio_isr_handler_add(GPIO_HANDSHAKE, gpio_handshake_isr_handler, NULL);

//     //Initialize the SPI bus and add the device we want to send stuff to.
//     ret=spi_bus_initialize(SENDER_HOST, &buscfg, DMA_CHAN);
//     assert(ret==ESP_OK);
//     ret=spi_bus_add_device(SENDER_HOST, &devcfg, &handle);
//     assert(ret==ESP_OK);

//     //Assume the slave is ready for the first transmission: if the slave started up before us, we will not detect
//     //positive edge on the handshake line.
//     xSemaphoreGive(rdySem);

//     while(1) {
//         int res = snprintf(sendbuf, sizeof(sendbuf),
//                 "Sender, transmission no. %04i. Last time, I received: \"%s\"", n, recvbuf);
//         if (res >= sizeof(sendbuf)) {
//             printf("Data truncated\n");
//         }
//         t.length=sizeof(sendbuf)*8;
//         t.tx_buffer=sendbuf;
//         t.rx_buffer=recvbuf;
//         //Wait for slave to be ready for next byte before sending
//         xSemaphoreTake(rdySem, portMAX_DELAY); //Wait until slave is ready
//         ret=spi_device_transmit(handle, &t);
//         printf("Received: %s\n", recvbuf);
//         n++;
//     }

//     //Never reached.
//     ret=spi_bus_remove_device(handle);
//     assert(ret==ESP_OK);
// }

#include "SPI_task.h"


//The semaphore indicating the slave is ready to receive stuff.
xQueueHandle rdySem = NULL;

/*
This ISR is called when the handshake line goes high.
*/
void IRAM_ATTR gpio_handshake_isr_handler(void* arg)
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
        int res = snprintf(sendbuf_master, sizeof(sendbuf_master), "Licznik od mastera: %i", n);
                
        if (res >= sizeof(sendbuf_master)) {
            printf("Data truncated\n");
        }
        t_master.length=sizeof(sendbuf_master)*8;
        t_master.tx_buffer=sendbuf_master;
        t_master.rx_buffer=recvbuf_master;
        //Wait for slave to be ready for next byte before sending
        xSemaphoreTake(rdySem, portMAX_DELAY); //Wait until slave is ready
        ret_m=spi_device_transmit(*((spi_device_handle_t*)handle_ptr), &t_master);

        // if(ret_m==ESP_OK) printf("\ttransmisja udana\n");
        printf("(Master): %s\n", recvbuf_master);
        n++;
        vTaskDelay(300);
    }
}

// SPI slave task
void spi_slave_task( void * spi_s_t )
{
    static int n;
    static char sendbuf_slave[129];
    static char recvbuf_slave[129];
    static spi_slave_transaction_t t_slave;
    for( ;; )
    {
        // SPI slave
        pomiar = rand() % 100 + 1; // tutaj powinno być odebranie wyniku

        esp_err_t ret;
        //Clear receive buffer, set send buffer to something sane
        memset(recvbuf_slave, 0xA5, 129);
        sprintf(sendbuf_slave, "data recv = %d", pomiar);
                

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
        
        ret=spi_slave_transmit(RCV_HOST, &t_slave, portMAX_DELAY);
        // if(ret==ESP_OK) printf("\ttransmisja udana\n");
        //spi_slave_transmit does not return until the master has done a transmission, so by here we have sent our data and
        //received data from the master. Print it.
        // printf("(Slave): %s\n", recvbuf_slave);
        printf("(Slave): send data = %d\n", pomiar);
        n++;
        vTaskDelay(300);
    }
}

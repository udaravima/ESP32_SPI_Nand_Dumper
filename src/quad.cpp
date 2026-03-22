#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/uart.h"
#include "driver/gpio.h"

#define PIN_NUM_CS   5
#define PIN_NUM_CLK  18
#define PIN_NUM_D0   23 // SI  / IO0
#define PIN_NUM_D1   19 // SO  / IO1
#define PIN_NUM_D2   22 // WP# / IO2
#define PIN_NUM_D3   21 // HOLD# / IO3

#define PAGE_SIZE    2112
#define TOTAL_BLOCKS 1024
#define PAGES_PER_BLK 64

spi_device_handle_t spi;

// Helper to send 1-line standard SPI commands (like 13h, 0Fh, 1Fh)
void send_cmd_1_line(uint8_t cmd, uint32_t addr, uint8_t addr_len, uint8_t *rx_data, int rx_len) {
    spi_transaction_t t = {
        .flags = 0,
        .cmd = cmd,
        .addr = addr,
        .length = 0, // TX length
        .rxlength = rx_len * 8,
        .rx_buffer = rx_data
    };
    if (addr_len > 0) t.flags |= SPI_TRANS_USE_RXDATA; // Simplified for brevity
    spi_device_polling_transmit(spi, &t);
}

void app_main(void) {
    // 1. Initialize High-Speed UART (2,000,000 baud) for PC dumping
    uart_config_t uart_config = {
        .baud_rate = 2000000,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE
    };
    uart_param_config(UART_NUM_0, &uart_config);
    uart_driver_install(UART_NUM_0, PAGE_SIZE * 2, 0, 0, NULL, 0);

    // 2. Configure the Hardware SPI Bus for Quad Mode
    spi_bus_config_t buscfg = {
        .miso_io_num = PIN_NUM_D1,
        .mosi_io_num = PIN_NUM_D0,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = PIN_NUM_D2, // Hardware controlled WP
        .quadhd_io_num = PIN_NUM_D3, // Hardware controlled HOLD
        .max_transfer_sz = 4096
    };
    
    // Half-duplex is REQUIRED for changing pin directions mid-transaction
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 10 * 1000 * 1000, // 10 MHz
        .mode = 0,
        .spics_io_num = PIN_NUM_CS,
        .queue_size = 1,
        .flags = SPI_DEVICE_HALFDUPLEX, 
        .command_bits = 8,
        .address_bits = 24
    };

    spi_bus_initialize(VSPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
    spi_bus_add_device(VSPI_HOST, &devcfg, &spi);

    // 3. Enable Quad Mode on the NAND (Set Feature B0h, flip bit 0) [cite: 351, 578]
    // Default B0h is usually 10h (ECC enabled). 10h | 01h (QE) = 11h.
    uint8_t qe_payload[1] = {0x11}; 
    spi_transaction_t t_qe = {
        .cmd = 0x1F, // Set Feature
        .addr = 0xB0, // OTP Register [cite: 351]
        .address_bits = 8,
        .length = 8,
        .tx_buffer = qe_payload
    };
    spi_device_polling_transmit(spi, &t_qe);

    // Buffer for our 4-lane data catch
    uint8_t *page_buffer = heap_caps_malloc(PAGE_SIZE, MALLOC_CAP_DMA);

    // 4. The Quad-Extraction Loop
    for (int block = 0; block < TOTAL_BLOCKS; block++) {
        for (int page = 0; page < PAGES_PER_BLK; page++) {
            
            uint32_t row_addr = (block << 6) | page;

            // STEP A: Stage data (13h) - Standard 1-bit mode
            spi_transaction_t t_stage = {
                .cmd = 0x13,
                .addr = row_addr,
                .address_bits = 24,
                .length = 0
            };
            spi_device_polling_transmit(spi, &t_stage);

            // Wait for OIP (Operation In Progress) to clear
            uint8_t status = 0x01;
            while(status & 0x01) {
                spi_transaction_t t_status = {
                    .cmd = 0x0F,
                    .addr = 0xC0,
                    .address_bits = 8,
                    .rxlength = 8,
                    .flags = SPI_TRANS_USE_RXDATA
                };
                spi_device_polling_transmit(spi, &t_status);
                status = t_status.rx_data[0];
            }

            // STEP B: The 1-1-4 Quad Read (6Bh) [cite: 196]
            spi_transaction_t t_read = {
                .cmd = 0x6B,
                .addr = 0x000000, // 24 bits: Col addr + Dummy byte 
                .address_bits = 24,
                .rxlength = PAGE_SIZE * 8, // Receive 2112 bytes
                .rx_buffer = page_buffer,
                // THIS IS THE MAGIC FLAG: Forces the silicon into 4-lane RX mode
                .flags = SPI_TRANS_MULTILINE_RX 
            };
            spi_device_polling_transmit(spi, &t_read);

            // STEP C: Blast to PC via UART
            uart_write_bytes(UART_NUM_0, (const char*)page_buffer, PAGE_SIZE);
        }
    }
}
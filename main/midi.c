#include "alles.h"

// midi spec
// one device can have a midi port optionally
// it can act as a broadcast channel (and also play its own audio)
// meaning, i send a message like channel 1, program 23, then note on, channel 1, etc 
// channel == booted ID. channel 0 is all synths. channel 1 is only ID = 0, and so on
// if people really want to address more than 16 synths over MIDI make a 2nd control bank
// but i assume bigger meshes are controlled via UDP only in practice 
// program == sound -- SINE, SQUARE, SAW, TRIANGLE, NOISE, (FM), KS
// bank 0 is default set here ^
// bank 1 is FM bank 0 and so on 

extern void serialize_event(struct event e, uint16_t client);
extern void play_event(struct event e);
extern struct event default_event();

QueueHandle_t uart_queue;
uint8_t midi_voice = 0;
uint8_t program_bank = 0;
uint8_t program = 0;
uint8_t note_map[VOICES];


// TODO don't schedule notes to me, or ignore them 
void callback_midi_message_received(uint8_t source, uint16_t timestamp, uint8_t midi_status, uint8_t *remaining_message, size_t len) {
    // uart is 1 if this came in through uart, 0 if ble
    //printf("got midi message source %d ts %d status %d -- ", source, timestamp, midi_status);
    for(int i=0;i<len;i++) printf("%d ", remaining_message[i]);
    printf("\n");
    uint8_t channel = midi_status & 0x0F;
    uint8_t message = midi_status & 0xF0;
    if(len > 0) {
        uint8_t data1 = remaining_message[0];
        if(message == 0x90) {  // note on 
            uint8_t data2 = remaining_message[1];
            struct event e = default_event();
            e.time = esp_timer_get_time() / 1000; // looks like BLE timestamp rolls over within 10s
            if(program_bank > 0) {
                e.wave = FM;
                e.patch = ((program_bank-1) * 128) + program;
            } else {
                e.wave = program;
            }
            e.voice = midi_voice;
            e.midi_note = data1;
            e.velocity = data2;
            e.amp = 0.1; // for now
            note_map[midi_voice] = data1;
            if(channel == 0) {
                serialize_event(e,256);
            } else {
                serialize_event(e, channel - 1);
            }
            midi_voice = (midi_voice + 1) % (VOICES);

        } else if (message == 0x80) { 
            // note off
            uint8_t data2 = remaining_message[1];
            // for now, only handle broadcast note offs... will have to refactor if i go down this path farther
            // assume this is the new envelope command we keep putting off-- like "$e30a0" where e is an event number 
            for(uint8_t v=0;v<VOICES;v++) {
                if(note_map[v] == data1) {
                    struct event e = default_event();
                    e.amp = 0;
                    e.voice = v;
                    e.time = esp_timer_get_time() / 1000;
                    e.velocity = data2; // note off velocity, not used... yet
                    serialize_event(e, 256);
                }
            }
                        
        } else if(message == 0xC0) { // program change 
            program = data1;
        } else if(message == 0xB0) {
            // control change
            uint8_t data2 = remaining_message[1];
            // Bank select for program change
            if(data1 == 0x00) { 
                program_bank = data2;
            }
            // feedback
            // duty cycle
            // pitch bend (?) 
            // amplitude / volume
        }
    }
}



void read_midi_uart() {
    printf("UART MIDI running on core %d\n", xPortGetCoreID());
    const int uart_num = UART_NUM_2;
    uint8_t data[128];
    size_t length = 0;
    while(1) {
        // Sleep 5ms to wait to get more MIDI data and avoid starving other threads
        // I increased RTOS clock rate from 100hz to 500hz to go down to a 5ms delay here
        // https://www.esp32.com/viewtopic.php?t=7554
        vTaskDelay(5 / portTICK_PERIOD_MS);
        ESP_ERROR_CHECK(uart_get_buffered_data_len(uart_num, (size_t*)&length));
        if(length) {
            length = uart_read_bytes(uart_num, data, length, 100);
            if(length > 1) {
                callback_midi_message_received(1,esp_timer_get_time() / 1000, data[0], data+1, length-1);
            }
        }  // end was there any bytes at all 
    } // end loop forever
}

TaskHandle_t read_midi_uart_task = NULL;

void midi_deinit() {
    // Shutdown blemidi somehow?
    blemidi_deinit();
    vTaskDelete(read_midi_uart_task);


}
void midi_init() {
    // Setup UART2 and BLE to listen for MIDI messages 
    const int uart_num = UART_NUM_2;
    uart_config_t uart_config = {
        .baud_rate = 31250,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 122,
    };

    for(uint8_t v=0;v<VOICES;v++) {
        note_map[v] = 0;
    }
    // Configure UART parameters
    ESP_ERROR_CHECK(uart_param_config(uart_num, &uart_config));
    // TX, RX, CTS/RTS -- Only care about RX here, pin 19 for now
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM_2, 18, 19, 21, 5));

    const int uart_buffer_size = (1024 * 2);
    // Install UART driver using an event queue here
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_2, uart_buffer_size, \
                                          uart_buffer_size, 10, &uart_queue, 0));

    xTaskCreatePinnedToCore(&read_midi_uart, "read_midi_task", 4096, NULL, 1, &read_midi_uart_task, 1);

    int status = blemidi_init(callback_midi_message_received);
    if( status < 0 ) {
      ESP_LOGE(BLEMIDI_TAG, "BLE MIDI Driver returned status=%d", status);
    } else {
      ESP_LOGI(BLEMIDI_TAG, "BLE MIDI Driver initialized successfully");
    }

}


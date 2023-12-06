#include "stm32f10x.h"
#include "nvic.h"
#include "uart.h"
int main(void) {
    nvic_init();
    uart_init();
    uart_send_string("Hello World\r\n");

    while (true) {

    }
    return 0;
}
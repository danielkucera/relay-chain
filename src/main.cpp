#include <ch32v00x.h>

#include <debug.h>

#include "relay_protocol.h"
#include "status_led.h"

/* PC2 */
#define STATUS_GPIO_PORT    GPIOC
#define STATUS_GPIO_PIN     GPIO_Pin_2

#define STATUS_CLOCK        RCC_APB2Periph_GPIOC

#define RELAY_GPIO_PORT     GPIOA
#define RELAY_GPIO_PIN      GPIO_Pin_2
#define RELAY_CLOCK         RCC_APB2Periph_GPIOA

#define UART_BAUD_RATE      9600

extern "C" void NMI_Handler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
extern "C" void HardFault_Handler(void) __attribute__((interrupt("WCH-Interrupt-fast")));

void uartBeginRx(uint32_t baudRate)
{
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOD | RCC_APB2Periph_USART1, ENABLE);

    GPIO_InitTypeDef gpioInit = {0};
    gpioInit.GPIO_Pin = GPIO_Pin_6;
    gpioInit.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_Init(GPIOD, &gpioInit);

    USART_InitTypeDef uartInit = {0};
    uartInit.USART_BaudRate = baudRate;
    uartInit.USART_WordLength = USART_WordLength_8b;
    uartInit.USART_StopBits = USART_StopBits_1;
    uartInit.USART_Parity = USART_Parity_No;
    uartInit.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    uartInit.USART_Mode = USART_Mode_Rx;
    USART_Init(USART1, &uartInit);
    USART_Cmd(USART1, ENABLE);
}

// Call after uartBeginRx to enable transmission without disturbing reception.
void uartBeginTx()
{
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOD, ENABLE);

    GPIO_InitTypeDef gpioInit = {0};
    gpioInit.GPIO_Pin = GPIO_Pin_5;
    gpioInit.GPIO_Mode = GPIO_Mode_AF_PP;
    gpioInit.GPIO_Speed = GPIO_Speed_30MHz;
    GPIO_Init(GPIOD, &gpioInit);

    USART1->CTLR1 |= USART_Mode_Tx;
}

int main(void)
{
    #ifdef NVIC_PriorityGroup_2
	NVIC_PriorityGroupConfig(NVIC_PriorityGroup_2);
#else
	NVIC_PriorityGroupConfig(NVIC_PriorityGroup_1);
#endif
    SystemCoreClockUpdate();
    
    RCC_APB2PeriphClockCmd(RELAY_CLOCK, ENABLE);
    GPIO_WriteBit(RELAY_GPIO_PORT, RELAY_GPIO_PIN, BitAction::Bit_RESET);
    GPIO_InitTypeDef relayGpioInit = {0};
    relayGpioInit.GPIO_Pin = RELAY_GPIO_PIN;
    relayGpioInit.GPIO_Mode = GPIO_Mode_Out_PP;
    relayGpioInit.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(RELAY_GPIO_PORT, &relayGpioInit);

    RCC_APB2PeriphClockCmd(STATUS_CLOCK, ENABLE);
    GPIO_InitTypeDef ledGpioInit = {0};
    ledGpioInit.GPIO_Pin = STATUS_GPIO_PIN;
    ledGpioInit.GPIO_Mode = GPIO_Mode_Out_PP;
    ledGpioInit.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(STATUS_GPIO_PORT, &ledGpioInit);

    SysTick->CTLR = 0;
    SysTick->CNT = 0;
    SysTick->CTLR = 1;
    const uint32_t ticksPerMs = SystemCoreClock / 8 / 1000;
    uint32_t lastTicks = 0;
    uint32_t fractionalTicks = 0;
    uint32_t now = 0;
    bool fault = false;
    bool txEnabled = false;
    bool forwardingFailed = false;
    uint8_t txBuffer[32] = {};
    uint8_t txHead = 0;
    uint8_t txTail = 0;
    RelayNode node;
    StatusLed statusLed;
    ResetAfterTransmit reset;

    uartBeginRx(UART_BAUD_RATE);
    while (1)
    {
        bool frameReceived = false;
        const uint32_t ticks = SysTick->CNT;
        const uint32_t elapsedTicks = ticks - lastTicks;
        lastTicks = ticks;
        now += elapsedTicks / ticksPerMs;
        fractionalTicks += elapsedTicks % ticksPerMs;
        if (fractionalTicks >= ticksPerMs) {
            fractionalTicks -= ticksPerMs;
            ++now;
        }

        const uint16_t status = USART1->STATR;
        if (!reset.waiting() && (status & (USART_FLAG_ORE | USART_FLAG_NE | USART_FLAG_FE | USART_FLAG_PE))) {
            USART_ReceiveData(USART1);
            node.resetParser();
            fault = true;
            forwardingFailed = false;
        } else if (!reset.waiting() && (status & USART_FLAG_RXNE)) {
            const uint8_t byte = USART_ReceiveData(USART1);
            const NodeResult result = node.receive(byte, now);
            frameReceived = result.frameComplete;
            if ((result.forward || result.enableTx) && !txEnabled) {
                uartBeginTx();
                txEnabled = true;
            }
            if (result.forward) {
                const uint8_t nextHead = (txHead + 1) % sizeof(txBuffer);
                if (nextHead == txTail) {
                    fault = true;
                    forwardingFailed = true;
                } else {
                    txBuffer[txHead] = result.forwardByte;
                    txHead = nextHead;
                }
            }
            if (result.error) {
                fault = true;
            }
            if (result.disableStatusLed) {
                statusLed.disable();
            }
            if (result.reset && !forwardingFailed) {
                reset.request();
            }
            if (result.frameComplete) {
                forwardingFailed = false;
            }
        }

        if (txHead != txTail && (USART1->STATR & USART_FLAG_TXE)) {
            USART_SendData(USART1, txBuffer[txTail]);
            txTail = (txTail + 1) % sizeof(txBuffer);
        }
        if (reset.ready(txHead == txTail, txEnabled, USART1->STATR & USART_FLAG_TC)) {
            NVIC_SystemReset();
            while (1) {}
        }

        node.update(now);
        GPIO_WriteBit(RELAY_GPIO_PORT, RELAY_GPIO_PIN,
                      node.relayEnabled() ? Bit_SET : Bit_RESET);
        const bool statusOn = statusLed.update(now, fault, frameReceived, txEnabled);
        GPIO_WriteBit(STATUS_GPIO_PORT, STATUS_GPIO_PIN, statusOn ? Bit_SET : Bit_RESET);
    }
}

void NMI_Handler(void) {}
void HardFault_Handler(void)
{
    while (1)
    {
    }
}

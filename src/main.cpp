#include <ch32v00x.h>

#include <debug.h>

/* PC2 */
#define BLINKY_GPIO_PORT    GPIOC
#define BLINKY_GPIO_PIN     GPIO_Pin_2

#define BLINKY_CLOCK        RCC_APB2Periph_GPIOC

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

class LEDBlinker {
private:
    GPIO_TypeDef* gpio_port;
    uint32_t gpio_pin;
    uint32_t rcc_clock;
public:
    LEDBlinker(GPIO_TypeDef* gpio_port, uint32_t gpio_pin, uint32_t rcc_clock) {
        this->gpio_port = gpio_port;
        this->gpio_pin = gpio_pin;
        this->rcc_clock = rcc_clock;
    }

    void begin() {
        RCC_APB2PeriphClockCmd(this->rcc_clock, ENABLE);
        GPIO_InitTypeDef GPIO_InitStructure = {0};
        GPIO_InitStructure.GPIO_Pin = this->gpio_pin;
        GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
        GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
        GPIO_Init(this->gpio_port, &GPIO_InitStructure);
    }

    void blink(int time_ms) {
        GPIO_WriteBit(this->gpio_port, this->gpio_pin, BitAction::Bit_SET);
        Delay_Ms(time_ms);
        GPIO_WriteBit(this->gpio_port, this->gpio_pin, BitAction::Bit_RESET);
        Delay_Ms(time_ms);
    }
};

LEDBlinker blinker(BLINKY_GPIO_PORT, BLINKY_GPIO_PIN, BLINKY_CLOCK);

int main(void)
{
    #ifdef NVIC_PriorityGroup_2
	NVIC_PriorityGroupConfig(NVIC_PriorityGroup_2);
#else
	NVIC_PriorityGroupConfig(NVIC_PriorityGroup_1);
#endif
    SystemCoreClockUpdate();
    Delay_Init();
    uartBeginRx(UART_BAUD_RATE);
    
    RCC_APB2PeriphClockCmd(RELAY_CLOCK, ENABLE);
    GPIO_WriteBit(RELAY_GPIO_PORT, RELAY_GPIO_PIN, BitAction::Bit_RESET);
    GPIO_InitTypeDef relayGpioInit = {0};
    relayGpioInit.GPIO_Pin = RELAY_GPIO_PIN;
    relayGpioInit.GPIO_Mode = GPIO_Mode_Out_PP;
    relayGpioInit.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(RELAY_GPIO_PORT, &relayGpioInit);

    blinker.begin();
    while (1)
    {
        blinker.blink(1000);
    }
}

void NMI_Handler(void) {}
void HardFault_Handler(void)
{
    while (1)
    {
    }
}

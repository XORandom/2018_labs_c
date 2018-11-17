#include "stm32f10x.h"

// Адрес во Flash для сохранения настроек
#define FLASH_USER_START_ADDR   0x0800FC00

// Структура для хранения конфигурации
typedef struct {
    uint32_t frequency;  // Частота в МГц
    uint8_t power_state; // Состояние питания ADF41020
} Config_t;

// Глобальные переменные
Config_t config = {100, 1}; // Значения по умолчанию: 100 МГц, включено
volatile uint8_t uart_rx_buffer[32];
volatile uint8_t uart_rx_index = 0;

// Прототипы функций
void RCC_Config(void);
void GPIO_Config(void);
void UART_Config(void);
void SPI_Config(void);
void ADC_Config(void);
void ADF41020_Init(void);
void ADF41020_SetFrequency(uint32_t freq_mhz);
void ADF41020_PowerDown(uint8_t state);
uint8_t ADF41020_GetLockDetect(void);
float Get_Temperature(void);
void Flash_SaveConfig(void);
void Flash_LoadConfig(void);

// Инициализация тактирования
void RCC_Config(void) {
    // Включаем внешний кварц 8 МГц
    RCC->CR |= RCC_CR_HSEON;
    while(!(RCC->CR & RCC_CR_HSERDY));
    
    // Настройка PLL: 8 МГц * 9 = 72 МГц
    RCC->CFGR = RCC_CFGR_PLLSRC_HSE | RCC_CFGR_PLLMULL9;
    RCC->CR |= RCC_CR_PLLON;
    while(!(RCC->CR & RCC_CR_PLLRDY));
    
    // Переключаемся на PLL как источник системной частоты
    RCC->CFGR |= RCC_CFGR_SW_PLL;
    while((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL);
    
    // Включаем тактирование периферии
    RCC->APB2ENR |= RCC_APB2ENR_IOPAEN | RCC_APB2ENR_IOPCEN | 
                    RCC_APB2ENR_USART1EN | RCC_APB2ENR_SPI1EN | 
                    RCC_APB2ENR_ADC1EN;
}

// Конфигурация GPIO
void GPIO_Config(void) {
    GPIOA->CRL = 0x8B348444;  // PA2 - UART TX (AF PP), PA3 - UART RX (IN), 
                             // PA5 - SPI CLK (PP), PA6 - IN, PA7 - SPI DATA (PP)
    GPIOC->CRL = 0x44004400;  // PC4, PC5 - ADF41020 LE, CE (PP)
    GPIOD->CRL = 0x00000004;  // PD0 - HSE
}

// Конфигурация UART
void UART_Config(void) {
    USART1->BRR = 0x271;  // 115200 бод при 72 МГц
    USART1->CR1 = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE | USART_CR1_RXNEIE;
    NVIC_EnableIRQ(USART1_IRQn);
}

// Конфигурация SPI
void SPI_Config(void) {
    SPI1->CR1 = SPI_CR1_MSTR | SPI_CR1_BR_1 | SPI_CR1_SSM | 
                SPI_CR1_SSI | SPI_CR1_SPE;  // Master, f/8, soft NSS
}

// Конфигурация АЦП для температуры
void ADC_Config(void) {
    ADC1->SMPR1 = ADC_SMPR1_SMP16_2;  // 55.5 циклов выборки
    ADC1->SQR3 = 16;                  // Канал 16 (температура)
    ADC1->CR2 = ADC_CR2_ADON | ADC_CR2_TSVREFE;  // Включение АЦП и датчика
}

// Инициализация ADF41020
void ADF41020_Init(void) {
    // Инициализация регистров
    uint32_t init_reg = (1 << 23) | (0xA << 14) | (1 << 13) | (1 << 3);  // CP 2.5mA, PD positive
    GPIOC->BSRR = GPIO_BSRR_BR4;  // LE low
    SPI1->DR = init_reg >> 16;
    while(!(SPI1->SR & SPI_SR_TXE));
    SPI1->DR = init_reg & 0xFFFF;
    while(!(SPI1->SR & SPI_SR_TXE));
    GPIOC->BSRR = GPIO_BSRR_BS4;  // LE high
}

// Установка частоты ADF41020
void ADF41020_SetFrequency(uint32_t freq_mhz) {
    uint32_t n = freq_mhz / 100;  // Делитель для 100 МГц опоры
    uint32_t freq_reg = (n << 8) | 1;  // R регистр
    GPIOC->BSRR = GPIO_BSRR_BR4;
    SPI1->DR = freq_reg >> 16;
    while(!(SPI1->SR & SPI_SR_TXE));
    SPI1->DR = freq_reg & 0xFFFF;
    while(!(SPI1->SR & SPI_SR_TXE));
    GPIOC->BSRR = GPIO_BSRR_BS4;
    config.frequency = freq_mhz;
}

// Управление питанием ADF41020
void ADF41020_PowerDown(uint8_t state) {
    uint32_t pd_reg = (state << 22) | 2;  // PD бит и контрольный бит
    GPIOC->BSRR = GPIO_BSRR_BR4;
    SPI1->DR = pd_reg >> 16;
    while(!(SPI1->SR & SPI_SR_TXE));
    SPI1->DR = pd_reg & 0xFFFF;
    while(!(SPI1->SR & SPI_SR_TXE));
    GPIOC->BSRR = GPIO_BSRR_BS4;
    config.power_state = state;
}

// Чтение Lock Detect
uint8_t ADF41020_GetLockDetect(void) {
    return (GPIOA->IDR & GPIO_IDR_IDR6) ? 1 : 0;
}

// Измерение температуры
float Get_Temperature(void) {
    ADC1->CR2 |= ADC_CR2_ADON;  // Старт преобразования
    while(!(ADC1->SR & ADC_SR_EOC));
    uint16_t adc_value = ADC1->DR;
    // Преобразование в температуру (примерная калибровка)
    return ((1430 - adc_value) * 3.3 / 4096) / 0.0043 + 25;
}

// Сохранение во Flash
void Flash_SaveConfig(void) {
    FLASH_Unlock();
    FLASH_ErasePage(FLASH_USER_START_ADDR);
    FLASH_ProgramWord(FLASH_USER_START_ADDR, config.frequency);
    FLASH_ProgramWord(FLASH_USER_START_ADDR + 4, config.power_state);
    FLASH_Lock();
}

// Загрузка из Flash
void Flash_LoadConfig(void) {
    config.frequency = *(uint32_t*)FLASH_USER_START_ADDR;
    config.power_state = *(uint32_t*)(FLASH_USER_START_ADDR + 4);
    if(config.frequency == 0xFFFFFFFF) config.frequency = 100;  // Default
    if(config.power_state == 0xFF) config.power_state = 1;
}

// Обработчик прерывания UART
void USART1_IRQHandler(void) {
    if(USART1->SR & USART_SR_RXNE) {
        char c = USART1->DR;
        if(c == '\n' || c == '\r') {
            uart_rx_buffer[uart_rx_index] = 0;
            // Обработка команд
            if(strncmp((char*)uart_rx_buffer, "SET_FREQ=", 9) == 0) {
                uint32_t freq = atoi((char*)uart_rx_buffer + 9);
                ADF41020_SetFrequency(freq);
                Flash_SaveConfig();
            }
            else if(strcmp((char*)uart_rx_buffer, "GET_FREQ") == 0) {
                printf("Frequency: %lu MHz\r\n", config.frequency);
            }
            else if(strcmp((char*)uart_rx_buffer, "POWER_DOWN") == 0) {
                ADF41020_PowerDown(1);
                Flash_SaveConfig();
            }
            else if(strcmp((char*)uart_rx_buffer, "POWER_UP") == 0) {
                ADF41020_PowerDown(0);
                Flash_SaveConfig();
            }
            else if(strcmp((char*)uart_rx_buffer, "GET_LOCK") == 0) {
                printf("Lock Detect: %d\r\n", ADF41020_GetLockDetect());
            }
            else if(strcmp((char*)uart_rx_buffer, "GET_TEMP") == 0) {
                printf("Temperature: %.1f C\r\n", Get_Temperature());
            }
            uart_rx_index = 0;
        }
        else if(uart_rx_index < 31) {
            uart_rx_buffer[uart_rx_index++] = c;
        }
    }
}

int main(void) {
    RCC_Config();
    GPIO_Config();
    UART_Config();
    SPI_Config();
    ADC_Config();
    
    Flash_LoadConfig();
    ADF41020_Init();
    ADF41020_SetFrequency(config.frequency);
    ADF41020_PowerDown(config.power_state);
    
    while(1) {
        __WFI();  // Ожидание прерываний
    }
}

// Простая реализация printf через UART
int __io_putchar(int ch) {
    while(!(USART1->SR & USART_SR_TXE));
    USART1->DR = ch;
    return ch;
}
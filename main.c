#include <avr/io.h>
#include <util/delay.h>
#include <avr/interrupt.h>
#include <math.h>
#include <stdlib.h> // Для strtol
#include <string.h> // Для strlen

#define F_CPU 16000000UL
#define BAUD 9600
#define UBRR_VAL (F_CPU / 16 / BAUD - 1)

// Глобальные переменные
volatile int8_t phase_counter = 0;
volatile int motor_state = 0;
volatile int steps_remaining = 0;
volatile int step_count_constant = 1000;
volatile int motor_dir = 0;
volatile uint8_t motor_running = 0;
volatile uint32_t steps_taken = 0;
volatile uint32_t last_steps_between_presses = 0;
volatile uint8_t last_button_pressed = 0;

uint8_t phase[] = {0b0001, 0b0011, 0b0010, 0b0110, 0b0100, 0b1100, 0b1000, 0b1001};

// Инициализация UART
void uart_init(void) {
    UBRR0H = (UBRR_VAL >> 8);
    UBRR0L = UBRR_VAL;
    UCSR0B = (1 << RXEN0) | (1 << TXEN0);
    UCSR0C = (1 << UCSZ01) | (1 << UCSZ00);
    _delay_ms(200);
}

void uart_send_char(char c) {
    while (!(UCSR0A & (1 << UDRE0)));
    UDR0 = c;
}

void uart_send_string(const char *str) {
    while (*str) {
        uart_send_char(*str++);
    }
}

void uart_send_number(uint16_t number) {
    char buffer[6]; // Достаточно для 5 цифр + нуль-терминатор
    itoa(number, buffer, 10); // Преобразование числа в строку
    uart_send_string(buffer);
    uart_send_string("\r\n");
}

// Чтение строки из UART. Возвращает 1, если строка пустая, 0 - если нет.
uint8_t uart_read_string(char *buffer, uint8_t max_length) {
    uint8_t i = 0;
    char c;

    while (i < max_length - 1) {
        while (!(UCSR0A & (1 << RXC0))); // Ждем символ
        c = UDR0;

        if (c == '\r' || c == '\n') {
            break; // Конец строки
        }

        buffer[i++] = c;
        uart_send_char(c); // Эхо обратно
    }

    buffer[i] = '\0'; // Нуль-терминатор
    uart_send_string("\r\n");

    return (i == 0); // Возвращаем 1, если строка пустая
}

// Преобразование строки в число с проверкой ошибок
long string_to_long(const char *str, int *error) {
    char *endptr;
    long val = strtol(str, &endptr, 10);

    if (*endptr != '\0' || str == endptr) {
        *error = 1; // Ошибка преобразования
        return 0;
    } else {
        *error = 0; // Нет ошибки
        return val;
    }
}


// Инициализация прерываний по кнопкам PD4 и PD5
void buttons_interrupt_init(void) {
    PCICR |= (1 << PCIE2);
    PCMSK2 |= (1 << PCINT20) | (1 << PCINT21);
}

// Обработчик прерываний на изменение состояния пинов PD4 и PD5
ISR(PCINT2_vect) {
    uint8_t pind_state = PIND;
    uint8_t button_pressed = 0;

    if (!(pind_state & (1 << PD4))) button_pressed = 4;
    else if (!(pind_state & (1 << PD5))) button_pressed = 5;

    if (button_pressed == 0) return;

    if (motor_running == 0) {
        motor_dir = !motor_dir;
        motor_running = 1;
        steps_taken = 0;
        last_button_pressed = button_pressed;

        motor_state = (motor_dir == 0) ? 1 : 2;
        steps_remaining = -1;
    } else {
        if (button_pressed != last_button_pressed) {
            // Останавливаем мотор независимо от motor_state
            motor_running = 0;
            motor_state = 0;
            steps_remaining = 0;
            PORTB = 0;

            last_steps_between_presses = steps_taken;
            last_button_pressed = 0;
        }
    }

    _delay_ms(50);
}

int main(void) {
    TCCR0B = (1 << CS00) | (1 << CS01);
    TIMSK0 = (1 << TOIE0);

    DDRB |= 0x0F;
    PORTB = 0;

    DDRD &= ~((1 << DDD4) | (1 << DDD5));
    PORTD |= (1 << PD4) | (1 << PD5);

    uart_init();
    buttons_interrupt_init();

    sei();

    char percent_str[4]; // Максимум 3 цифры + нуль-терминатор
    char command_str[2]; // Максимум 1 цифра + нуль-терминатор
    int percent_error, command_error;
    long percent, command;
    uint8_t percent_empty;

    while (1) {
        // Сбрасываем percent_empty в начале каждой итерации
        percent_empty = 0;

        // Считываем команду
        uart_send_string("Enter command (0-3): ");
        uart_read_string(command_str, sizeof(command_str));
        command = string_to_long(command_str, &command_error);

        if (command_error) {
            uart_send_string("Invalid command!\r\n");
            continue; // Начинаем цикл заново
        }

        // Если команда 3, не спрашиваем процент
        if (command == 3) {
            percent = 100; // Значение не важно, но нужно для логики ниже
            percent_error = 0;
            percent_empty = 1; // Считаем, что процент пустой
        } else {
            // Считываем процент
            uart_send_string("Enter percentage (0-100, leave empty for 100%): ");
            percent_empty = uart_read_string(percent_str, sizeof(percent_str));

            // Обрабатываем пустой ввод для процента
            if (percent_empty) {
                percent = 100;
                percent_error = 0; // Считаем, что ошибки нет
            } else {
                percent = string_to_long(percent_str, &percent_error);
            }

            if (!percent_empty && percent_error) {
                uart_send_string("Invalid percentage!\r\n");
                continue; // Начинаем цикл заново
            }
        }


        if (motor_running == 0 && last_steps_between_presses > 0) {
            if (percent >= 0 && percent <= 100 && (command == 1 || command == 2 || command == 3)) {
                uint16_t steps_to_move = (last_steps_between_presses * percent) / 100;
                if (steps_to_move == 0) steps_to_move = 1;

                motor_state = command;
                steps_remaining = steps_to_move;
                motor_running = 1;
                last_steps_between_presses = 0; // Сбрасываем здесь!
                continue;
            } else {
                uart_send_string("Invalid percentage or command!\r\n");
            }
        } else {
            if (command == 1 || command == 2) {
                if (percent > 100) percent = 100;
                steps_remaining = (step_count_constant * percent) / 100;
                if (steps_remaining == 0) steps_remaining = 1;
                motor_state = command;
                motor_running = 1;
                steps_taken = 0;
            } else if (command == 0) {
                motor_state = 0;
                steps_remaining = 0;
                motor_running = 0;
                PORTB = 0;
            } else if (command == 3) {
                motor_dir = 0;
                motor_state = 3;
                step_count_constant = 0;
                motor_running = 1;
                steps_taken = 0;
            } else {
                uart_send_string("Invalid command!\r\n");
                motor_state = 0;
                steps_remaining = 0;
                motor_running = 0;
                PORTB = 0;
            }
        }

        // Если мотор остановлен и были сохранены шаги, запрашиваем команду и процент заново
        if (!motor_running && last_steps_between_presses > 0) {
            continue; // Переходим к следующей итерации цикла, чтобы запросить ввод
        }
    }
}

ISR(TIMER0_OVF_vect) {
    if (motor_state == 1 || motor_state == 2) {
        if (steps_remaining != 0) {
            if (motor_state == 1) {
                phase_counter++;
                steps_taken++;
            } else {
                phase_counter--;
                steps_taken++;
            }

            if (phase_counter > 7) phase_counter = 0;
            else if (phase_counter < 0) phase_counter = 7;

            PORTB = phase[phase_counter];

            if (steps_remaining > 0) steps_remaining--;
        } else {
            motor_state = 0;
            motor_running = 0;
            PORTB = 0;
        }
    } else if (motor_state == 3) {
        if ((PIND & (1 << PD4)) || (PIND & (1 << PD5))) {
            motor_dir = !motor_dir;
        }

        if (motor_dir == 0)
            phase_counter++;
        else
            phase_counter--;

        if (phase_counter > 7) phase_counter = 0;
        else if (phase_counter < 0) phase_counter = 7;

        PORTB = phase[phase_counter];
        step_count_constant++;
    } else {
        PORTB = 0;
    }
}


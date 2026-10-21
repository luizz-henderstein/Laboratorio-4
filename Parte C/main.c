#define F_CPU 16000000UL
#include <avr/io.h>
#include <avr/interrupt.h>
#include <util/delay.h>
#include <stdint.h>

#define UBRR_9600 103
#define LCD_DIRECCION 0x27
#define UMBRAL_LDR_BAJO 420
#define UMBRAL_LDR_ALTO 480
#define ZONA_VENTANA 1
#define ZONA_MOVIMIENTO 2

typedef enum { MODO_AUTO, MODO_MANUAL } Modo;

static Modo modo = MODO_AUTO;
static uint8_t armada = 1;
static uint8_t alarma = 0;
static uint8_t silenciada = 0;
static uint8_t zonas = 0;

static volatile char rx_linea[24];
static volatile uint8_t rx_largo = 0;
static volatile uint8_t rx_lista = 0;

ISR(USART_RX_vect)
{
    uint8_t errores = UCSR0A;
    char c = UDR0;
    if (errores & ((1 << FE0) | (1 << DOR0) | (1 << UPE0))) return;
    if (rx_lista) return;
    if (c == '\r' || c == '\n') {
        if (rx_largo) {
            rx_linea[rx_largo] = 0;
            rx_lista = 1;
        }
    } else if (rx_largo < sizeof(rx_linea) - 1) {
        if (c >= 'a' && c <= 'z') c -= ('a' - 'A');
        rx_linea[rx_largo++] = c;
    } else {
        rx_largo = 0;
    }
}

static void uart_iniciar(void)
{
    UBRR0H = (uint8_t)(UBRR_9600 >> 8);
    UBRR0L = (uint8_t)UBRR_9600;
    UCSR0A = 0;
    UCSR0C = (1 << UCSZ01) | (1 << UCSZ00);
    UCSR0B = (1 << RXEN0) | (1 << TXEN0) | (1 << RXCIE0);
}

static void uart_caracter(char c)
{
    while (!(UCSR0A & (1 << UDRE0))) { }
    UDR0 = c;
}

static void uart_texto(const char *s)
{
    while (*s) uart_caracter(*s++);
}

static void uart_linea(const char *s)
{
    uart_texto(s);
    uart_texto("\r\n");
}

static void uart_numero(uint16_t n)
{
    char digitos[5];
    uint8_t i = 0;
    do {
        digitos[i++] = '0' + (n % 10);
        n /= 10;
    } while (n);
    while (i) uart_caracter(digitos[--i]);
}

static void uart_estado(void)
{
    uart_texto("ESTADO;MODO:");
    uart_texto(modo == MODO_AUTO ? "AUTO" : "MANUAL");
    uart_texto(";ARMADA:");
    uart_caracter(armada ? '1' : '0');
    uart_texto(";ALARMA:");
    uart_caracter(alarma ? '1' : '0');
    uart_texto(";SILENCIO:");
    uart_caracter(silenciada ? '1' : '0');
    uart_texto(";ZONA:");
    if (zonas == (ZONA_VENTANA | ZONA_MOVIMIENTO)) uart_texto("AMBAS");
    else if (zonas & ZONA_VENTANA) uart_texto("VENTANA");
    else if (zonas & ZONA_MOVIMIENTO) uart_texto("MOVIMIENTO");
    else uart_texto("NINGUNA");
    uart_texto("\r\n");
}

static void twi_inicio(void)
{
    TWCR = (1 << TWINT) | (1 << TWSTA) | (1 << TWEN);
    while (!(TWCR & (1 << TWINT))) { }
}

static void twi_dato(uint8_t dato)
{
    TWDR = dato;
    TWCR = (1 << TWINT) | (1 << TWEN);
    while (!(TWCR & (1 << TWINT))) { }
}

static void twi_fin(void)
{
    TWCR = (1 << TWINT) | (1 << TWSTO) | (1 << TWEN);
}

static void lcd_expansor(uint8_t dato)
{
    twi_inicio();
    twi_dato(LCD_DIRECCION << 1);
    twi_dato(dato | 0x08);
    twi_fin();
}

static void lcd_nibble(uint8_t nibble, uint8_t rs)
{
    uint8_t dato = (nibble & 0xF0) | rs;
    lcd_expansor(dato | 0x04);
    _delay_us(1);
    lcd_expansor(dato);
    _delay_us(50);
}

static void lcd_byte(uint8_t dato, uint8_t rs)
{
    lcd_nibble(dato, rs);
    lcd_nibble(dato << 4, rs);
}

static void lcd_comando(uint8_t comando)
{
    lcd_byte(comando, 0);
    if (comando == 0x01 || comando == 0x02) _delay_ms(2);
}

static void lcd_texto(const char *s)
{
    uint8_t i = 0;
    while (*s && i++ < 16) lcd_byte((uint8_t)*s++, 1);
}

static void lcd_mostrar(const char *arriba, const char *abajo)
{
    lcd_comando(0x01);
    lcd_comando(0x80);
    lcd_texto(arriba);
    lcd_comando(0xC0);
    lcd_texto(abajo);
}

static void lcd_iniciar(void)
{
    DDRC &= ~((1 << PC4) | (1 << PC5));
    PORTC |= (1 << PC4) | (1 << PC5);
    TWSR = 0;
    TWBR = 72;
    TWCR = (1 << TWEN);
    _delay_ms(50);
    lcd_nibble(0x30, 0);
    _delay_ms(5);
    lcd_nibble(0x30, 0);
    _delay_us(150);
    lcd_nibble(0x30, 0);
    lcd_nibble(0x20, 0);
    lcd_comando(0x28);
    lcd_comando(0x0C);
    lcd_comando(0x06);
    lcd_comando(0x01);
}

static void lcd_estado(void)
{
    const char *arriba = modo == MODO_AUTO ? "AUTO ARMADA" :
                         (armada ? "MANUAL ARMADA" : "MANUAL DESARM");
    const char *abajo;
    if (alarma && silenciada) abajo = "ALARMA EN SILENC";
    else if (alarma && (zonas & ZONA_VENTANA)) abajo = "ALARMA VENTANA";
    else if (alarma && (zonas & ZONA_MOVIMIENTO)) abajo = "ALARMA MOVIM.";
    else if (alarma) abajo = "ALARMA FORZADA";
    else if (zonas & ZONA_VENTANA) abajo = "Sensor ventana";
    else if (zonas & ZONA_MOVIMIENTO) abajo = "Sensor movim.";
    else abajo = "Sin alarma";
    lcd_mostrar(arriba, abajo);
}

static void adc_iniciar(void)
{
    ADMUX = (1 << REFS0);
    ADCSRA = (1 << ADEN) | (1 << ADPS2) | (1 << ADPS1) | (1 << ADPS0);
}

static uint16_t adc_leer(void)
{
    ADCSRA |= (1 << ADSC);
    while (ADCSRA & (1 << ADSC)) { }
    return ADC;
}

static void puertos_iniciar(void)
{

    DDRD &= ~((1 << PD4) | (1 << PD5) | (1 << PD6) | (1 << PD7));
    PORTD |= (1 << PD4) | (1 << PD5) | (1 << PD6) | (1 << PD7);
    DDRB |= (1 << PB0) | (1 << PB1) | (1 << PB2);
    PORTB &= ~((1 << PB0) | (1 << PB1) | (1 << PB2));
}

static void salidas_actualizar(void)
{
    if (armada) PORTB |= (1 << PB0); else PORTB &= ~(1 << PB0);
    if (alarma) PORTB |= (1 << PB1); else PORTB &= ~(1 << PB1);
    if (alarma && !silenciada) PORTB |= (1 << PB2);
    else PORTB &= ~(1 << PB2);
}

static uint8_t sensores_leer(void)
{
    static uint8_t ventana = 0;
    uint16_t muestra = adc_leer();
    uint8_t resultado = 0;
    if (muestra < UMBRAL_LDR_BAJO) ventana = 1;
    if (muestra > UMBRAL_LDR_ALTO) ventana = 0;
    if (ventana) resultado |= ZONA_VENTANA;
    if (!(PIND & (1 << PD4))) resultado |= ZONA_MOVIMIENTO;
    return resultado;
}

static void alarma_disparar(const char *zona)
{
    if (!armada) return;
    alarma = 1;
    silenciada = 0;
    uart_texto("ALARMA:ON;ZONA:");
    uart_linea(zona);
}

static void sensores_procesar(uint8_t nuevas_zonas)
{
    uint8_t aparecieron = nuevas_zonas & ~zonas;
    if (nuevas_zonas == zonas) return;
    zonas = nuevas_zonas;
    if (aparecieron & ZONA_VENTANA) alarma_disparar("VENTANA");
    if (aparecieron & ZONA_MOVIMIENTO) alarma_disparar("MOVIMIENTO");
    uart_estado();
    lcd_estado();
}

static uint8_t iguales(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == 0 && *b == 0;
}

static void comando_procesar(const char *s)
{
    if (iguales(s, "ESTADO")) {
        uart_estado();
        return;
    }
    if (iguales(s, "LECTURA")) {
        uart_texto("ADC_LDR:");
        uart_numero(adc_leer());
        uart_texto(";PRESENCIA:");
        uart_caracter((PIND & (1 << PD4)) ? '0' : '1');
        uart_texto("\r\n");
        return;
    }
    if (iguales(s, "AUTO")) {
        modo = MODO_AUTO;
        armada = 1;
        alarma = 0;
        silenciada = 0;
    } else if (iguales(s, "MANUAL")) {
        modo = MODO_MANUAL;
        armada = 0;
        alarma = 0;
        silenciada = 0;
    } else if (iguales(s, "ARMAR") && modo == MODO_MANUAL) {
        armada = 1;
    } else if (iguales(s, "DESARMAR") && modo == MODO_MANUAL) {
        armada = 0;
        alarma = 0;
        silenciada = 0;
    } else if (iguales(s, "SILENCIO")) {
        silenciada = 1;
    } else if (iguales(s, "FORZAR") && modo == MODO_MANUAL) {
        armada = 1;
        alarma_disparar("REMOTA");
    } else if (iguales(s, "RESET")) {
        alarma = 0;
        silenciada = 0;
    } else {
        uart_linea("ERROR:COMANDO_O_MODO");
        return;
    }
    uart_linea("OK");
    uart_estado();
    lcd_estado();
}

static void comando_recibir(void)
{
    char copia[sizeof(rx_linea)];
    uint8_t i, sreg = SREG;
    cli();
    if (!rx_lista) { SREG = sreg; return; }
    for (i = 0; i <= rx_largo; i++) copia[i] = rx_linea[i];
    rx_largo = 0;
    rx_lista = 0;
    SREG = sreg;
    comando_procesar(copia);
}

int main(void)
{
    uint8_t muestras = 0, ultima_muestra = 0, repetidas = 0;
    uint8_t ciclos_estado = 0;
    puertos_iniciar();
    adc_iniciar();
    uart_iniciar();
    lcd_iniciar();
    sei();
    uart_linea("ALARMA LISTA;COMANDOS: AUTO MANUAL ARMAR DESARMAR SILENCIO FORZAR RESET ESTADO LECTURA");
    lcd_estado();
    salidas_actualizar();
    for (;;) {
        comando_recibir();

        muestras = sensores_leer();
        if (muestras == ultima_muestra) {
            if (repetidas < 3) repetidas++;
        } else {
            repetidas = 0;
        }
        ultima_muestra = muestras;
        if (repetidas == 3) sensores_procesar(muestras);

        salidas_actualizar();
        if (++ciclos_estado == 100) {
            ciclos_estado = 0;
            uart_estado();
        }
        _delay_ms(20);
    }
}

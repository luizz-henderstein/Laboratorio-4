/* Laboratorio 4 - Problema A: cajero ATmega328P en C. */
#define F_CPU 16000000UL
#include <avr/io.h>
#include <avr/interrupt.h>
#include <util/delay.h>
#include <stdint.h>

#define CUENTAS 2
#define MARCA 0xA5
#define TAM_CUENTA 13
#define LCD_DIRECCION 0x27
static const char teclas[4][4] = {
    {'1','2','3','A'}, {'4','5','6','B'},
    {'7','8','9','C'}, {'*','0','#','D'}
};

/* ---------- EEPROM: marca, UID[4], PIN[4], saldo[4] por cuenta ---------- */
static uint16_t direccion_cuenta(uint8_t cuenta)
{
    return (uint16_t)cuenta * TAM_CUENTA;
}

static uint8_t ee_leer(uint16_t direccion)
{
    while (EECR & (1 << EEPE)) { }
    EEAR = direccion;
    EECR |= (1 << EERE);
    return EEDR;
}

static void ee_escribir(uint16_t direccion, uint8_t dato)
{
    uint8_t sreg;
    if (ee_leer(direccion) == dato) return;
    while (EECR & (1 << EEPE)) { }
    EEAR = direccion;
    EEDR = dato;
    sreg = SREG;
    cli();
    EECR |= (1 << EEMPE);
    EECR |= (1 << EEPE);
    SREG = sreg;
    while (EECR & (1 << EEPE)) { }
}

static uint8_t cuenta_existe(uint8_t cuenta)
{
    return ee_leer(direccion_cuenta(cuenta)) == MARCA;
}

static uint8_t primera_libre(void)
{
    uint8_t i;
    for (i = 0; i < CUENTAS; i++) {
        if (!cuenta_existe(i)) return i;
    }
    return CUENTAS;
}

static int8_t buscar_uid(const uint8_t uid[4])
{
    uint8_t i, j;
    for (i = 0; i < CUENTAS; i++) {
        if (!cuenta_existe(i)) continue;
        for (j = 0; j < 4; j++) {
            if (ee_leer(direccion_cuenta(i) + 1 + j) != uid[j]) break;
        }
        if (j == 4) return (int8_t)i;
    }
    return -1;
}

static uint8_t pin_correcto(uint8_t cuenta, const char pin[4])
{
    uint8_t j;
    for (j = 0; j < 4; j++) {
        if (ee_leer(direccion_cuenta(cuenta) + 5 + j) != (uint8_t)pin[j]) return 0;
    }
    return 1;
}

static uint32_t leer_saldo(uint8_t cuenta)
{
    uint8_t j;
    uint32_t saldo = 0;
    for (j = 0; j < 4; j++) {
        saldo |= (uint32_t)ee_leer(direccion_cuenta(cuenta) + 9 + j) << (8 * j);
    }
    return saldo;
}

static void guardar_saldo(uint8_t cuenta, uint32_t saldo)
{
    uint8_t j;
    for (j = 0; j < 4; j++) {
        ee_escribir(direccion_cuenta(cuenta) + 9 + j,
                    (uint8_t)(saldo >> (8 * j)));
    }
}

static void alta_cuenta(uint8_t cuenta, const uint8_t uid[4], const char pin[4])
{
    uint8_t j;
    uint16_t base = direccion_cuenta(cuenta);
    /* La marca se escribe al final para reconocer un alta interrumpida. */
    ee_escribir(base, 0);
    for (j = 0; j < 4; j++) ee_escribir(base + 1 + j, uid[j]);
    for (j = 0; j < 4; j++) ee_escribir(base + 5 + j, (uint8_t)pin[j]);
    guardar_saldo(cuenta, 0);
    ee_escribir(base, MARCA);
}

/* ---------- LCD 16x2 por TWI/I2C, PCF8574 ---------- */
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
    twi_dato(dato | 0x08); /* luz de fondo */
    twi_fin();
}

static void lcd_nibble(uint8_t nibble, uint8_t rs)
{
    uint8_t dato = (nibble & 0xF0) | rs;
    lcd_expansor(dato | 0x04); /* EN = 1 */
    _delay_us(1);
    lcd_expansor(dato);        /* EN = 0 */
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

static void lcd_texto(const char *texto)
{
    uint8_t i = 0;
    while (*texto && i < 16) {
        lcd_byte((uint8_t)*texto++, 1);
        i++;
    }
}

static void lcd_linea(uint8_t linea, const char *texto)
{
    lcd_comando(linea ? 0xC0 : 0x80);
    lcd_texto(texto);
}

static void lcd_mostrar(const char *arriba, const char *abajo)
{
    lcd_comando(0x01);
    lcd_linea(0, arriba);
    lcd_linea(1, abajo);
}

static void lcd_iniciar(void)
{
    DDRC &= ~((1 << PC4) | (1 << PC5));
    PORTC |= (1 << PC4) | (1 << PC5);
    TWSR = 0;
    TWBR = 72; /* 100 kHz a 16 MHz */
    TWCR = (1 << TWEN);
    _delay_ms(50);
    lcd_nibble(0x30, 0);
    _delay_ms(5);
    lcd_nibble(0x30, 0);
    _delay_us(150);
    lcd_nibble(0x30, 0);
    lcd_nibble(0x20, 0); /* modo 4 bits */
    lcd_comando(0x28);   /* 2 lineas */
    lcd_comando(0x0C);   /* pantalla encendida */
    lcd_comando(0x06);   /* cursor avanza */
    lcd_comando(0x01);
}

/* ---------- Teclado matricial: exploracion de filas y columnas ---------- */
static void teclado_iniciar(void)
{
    DDRD |= 0x3C;  /* PD2..PD5, filas R4..R1 */
    PORTD |= 0xFC; /* filas altas, pull-up en PD6/PD7 */
    DDRB &= ~((1 << PB0) | (1 << PB1));
    PORTB |= (1 << PB0) | (1 << PB1);
}

static char teclado_detectar(void)
{
    uint8_t fila, columna, columnas;
    for (fila = 0; fila < 4; fila++) {
        PORTD |= 0x3C;
        PORTD &= ~(1 << (5 - fila)); /* R1=PD5 ... R4=PD2 */
        _delay_us(10);
        columnas = ((PIND >> 6) & 0x03) | ((PINB & 0x03) << 2);
        for (columna = 0; columna < 4; columna++) {
            if (!(columnas & (1 << columna))) {
                PORTD |= 0x3C;
                return teclas[fila][columna];
            }
        }
    }
    PORTD |= 0x3C;
    return 0;
}

static char teclado_leer(void)
{
    char tecla;
    do { tecla = teclado_detectar(); } while (!tecla);
    _delay_ms(25); /* antirrebote */
    if (tecla != teclado_detectar()) return 0;
    while (teclado_detectar()) { } /* una accion por pulsacion */
    _delay_ms(25);
    return tecla;
}

/* Devuelve cantidad de digitos; 0 si se cancela con '*'. '#' confirma. */
static uint8_t ingresar(const char *titulo, char *digitos, uint8_t maximo,
                       uint8_t ocultar)
{
    uint8_t cantidad = 0, i;
    char linea[17];
    char tecla;
    lcd_mostrar(titulo, "# OK  * CANCELA");
    for (;;) {
        tecla = teclado_leer();
        if (!tecla) continue;
        if (tecla == '*') return 0;
        if (tecla == '#') {
            if (cantidad == maximo || (maximo == 5 && cantidad > 0)) return cantidad;
            continue;
        }
        if (tecla < '0' || tecla > '9' || cantidad >= maximo) continue;
        digitos[cantidad++] = tecla;
        for (i = 0; i < cantidad; i++) linea[i] = ocultar ? '*' : digitos[i];
        linea[cantidad] = 0;
        lcd_comando(0x01);
        lcd_linea(0, titulo);
        lcd_linea(1, linea);
    }
}

/* ---------- LED verde, LED rojo y buzzer activo ---------- */
static void exito(void)
{
    PORTC |= (1 << PC1);
    _delay_ms(1200);
    PORTC &= ~(1 << PC1);
}

static void error(void)
{
    PORTC |= (1 << PC2);
    _delay_ms(1200);
    PORTC &= ~(1 << PC2);
}

static void alarma(void)
{
    PORTC |= (1 << PC2) | (1 << PC3);
    _delay_ms(3000);
    PORTC &= ~((1 << PC2) | (1 << PC3));
}

static void texto_saldo(uint32_t saldo, char texto[17])
{
    char inverso[5];
    uint8_t i = 0, j = 7;
    texto[0] = 'S'; texto[1] = 'a'; texto[2] = 'l'; texto[3] = 'd';
    texto[4] = 'o'; texto[5] = ':'; texto[6] = ' ';
    do {
        inverso[i++] = '0' + (saldo % 10);
        saldo /= 10;
    } while (saldo && i < 5);
    while (i) texto[j++] = inverso[--i];
    texto[j] = 0;
}

/* A y B representan temporalmente las tarjetas mientras se integra el RC522. */
static const uint8_t tarjetas[2][4] = {
    {0x11, 0x22, 0x33, 0x44}, {0x55, 0x66, 0x77, 0x88}
};

int main(void)
{
    char tecla, pin[5], texto[17];
    uint8_t indice;
    int8_t cuenta;
    DDRC |= (1 << PC1) | (1 << PC2) | (1 << PC3);
    PORTC &= ~((1 << PC1) | (1 << PC2) | (1 << PC3));
    teclado_iniciar();
    lcd_iniciar();
    for (;;) {
        lcd_mostrar(primera_libre() < CUENTAS ? "Alta de cuenta" : "Cajero listo",
                    "A/B: elegir tarjeta");
        tecla = teclado_leer();
        if (tecla != 'A' && tecla != 'B') continue;
        indice = tecla == 'A' ? 0 : 1;
        cuenta = buscar_uid(tarjetas[indice]);
        if (primera_libre() < CUENTAS) {
            if (cuenta >= 0) { error(); continue; }
            if (ingresar("Nuevo PIN 4 dig", pin, 4, 1) != 4) continue;
            alta_cuenta(primera_libre(), tarjetas[indice], pin);
            lcd_mostrar("Cuenta registrada", "Saldo inicial 0");
            exito();
        } else {
            if (cuenta < 0) { error(); continue; }
            if (ingresar("Ingrese PIN", pin, 4, 1) != 4) continue;
            if (!pin_correcto((uint8_t)cuenta, pin)) { alarma(); continue; }
            texto_saldo(leer_saldo((uint8_t)cuenta), texto);
            lcd_mostrar(texto, "# para volver");
            exito();
            while (teclado_leer() != '#') { }
        }
    }
}

/* Laboratorio 4 - Problema A: cajero con ATmega328P, en C.
 * F_CPU = 16 MHz. Solo usa registros AVR, SPI, TWI, EEPROM y retardos vistos
 * en clase. LCD 16x2 con PCF8574 (direccion 0x27, como LCD.zip).
 */
#define F_CPU 16000000UL
#include <avr/io.h>
#include <avr/interrupt.h>
#include <util/delay.h>
#include <stdint.h>

#ifdef SIM_PICSIMLAB
#define CUENTAS 3
#else
#define CUENTAS 2
#endif
#define MARCA 0xA5
#define TAM_CUENTA 13
#define SALDO_MAX 99999UL
#define LCD_DIRECCION 0x27

/* Teclado, de arriba hacia abajo con el conector a la izquierda:
 * C4->D9, C3->D8, C2->D7, C1->D6, R1->D5, R2->D4, R3->D3, R4->D2.
 */
static const char teclas[4][4] = {
    {'1','2','3','A'}, {'4','5','6','B'},
    {'7','8','9','C'}, {'*','0','#','D'}
};

typedef enum {
    ESTADO_ALTA, ESTADO_TARJETA, ESTADO_PIN, ESTADO_MENU,
    ESTADO_SALDO, ESTADO_RETIRO, ESTADO_DEPOSITO, ESTADO_FIN
} Estado;

static uint8_t cuenta_actual;
static uint8_t intentos;

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

/* ---------- Identificacion: RC522 real o botones de la simulacion ---------- */
#ifdef SIM_PICSIMLAB
/* D0, D1 y D10 representan tres tarjetas distintas en PICSimLab. */
#define VersionReg 0x37

static void rc_iniciar(void)
{
    DDRD &= ~((1 << PD0) | (1 << PD1));
    PORTD |= (1 << PD0) | (1 << PD1); /* pull-up: boton pulsado = 0 */
    DDRB &= ~(1 << PB2);
    PORTB |= (1 << PB2);
}

static uint8_t rc_leer(uint8_t registro)
{
    (void)registro;
    return 0x92; /* lector representado, sin dispositivo SPI */
}

static uint8_t rc_uid(uint8_t uid[4])
{
    static const uint8_t tarjetas[3][4] = {
        {0x11, 0x22, 0x33, 0x44},
        {0x55, 0x66, 0x77, 0x88},
        {0x99, 0xAA, 0xBB, 0xCC}
    };
    uint8_t cuenta, i;
    if (!(PIND & (1 << PD0))) cuenta = 0;
    else if (!(PIND & (1 << PD1))) cuenta = 1;
    else if (!(PINB & (1 << PB2))) cuenta = 2;
    else return 0;
    for (i = 0; i < 4; i++) uid[i] = tarjetas[cuenta][i];
    return 1;
}

static void esperar_retirar_tarjeta(void)
{
    while (!(PIND & (1 << PD0)) || !(PIND & (1 << PD1)) ||
           !(PINB & (1 << PB2))) { }
    _delay_ms(100);
}

#else
/* SPI y RC522: REQA, anticollision CL1, UID de 4 bytes. */
#define CommandReg     0x01
#define ComIrqReg      0x04
#define ErrorReg       0x06
#define FIFODataReg    0x09
#define FIFOLevelReg   0x0A
#define ControlReg     0x0C
#define BitFramingReg  0x0D
#define ModeReg        0x11
#define TxControlReg   0x14
#define TxASKReg       0x15
#define RFCfgReg       0x26
#define TModeReg       0x2A
#define TPrescalerReg  0x2B
#define TReloadRegH    0x2C
#define TReloadRegL    0x2D
#define VersionReg     0x37

static uint8_t spi_transferir(uint8_t dato)
{
    SPDR = dato;
    while (!(SPSR & (1 << SPIF))) { }
    return SPDR;
}

static void rc_escribir(uint8_t registro, uint8_t dato)
{
    PORTB &= ~(1 << PB2);
    spi_transferir((registro << 1) & 0x7E);
    spi_transferir(dato);
    PORTB |= (1 << PB2);
}

static uint8_t rc_leer(uint8_t registro)
{
    uint8_t dato;
    PORTB &= ~(1 << PB2);
    spi_transferir(((registro << 1) & 0x7E) | 0x80);
    dato = spi_transferir(0);
    PORTB |= (1 << PB2);
    return dato;
}

static void rc_iniciar(void)
{
    DDRB |= (1 << PB2) | (1 << PB3) | (1 << PB5);
    DDRB &= ~(1 << PB4);
    PORTB |= (1 << PB2);
    SPCR = (1 << SPE) | (1 << MSTR) | (1 << SPR0); /* 1 MHz */
    SPSR = 0;
    DDRC |= (1 << PC0);
    PORTC &= ~(1 << PC0);
    _delay_ms(10);
    PORTC |= (1 << PC0);
    _delay_ms(50);
    rc_escribir(CommandReg, 0x0F); /* SoftReset */
    _delay_ms(50);
    rc_escribir(TModeReg, 0x8D);
    rc_escribir(TPrescalerReg, 0x3E);
    rc_escribir(TReloadRegH, 0);
    rc_escribir(TReloadRegL, 30);
    rc_escribir(TxASKReg, 0x40);
    rc_escribir(ModeReg, 0x3D);
    rc_escribir(RFCfgReg, 0x7F);
    rc_escribir(TxControlReg, rc_leer(TxControlReg) | 0x03);
}

static uint8_t rc_intercambiar(const uint8_t *enviar, uint8_t largo,
                              uint8_t ultimos_bits, uint8_t *recibir,
                              uint8_t maximo)
{
    uint16_t espera = 3000;
    uint8_t i, irq, recibidos;
    rc_escribir(CommandReg, 0x00); /* Idle */
    rc_escribir(ComIrqReg, 0x7F);
    rc_escribir(FIFOLevelReg, 0x80); /* vaciar FIFO */
    for (i = 0; i < largo; i++) rc_escribir(FIFODataReg, enviar[i]);
    rc_escribir(BitFramingReg, ultimos_bits);
    rc_escribir(CommandReg, 0x0C); /* Transceive */
    rc_escribir(BitFramingReg, ultimos_bits | 0x80); /* StartSend */
    do {
        irq = rc_leer(ComIrqReg);
        espera--;
    } while (espera && !(irq & 0x31)); /* RxIRq, IdleIRq o TimerIRq */
    rc_escribir(BitFramingReg, ultimos_bits);
    if (!espera || (irq & 0x01) || (rc_leer(ErrorReg) & 0x1F)) return 0;
    recibidos = rc_leer(FIFOLevelReg) & 0x7F;
    if (recibidos > maximo || (rc_leer(ControlReg) & 0x07)) return 0;
    for (i = 0; i < recibidos; i++) recibir[i] = rc_leer(FIFODataReg);
    return recibidos;
}

static uint8_t rc_uid(uint8_t uid[4])
{
    uint8_t reqa = 0x26;
    uint8_t anticollision[2] = {0x93, 0x20};
    uint8_t respuesta[5], i, bcc = 0;
    if (rc_intercambiar(&reqa, 1, 0x07, respuesta, 5) != 2) return 0;
    if (rc_intercambiar(anticollision, 2, 0, respuesta, 5) != 5) return 0;
    /* 0x88 indica que el UID continua en otro nivel; este laboratorio usa 4 bytes. */
    if (respuesta[0] == 0x88) return 0;
    for (i = 0; i < 4; i++) bcc ^= respuesta[i];
    if (bcc != respuesta[4]) return 0;
    for (i = 0; i < 4; i++) uid[i] = respuesta[i];
    return 1;
}

static void esperar_retirar_tarjeta(void)
{
    uint8_t uid[4], ausencias = 0;
    while (ausencias < 3) {
        /* Al interrumpir brevemente el campo RF, una tarjeta presente vuelve
         * a IDLE y responde a REQA. Sin esto, READY parece "tarjeta fuera". */
        rc_escribir(TxControlReg, rc_leer(TxControlReg) & ~0x03);
        _delay_ms(30);
        rc_escribir(TxControlReg, rc_leer(TxControlReg) | 0x03);
        _delay_ms(5);
        if (rc_uid(uid)) ausencias = 0;
        else ausencias++;
        _delay_ms(100);
    }
}
#endif

/* ---------- Pantallas y operaciones ---------- */
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

static uint32_t monto_de(char *digitos, uint8_t cantidad)
{
    uint8_t i;
    uint32_t monto = 0;
    for (i = 0; i < cantidad; i++) monto = monto * 10 + digitos[i] - '0';
    return monto;
}

int main(void)
{
    Estado estado;
    uint8_t uid[4], version, libre, cantidad;
    int8_t encontrada;
    char digitos[5], texto[17], tecla;
    uint32_t monto, saldo;

    DDRC |= (1 << PC1) | (1 << PC2) | (1 << PC3);
    PORTC &= ~((1 << PC1) | (1 << PC2) | (1 << PC3));
    teclado_iniciar();
    lcd_iniciar();
    rc_iniciar();
    version = rc_leer(VersionReg);
    if (version == 0 || version == 0xFF) {
        lcd_mostrar("RC522 no responde", "Revise conexion");
        for (;;) { error(); _delay_ms(400); }
    }

    estado = primera_libre() < CUENTAS ? ESTADO_ALTA : ESTADO_TARJETA;
    for (;;) {
        switch (estado) {
        case ESTADO_ALTA:
            libre = primera_libre();
            if (libre == CUENTAS) {
                lcd_mostrar(CUENTAS == 3 ? "3 cuentas listas" : "2 cuentas listas",
                            "Acerque tarjeta");
                estado = ESTADO_TARJETA;
                break;
            }
            texto[0] = 'A'; texto[1] = 'l'; texto[2] = 't'; texto[3] = 'a';
            texto[4] = ' '; texto[5] = 'c'; texto[6] = 'u'; texto[7] = 'e';
            texto[8] = 'n'; texto[9] = 't'; texto[10] = 'a'; texto[11] = ' ';
            texto[12] = '1' + libre; texto[13] = 0;
            lcd_mostrar(texto, "Acerque tarjeta");
            if (!rc_uid(uid)) { _delay_ms(100); break; }
            if (buscar_uid(uid) >= 0) {
                lcd_mostrar("Tarjeta repetida", "Use otra tarjeta");
                error();
                esperar_retirar_tarjeta();
                break;
            }
            cantidad = ingresar("Nuevo PIN 4 dig", digitos, 4, 1);
            if (cantidad == 4) {
                alta_cuenta(libre, uid, digitos);
                lcd_mostrar("Cuenta registrada", "Saldo inicial 0");
                exito();
            }
            esperar_retirar_tarjeta();
            break;

        case ESTADO_TARJETA:
            lcd_mostrar("CAJERO: Acerque", "su tarjeta RFID");
            while (!rc_uid(uid)) _delay_ms(100);
            encontrada = buscar_uid(uid);
            if (encontrada < 0) {
                lcd_mostrar("Tarjeta no", "registrada");
                error();
                esperar_retirar_tarjeta();
            } else {
                cuenta_actual = (uint8_t)encontrada;
                intentos = 0;
                estado = ESTADO_PIN;
            }
            break;

        case ESTADO_PIN:
            cantidad = ingresar("Ingrese PIN", digitos, 4, 1);
            if (cantidad == 0) {
                estado = ESTADO_FIN;
            } else if (pin_correcto(cuenta_actual, digitos)) {
                lcd_mostrar("PIN correcto", "Bienvenido");
                exito();
                estado = ESTADO_MENU;
            } else {
                intentos++;
                lcd_mostrar("PIN incorrecto", "Intente de nuevo");
                error();
                if (intentos >= 3) {
                    lcd_mostrar("3 intentos", "Acceso denegado");
                    alarma();
                    estado = ESTADO_FIN;
                }
            }
            break;

        case ESTADO_MENU:
            lcd_mostrar("A Saldo B Retiro", "C Depos D Salir");
            tecla = teclado_leer();
            if (tecla == 'A') estado = ESTADO_SALDO;
            if (tecla == 'B') estado = ESTADO_RETIRO;
            if (tecla == 'C') estado = ESTADO_DEPOSITO;
            if (tecla == 'D') estado = ESTADO_FIN;
            break;

        case ESTADO_SALDO:
            saldo = leer_saldo(cuenta_actual);
            texto_saldo(saldo, texto);
            lcd_mostrar(texto, "# para volver");
            do { tecla = teclado_leer(); } while (tecla != '#');
            estado = ESTADO_MENU;
            break;

        case ESTADO_RETIRO:
        case ESTADO_DEPOSITO:
            cantidad = ingresar(estado == ESTADO_RETIRO ? "Monto a retirar"
                                                       : "Monto a depositar",
                               digitos, 5, 0);
            if (cantidad == 0) { estado = ESTADO_MENU; break; }
            monto = monto_de(digitos, cantidad);
            saldo = leer_saldo(cuenta_actual);
            if (monto == 0 || monto > SALDO_MAX) {
                lcd_mostrar("Monto invalido", "Operacion anulada");
                error();
            } else if (estado == ESTADO_RETIRO && monto > saldo) {
                lcd_mostrar("Saldo insufic.", "Operacion anulada");
                error();
            } else if (estado == ESTADO_DEPOSITO && monto > SALDO_MAX - saldo) {
                lcd_mostrar("Limite de saldo", "Operacion anulada");
                error();
            } else {
                saldo = estado == ESTADO_RETIRO ? saldo - monto : saldo + monto;
                guardar_saldo(cuenta_actual, saldo);
                texto_saldo(saldo, texto);
                lcd_mostrar("Operacion exitosa", texto);
                exito();
                _delay_ms(1400);
            }
            estado = ESTADO_MENU;
            break;

        case ESTADO_FIN:
            lcd_mostrar("Fin de operacion", "Retire tarjeta");
            esperar_retirar_tarjeta();
            estado = ESTADO_TARJETA;
            break;
        }
    }
}

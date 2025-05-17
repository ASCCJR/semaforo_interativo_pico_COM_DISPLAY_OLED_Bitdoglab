#include <stdio.h>
#include <string.h> 
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/timer.h"
#include "pico/time.h"    // Necessário para repeating_timer, get_absolute_time
#include "hardware/pwm.h" // Necessário para PWM do buzzer
#include "hardware/i2c.h" // Necessário para I2C do OLED

// Biblioteca para o display SSD1306 (se usar dentro de uma pasta inc)
#include "inc/ssd1306.h"  

// ==============================================
// DEFINIÇÕES DE HARDWARE
// ==============================================
// Botão para pedestre
#define BUTTON_PIN 5      // Botão A (GPIO5)
// LED RGB para semáforo
#define LED_RED 13        // Pino do LED Vermelho
#define LED_GREEN 11      // Pino do LED Verde
#define LED_BLUE 12       // Pino do LED Azul (não utilizado ativamente neste semáforo)
#define BUZZER_PIN 21     // Buzzer A (GPIO21) com transistor para controle PWM

// --- Configurações do Display OLED e I2C (Conforme manual BitDogLab/OLED e código disponivel no github da bitdoglab) ---
const uint I2C_SDA_PIN = 14; // Pino SDA do I2C (GPIO14)
const uint I2C_SCL_PIN = 15; // Pino SCL do I2C (GPIO15)
#define I2C_OLED_PORT i2c1   // Usando a porta I2C1 (hardware i2c1)

// Frequência do alerta sonoro para pedestres
#define PEDESTRIAN_ALERT_FREQ 2000 // 2kHz

// ==============================================
// ESTADOS DO SEMÁFORO
// ==============================================
typedef enum {
    STATE_RED,
    STATE_GREEN,
    STATE_YELLOW,
    STATE_PEDESTRIAN_YELLOW // Amarelo especial para transição pedestre
} TrafficState;

// ==============================================
// VARIÁVEIS GLOBAIS
// ==============================================
volatile TrafficState current_state = STATE_RED; //  Estado inicial Vermelho
volatile bool button_pressed_flag = false;       // Flag para debounce lógico do botão
//Uso de Alarmes; Funções de alarme
alarm_id_t main_timer_alarm_id = 0;              // ID do timer principal

// Controle PWM do buzzer
uint buzzer_pwm_slice_num;
uint buzzer_pwm_chan;

// Temporização de sistemas com Temporizador do RP2040 (para pedestre)
struct repeating_timer pedestrian_countdown_timer_obj;
volatile int pedestrian_countdown_value;         // Variável para cronômetro regressivo
volatile bool pedestrian_walk_active = false;    // Indica estado de travessia de pedestre

// Globais para o Display OLED
uint8_t oled_display_buffer[ssd1306_buffer_length];
struct render_area oled_full_area;

// ==============================================
// FUNÇÕES DE CONTROLE DO DISPLAY OLED
// ==============================================

// Função para atualizar o display OLED
void oled_display_message(const char *line1, const char *line2, const char *line3) {
    // Limpa o buffer local do display
    memset(oled_display_buffer, 0, sizeof(oled_display_buffer));

    if (line1 && strlen(line1) > 0) {
        ssd1306_draw_string(oled_display_buffer, 0, 0, (char*)line1);  // Linha 1 (topo)
    }
    if (line2 && strlen(line2) > 0) {
        ssd1306_draw_string(oled_display_buffer, 0, 16, (char*)line2); // Linha 3 (y=16 pixels)
    }
    if (line3 && strlen(line3) > 0) {
        ssd1306_draw_string(oled_display_buffer, 0, 32, (char*)line3); // Linha 5 (y=32 pixels)
    }

    // Renderiza o buffer local na tela inteira
    render_on_display(oled_display_buffer, &oled_full_area);
}


// ==============================================
// CONTROLE DOS LEDS
// ==============================================
// LED RGB- Simula as cores do semáforo;
void set_red() {
    gpio_put(LED_RED, 1);
    gpio_put(LED_GREEN, 0);
    gpio_put(LED_BLUE, 0); // Garante que o azul está desligado
    printf("Sinal: Vermelho\n"); // Imprime "Sinal: nome da cor"
    oled_display_message("SEMAFORO", "VERMELHO", ""); // Atualiza OLED
}

void set_green() {
    gpio_put(LED_RED, 0);
    gpio_put(LED_GREEN, 1);
    gpio_put(LED_BLUE, 0);
    printf("Sinal: Verde\n");   //
    oled_display_message("SEMAFORO", "VERDE", ""); // Atualiza OLED
}

void set_yellow() {
    // Amarelo com LEDs Verde e Vermelho
    gpio_put(LED_RED, 1);
    gpio_put(LED_GREEN, 1);
    gpio_put(LED_BLUE, 0);
    printf("Sinal: Amarelo\n");
    oled_display_message("SEMAFORO", "AMARELO", ""); // Atualiza OLED
}

// ==============================================
// CONTROLE DO BUZZER PWM
// ==============================================
// Buzzer para alerta sonoro com controle de tons (PWM)
void setup_buzzer_pwm() {
    gpio_set_function(BUZZER_PIN, GPIO_FUNC_PWM);
    buzzer_pwm_slice_num = pwm_gpio_to_slice_num(BUZZER_PIN);
    buzzer_pwm_chan = pwm_gpio_to_channel(BUZZER_PIN);
    pwm_set_enabled(buzzer_pwm_slice_num, false); // Inicia desligado
}

void play_buzzer_tone(uint16_t freq) {
    if (freq == 0) {
        pwm_set_enabled(buzzer_pwm_slice_num, false);
        return;
    }
    pwm_set_clkdiv_int_frac(buzzer_pwm_slice_num, 100, 0); // Divisor de clock
    float wrap_float = 1250000.0f / freq; 
    if (wrap_float < 1.0f) wrap_float = 1.0f; 
    if (wrap_float > 65535.0f) { 
        pwm_set_enabled(buzzer_pwm_slice_num, false);
        return;
    }
    uint16_t wrap_value = (uint16_t)wrap_float;
    pwm_set_wrap(buzzer_pwm_slice_num, wrap_value);
    pwm_set_chan_level(buzzer_pwm_slice_num, buzzer_pwm_chan, wrap_value / 2); // 50% duty cycle
    pwm_set_enabled(buzzer_pwm_slice_num, true);
}

void stop_buzzer_tone() {
    pwm_set_enabled(buzzer_pwm_slice_num, false);
}

// ==============================================
// CALLBACK DO TEMPORIZADOR DO PEDESTRE
// ==============================================
bool pedestrian_countdown_timer_callback(struct repeating_timer *t) {
    char oled_countdown_line2[20]; // Buffer para texto do OLED

    if (pedestrian_countdown_value > 0) {
        // Monitor Serial para cronômetro regressivo
        printf("Pedestre: %d segundos restantes\n", pedestrian_countdown_value);
        sprintf(oled_countdown_line2, "Tempo: %02ds", pedestrian_countdown_value);
        oled_display_message("PEDESTRE", oled_countdown_line2, "ATRAVESSE");
        
        // Emissão de alerta sonoro para pedestres (intermitente com tom PWM)
        static bool play_the_tone_this_second = false;
        play_the_tone_this_second = !play_the_tone_this_second;
        if (play_the_tone_this_second) {
            play_buzzer_tone(PEDESTRIAN_ALERT_FREQ);
        } else {
            stop_buzzer_tone();
        }
    }
    pedestrian_countdown_value--;

    if (pedestrian_countdown_value < 0) {
        stop_buzzer_tone();
        pedestrian_walk_active = false;

        if(current_state == STATE_RED) { // Garante que mostra vermelho antes de transitar para verde
             oled_display_message("SEMAFORO", "VERMELHO", "");
        }
        return false; // Para o temporizador repetitivo
    }
    return true; // Continua
}

// ==============================================
// CALLBACK DO TIMER PRINCIPAL (MÁQUINA DE ESTADOS)
// ==============================================
int64_t main_timer_callback(alarm_id_t id, void *user_data) {

    if (pedestrian_walk_active && current_state == STATE_RED) { // Fim dos 10s do vermelho de pedestre
        cancel_repeating_timer(&pedestrian_countdown_timer_obj);
        stop_buzzer_tone();
        pedestrian_walk_active = false;

    } else if (pedestrian_walk_active && current_state != STATE_RED && current_state != STATE_PEDESTRIAN_YELLOW) {
        // Caso de segurança: se sairmos da fase de pedestre de forma inesperada
        cancel_repeating_timer(&pedestrian_countdown_timer_obj);
        stop_buzzer_tone();
        pedestrian_walk_active = false;
    }

    switch (current_state) {
        case STATE_RED:
            // Transição Vermelho (10s) -> Verde
            set_green(); // Atualiza LEDs, Serial e OLED
            current_state = STATE_GREEN;
            main_timer_alarm_id = add_alarm_in_ms(10000, main_timer_callback, NULL, false); // Verde por 10s
            break;
            
        case STATE_GREEN:
            // Transição Verde (10s) -> Amarelo
            set_yellow(); // Atualiza LEDs, Serial e OLED
            current_state = STATE_YELLOW;
            main_timer_alarm_id = add_alarm_in_ms(3000, main_timer_callback, NULL, false); // Amarelo por 3s
            break;
            
        case STATE_YELLOW: // Amarelo normal do ciclo dos carros
            // Repetir ciclo (Amarelo (3s) -> Vermelho)
            set_red(); // Atualiza LEDs, Serial e OLED
            current_state = STATE_RED;
            main_timer_alarm_id = add_alarm_in_ms(10000, main_timer_callback, NULL, false); // Vermelho por 10s
            break;
            
        case STATE_PEDESTRIAN_YELLOW: // Transição Amarelo de pedestre (3s)
            // ...para Vermelho, reiniciando o ciclo.
            set_red(); // Atualiza LEDs, Serial e OLED para VERMELHO
            current_state = STATE_RED; // Este vermelho é para pedestres e reinicia o ciclo
            
            pedestrian_walk_active = true;   // Ativa a fase de travessia do pedestre
            button_pressed_flag = false;     // Permite novo acionamento do botão

            pedestrian_countdown_value = 10; // Cronômetro de 10s para pedestre
            cancel_repeating_timer(&pedestrian_countdown_timer_obj); // Garante limpeza
            // Ativa cronômetro (OLED/Serial) e buzzer PWM para pedestre
            add_repeating_timer_ms(-1000, pedestrian_countdown_timer_callback, NULL, &pedestrian_countdown_timer_obj);
            stop_buzzer_tone(); // Garante que buzzer está desligado antes do callback do pedestre controlar

            main_timer_alarm_id = add_alarm_in_ms(10000, main_timer_callback, NULL, false); // 10s para este vermelho
            break;
    }
    return 0;
}

// ==============================================
// INTERRUPÇÃO DO BOTÃO (PEDESTRE)
// ==============================================
void button_isr(uint gpio, uint32_t events) {
    if (gpio == BUTTON_PIN && (events & GPIO_IRQ_EDGE_FALL)) { // Verifica o pino e o evento
        
        static absolute_time_t last_press = {0}; // Para debounce por tempo
        absolute_time_t now = get_absolute_time();

        // Debounce por tempo para filtrar bounces elétricos
        if (absolute_time_diff_us(last_press, now) > 200000) { // 200ms debounce
            last_press = now; // Atualiza o tempo do último toque válido

            // Só processa se não houver uma solicitação de pedestre já em andamento
            if (!button_pressed_flag) { 
                button_pressed_flag = true; // Marca que a solicitação foi recebida
                // Imprime "Botão de Pedestres acionado"
                printf("Botão de Pedestres acionado\n"); 
                
                // Cancela o timer atual do ciclo de carros, se houver um ativo
                if (main_timer_alarm_id != 0) {
                    cancel_alarm(main_timer_alarm_id);
                    main_timer_alarm_id = 0; // Invalida o ID
                }
                
                // Ao acionar o Botão... o semáforo deve acender amarelo e permanecer por 3s
                set_yellow(); // Atualiza LEDs, Serial e OLED para AMARELO
                current_state = STATE_PEDESTRIAN_YELLOW;
                main_timer_alarm_id = add_alarm_in_ms(3000, main_timer_callback, NULL, false);
            }
        }
    }
}

// ==============================================
// CONFIGURAÇÃO INICIAL (SETUP)
// ==============================================
void setup() {
    stdio_init_all(); // Para printf no Monitor Serial
    sleep_ms(5000);  // Aguarda 5s para permitir conexão ao Serial Monitor
   
    // Configura LEDs
    gpio_init(LED_RED); gpio_set_dir(LED_RED, GPIO_OUT);
    gpio_init(LED_GREEN); gpio_set_dir(LED_GREEN, GPIO_OUT);
    gpio_init(LED_BLUE); gpio_set_dir(LED_BLUE, GPIO_OUT); // Mesmo não usado, boa prática configurar
    
    // Configura Buzzer PWM
    setup_buzzer_pwm(); //
    stop_buzzer_tone(); // Garante que está desligado inicialmente

    // Configura Botão com interrupção
    gpio_init(BUTTON_PIN);
    gpio_set_dir(BUTTON_PIN, GPIO_IN);
    gpio_pull_up(BUTTON_PIN); //Configuração do botão
    // Configuração de interrupção para o botão
    gpio_set_irq_enabled_with_callback(BUTTON_PIN, GPIO_IRQ_EDGE_FALL, true, &button_isr);

    // --- Inicialização do Display OLED ---
    // Inicialização do I2C e Display OLED
    i2c_init(I2C_OLED_PORT, 400 * 1000); // 400kHz para I2C
    gpio_set_function(I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA_PIN); // Pull-ups externos são mais robustos, mas internos podem ajudar
    gpio_pull_up(I2C_SCL_PIN);

    ssd1306_init(); // Função da sua biblioteca para inicializar o display SSD1306

    // Configura a área de renderização para a tela cheia
    oled_full_area.start_column = 0;
    oled_full_area.end_column = ssd1306_width - 1; // Assumindo ssd1306_width definido em inc/ssd1306.h ou similar
    oled_full_area.start_page = 0;
    oled_full_area.end_page = (ssd1306_height / 8) - 1; // Assumindo ssd1306_height definido
    calculate_render_area_buffer_length(&oled_full_area); // Função da sua biblioteca

    oled_display_message("SEMAFORO", "INICIANDO...", ""); // Mensagem inicial no OLED
    printf("Semáforo Interativo Iniciado (com OLED)\n"); // Também no serial

    // Inicia ciclo do semáforo
    // Semáforo inicia em vermelho por 10 segundos
    set_red(); // Atualiza LEDs, Serial e OLED para VERMELHO
    current_state = STATE_RED;
    main_timer_alarm_id = add_alarm_in_ms(10000, main_timer_callback, NULL, false);
}

// ==============================================
// LOOP PRINCIPAL
// ==============================================
// Todo o processo ocorre FORA do Loop Infinito (CPU poupada)
int main() {
    setup();
    while (true) {
        tight_loop_contents(); // Aguarda interrupções, economiza energia
    }
    return 0; // Nunca alcançado
}
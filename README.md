# Master Control - ESP32

Este projeto é um sistema de controle integrado e interativo baseado no microcontrolador ESP32, desenvolvido utilizando o framework **ESP-IDF** via **PlatformIO**. Ele combina múltiplos periféricos de hardware com uma máquina de estados robusta, oferecendo recursos de leitura NFC, controle de servomotores, emissão de sinais infravermelhos (para LEDs), persistência de dados locais e conectividade remota via Wi-Fi e TCP/IP.

## ⚙️ Principais Funcionalidades

* **Ponto de Acesso Wi-Fi e Servidor TCP:** Cria uma rede local dedicada (AP) e escuta comandos externos através da porta TCP `3333`.
* **Leitura NFC/RFID:** Mapeia e autentica "Planetas" relacionando o UID de tags físicas a registros internos.
* **Controle de Servomotores:** Gerencia 4 servos independentes simultaneamente utilizando modulação PWM (LEDC).
* **Controle de Fita LED IR:** Emula um controle remoto enviando códigos hexadecimais via protocolo NEC nativo.
* **Teclado Analógico:** Lê até 5 botões em um único pino utilizando o conversor analógico-digital (ADC) para entrada de textos e navegação em menus.
* **Armazenamento Não-Volátil (SPIFFS):** Salva permanentemente o banco de dados de mapeamento entre Nomes de Planetas e UIDs de tags.

## 📌 Mapeamento de Hardware (Pinout)

| Módulo / Periférico | Pinos (GPIO) | Detalhes Técnicos |
| :--- | :--- | :--- |
| **Leitor NFC (RC522)** | `CS: 5`, `CLK: 18`, `MISO: 19`, `MOSI: 23`, `RST: 22` | Comunicação SPI (SPI3_HOST) |
| **Servomotores (1 a 4)** | `13`, `12`, `14`, `27` | LEDC Timer 0 (50Hz / 13-bit) |
| **Fita LED (Emissor IR)** | `2` | Saída Digital (Protocolo NEC - 32 bits) |
| **Matriz de Botões** | `32` | ADC1 Canal 4 (Divisor de tensão resistivo) |
| **Monitor Serial** | UART0 (Padrão ESP32) | Baud rate: `115200` |

## 🏗️ Estrutura do Software

O projeto está modularizado em componentes independentes para facilitar a manutenção e a escalabilidade:

* `wifi_tcp_mgr`: Gerencia a rede Wi-Fi AP (`SSID: TERRAPLANISMO`, `Senha: terraplanismo_adm`) e roteia as mensagens TCP.
* `nfc_reader`: Abstrai os comandos e registros do chip MFRC522 (Anti-colisão, requisição, leitura/escrita de blocos).
* `servo_ctrl`: Converte ângulos de 0° a 180° para Duty Cycles específicos de servomotores.
* `led_ctrl`: Gera com precisão de microssegundos o *Leader Pulse* e os bits do protocolo infravermelho.
* `button_matrix`: Mapeia faixas de tensão (ADC) para ações específicas (Anterior, Próxima, Confirmar, Apagar, Enter).
* `storage_mgr`: Controla o arquivo local `/spiffs/planetas.txt`.
* `system_logic`: O cérebro do projeto. Mantém a máquina de estados (Idle, Gravação, Busca NFC, Minigame, Teclado virtual).

## 💻 Interface de Comandos (TCP / Serial)

O sistema aceita diversos comandos para controle e testes de diagnóstico. Eles podem ser enviados via Serial ou via Socket TCP de um computador conectado à rede do ESP32.

### Comandos de Operação Principal
* `GRAVAR` - Inicia o modo de associação (Solicita nome, depois aguarda a tag NFC).
* `BUSCA:<nome>` - Aciona temporizador de 30s buscando o planeta especificado pela tag NFC.
* `TECLADO` / `FIM_TECLADO` - Ativa ou desativa a interface do teclado analógico enviando os eventos de botão/texto pela rede.
* `MINIGAME` / `FIM_MINIGAME` - Delega os inputs da matriz de botões diretamente para a rede TCP.
* `STATUS` - Retorna o estado atual da máquina e a saúde do leitor NFC.

### Comandos de Diagnóstico e Memória
* `LISTAR` - Imprime todos os registros salvos em `planetas.txt`.
* `LIMPAR` - Formata e apaga o banco de dados.
* `TESTE` - Roda uma rotina combinada (Servos + LEDs).
* `TESTE_SERVO` - Movimenta os 4 servos até os limites para testes mecânicos.
* `TESTE_LED` - Percorre um arco-íris na fita LED emitindo códigos IR.
* `TESTE_NFC` - Realiza um *self-test* do SPI e varredura contínua de tags por 10 a 15 segundos.
* `TESTE_BOTOES` - Exibe os valores *Raw* do ADC para calibração da matriz de resistores.

## 🚀 Como Compilar e Fazer o Upload

### Pré-requisitos
* **VS Code** com a extensão **PlatformIO** instalada.
* Framework configurado: **ESP-IDF** (`v5.5.0` ou compatível).

### Passos
1. Clone ou extraia o projeto no seu computador.
2. Abra a pasta do projeto no VS Code (o PlatformIO fará o reconhecimento do `platformio.ini`).
3. Certifique-se de que a partição customizada (`partitions.csv`) está na raiz, pois ela reserva 1MB para os arquivos em Flash (SPIFFS).
4. Utilize as tarefas do PlatformIO (ícone do alienígena na barra lateral):
   * Clique em **Build** para compilar.
   * Clique em **Upload** para gravar o firmware no ESP32.
   * Clique em **Upload File System image** (em *Platform -> Advanced*) para criar a partição SPIFFS inicial vazia, caso seja a primeira gravação.
5. Abra o **Serial Monitor** para interagir com o terminal embarcado.
